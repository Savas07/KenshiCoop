#include "HangWatch.h"
#include "CrashDump.h"
#include "../CoopLog.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace coop {
namespace hangwatch {
namespace {

const DWORD POLL_MS        = 1000;
const DWORD STALL_MS       = 20000;    // gameplay: this long without a tick gets sampled
const DWORD LOAD_STALL_MS  = 180000;   // the same, when the silence follows a load edge
const DWORD LOAD_WINDOW_MS = 30000;    // how soon after a load edge a silence counts as the load
const DWORD RESAMPLE_MS    = 40000;    // second sample: same frames = waiting, new frames = spinning
const int   MAX_FRAMES     = 40;

// ntdll's single-step unwinder. Resolved by name, like the fault tracer's
// RtlCaptureStackBackTrace, so nothing depends on the SDK's declaration.
typedef PVOID (NTAPI* VirtualUnwindFn)(DWORD, DWORD64, DWORD64, PVOID, PCONTEXT,
                                       PVOID*, PDWORD64, PVOID);

// One .pdata entry (RUNTIME_FUNCTION), spelled out for the same reason.
struct RtFn { DWORD begin, end, unwind; };

bool            g_installed = false;
bool            g_on        = false;
unsigned int  (*g_loadSerial)() = 0;
VirtualUnwindFn g_unwind    = 0;
DWORD64         g_exeBase   = 0;
char            g_exeLeaf[64] = { 0 };

// The watched thread: whichever runs the first gameplay tick.
bool            g_captureTried = false;
DWORD64         g_stackBase = 0;           // written before g_main is published
volatile HANDLE g_main      = 0;
DWORD           g_mainTid   = 0;

// Written by the main thread every tick, read by the watchdog thread.
volatile LONG         g_armed      = 0;
volatile DWORD        g_lastTickMs = 0;
volatile unsigned int g_tickSerial = 0;    // load serial as of the last gameplay tick
volatile DWORD        g_loadMarkMs = 0;    // tick that first saw a new load edge
volatile LONG         g_hasLoadMark = 0;
bool                  g_serialSeen = false;

// Base of the mapped image holding addr, or 0 when addr is not image memory.
DWORD64 imageBase(DWORD64 addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return 0;
    return (DWORD64)mbi.AllocationBase;
}

// One data directory of a mapped image, or 0. The headers sit in the image's
// first page, which every mapped image has.
const IMAGE_DATA_DIRECTORY* dataDir(DWORD64 base, int which) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x800)
        return 0;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        (DWORD)which >= nt->OptionalHeader.NumberOfRvaAndSizes)
        return 0;
    const IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[which];
    return (d->VirtualAddress && d->Size) ? d : 0;
}

// The .pdata entry covering pc, by binary search over the image's sorted
// exception directory - the lookup RtlLookupFunctionEntry does, minus its lock.
const RtFn* findFunction(DWORD64 base, DWORD64 pc) {
    const IMAGE_DATA_DIRECTORY* d = dataDir(base, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!d) return 0;
    const RtFn* t = (const RtFn*)(base + d->VirtualAddress);
    const DWORD rva = (DWORD)(pc - base);
    size_t lo = 0, hi = d->Size / sizeof(RtFn);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (rva < t[mid].begin)      hi = mid;
        else if (rva >= t[mid].end)  lo = mid + 1;
        else {
            const RtFn* f = &t[mid];
            if (f->unwind & 1) f = (const RtFn*)(base + (f->unwind & ~1u)); // indirect entry
            return f;
        }
    }
    return 0;
}

// Walk the suspended main thread's stack from its context into pcs.
//
// Lock-free on purpose. The usual walkers find functions through ntdll's
// inverted function table (RtlLookupFunctionEntry, behind an SRW lock) or the
// loader (dbghelp, module enumeration: the loader lock), and the main thread
// may be suspended holding either. Waiting on it then would turn a sample into
// a permanent hang of our own making. So the .pdata lookup is done by hand,
// RtlVirtualUnwind (computation over the context and stack memory) takes each
// step, and every frame must sit strictly higher on the main thread's own stack
// than the one before, which also bounds what the unwinder can read.
int walk(CONTEXT* c, DWORD64* pcs, int cap) {
    int n = 0;
    __try {
        while (n < cap && c->Rip) {
            const DWORD64 pc = c->Rip, sp = c->Rsp;
            pcs[n++] = pc;
            if (sp >= g_stackBase || (sp & 7)) break;
            // A return address can sit one past a no-return call that ends its
            // function, so frames above the top one are looked up a byte back.
            const DWORD64 at   = n == 1 ? pc : pc - 1;
            const DWORD64 base = imageBase(at);
            const RtFn* f = (base && g_unwind) ? findFunction(base, at) : 0;
            if (f) {
                PVOID data = 0; DWORD64 frame = 0;
                g_unwind(0 /* UNW_FLAG_NHANDLER */, base, pc, (PVOID)f, c, &data, &frame, 0);
            } else if (n == 1 && sp + 8 <= g_stackBase) {
                c->Rip = *(const DWORD64*)sp;   // a leaf: no frame, return address on top
                c->Rsp = sp + 8;
            } else {
                break;   // no unwind data above the top frame: cannot step past it
            }
            if (c->Rsp <= sp || c->Rsp > g_stackBase) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

// module+RVA for one frame. The module is named from its own export directory
// rather than through the loader, so naming cannot block either.
void nameFrame(DWORD64 pc, char* out, size_t cap) {
    const DWORD64 base = imageBase(pc);
    char name[64];
    name[0] = '\0';
    if (base) {
        __try {
            if (base == g_exeBase) {
                strncpy(name, g_exeLeaf, sizeof(name) - 1);
            } else {
                const IMAGE_DATA_DIRECTORY* d = dataDir(base, IMAGE_DIRECTORY_ENTRY_EXPORT);
                if (d) {
                    const IMAGE_EXPORT_DIRECTORY* e =
                        (const IMAGE_EXPORT_DIRECTORY*)(base + d->VirtualAddress);
                    if (e->Name) strncpy(name, (const char*)(base + e->Name), sizeof(name) - 1);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = '\0'; }
        name[sizeof(name) - 1] = '\0';
    }
    if (!base)
        _snprintf(out, cap - 1, "0x%llx (not in a module)", (unsigned long long)pc);
    else if (name[0])
        _snprintf(out, cap - 1, "%s+0x%llx", name, (unsigned long long)(pc - base));
    else
        _snprintf(out, cap - 1, "module@0x%llx+0x%llx", (unsigned long long)base,
                  (unsigned long long)(pc - base));
    out[cap - 1] = '\0';
}

void sample(DWORD silentMs, int nth, bool duringLoad) {
    DWORD64 pcs[MAX_FRAMES];
    int n = 0;
    DWORD err = 0;
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    // While the main thread is suspended: syscalls, reads and arithmetic only.
    // No logging, formatting or allocation - it may be holding the log's lock,
    // the CRT's or the heap's.
    if (SuspendThread(g_main) == (DWORD)-1) {
        err = GetLastError();
    } else {
        if (GetThreadContext(g_main, &c)) n = walk(&c, pcs, MAX_FRAMES);
        else err = GetLastError();
        ResumeThread(g_main);
    }

    char b[200];
    if (n == 0) {
        _snprintf(b, sizeof(b) - 1,
                  "[hang] main thread %lu silent %lu ms%s - cannot sample it (error %lu)",
                  (unsigned long)g_mainTid, (unsigned long)silentMs,
                  duringLoad ? " after a load edge" : "", (unsigned long)err);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        return;
    }
    _snprintf(b, sizeof(b) - 1,
              "[hang] main thread %lu silent %lu ms%s - stack sample %d of 2, %d frames:",
              (unsigned long)g_mainTid, (unsigned long)silentMs,
              duringLoad ? " after a load edge" : "", nth, n);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    for (int i = 0; i < n; ++i) {
        char where[112];
        nameFrame(pcs[i], where, sizeof(where));
        _snprintf(b, sizeof(b) - 1, "[hang]   #%02d %s", i, where);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// A load is under way if an edge arrived after the last gameplay tick (the
// mod's own load executes in the tick that requests it), or the tick that saw
// the latest edge is recent (the engine runs a requested load a few frames on).
bool loadUnderWay(DWORD lastTick) {
    if (g_loadSerial && g_loadSerial() != g_tickSerial) return true;
    return g_hasLoadMark && lastTick - g_loadMarkMs < LOAD_WINDOW_MS;
}

DWORD WINAPI watch(LPVOID) {
    int   reported   = 0;   // samples logged for the current silence (0..2)
    DWORD silentFrom = 0;   // tick stamp that silence started from
    for (;;) {
        Sleep(POLL_MS);
        const DWORD last = g_lastTickMs;
        const DWORD now  = GetTickCount();   // read after 'last', so now >= last
        if (reported && last != silentFrom) {
            char b[120];
            _snprintf(b, sizeof(b) - 1,
                      "[hang] main thread ticking again after about %lu ms silent",
                      (unsigned long)(last - silentFrom));
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            reported = 0;
        }
        if (!g_armed || !g_main) continue;
        const DWORD silent  = now - last;
        const bool  loading = loadUnderWay(last);
        const DWORD first   = loading ? LOAD_STALL_MS : STALL_MS;
        if (reported == 0 && silent >= first) {
            sample(silent, 1, loading);
            reported   = 1;
            silentFrom = last;
        } else if (reported == 1 && silent >= first + RESAMPLE_MS) {
            sample(silent, 2, loading);
            reported = 2;
        }
    }
}

// Runs on the first gameplay tick, so the thread opened is the game's main one.
void capture() {
    g_captureTried = true;
    g_mainTid   = GetCurrentThreadId();
    g_stackBase = (DWORD64)((NT_TIB*)NtCurrentTeb())->StackBase;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, g_mainTid);
    char b[120];
    _snprintf(b, sizeof(b) - 1, "[hang] watching main thread %lu (open=%d err=%lu)",
              (unsigned long)g_mainTid, h ? 1 : 0, (unsigned long)(h ? 0 : GetLastError()));
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    g_main = h;
}

} // namespace

void install(unsigned int (*loadSerial)()) {
    if (g_installed) return;
    g_installed = true;
    const char* e = getenv("KENSHICOOP_HANG_WATCH");
    if (e && e[0] == '0' && e[1] == '\0') {
        coop::logLine("[hang] main-thread watchdog off (KENSHICOOP_HANG_WATCH=0)");
        return;
    }
    g_loadSerial = loadSerial;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt) g_unwind = (VirtualUnwindFn)GetProcAddress(nt, "RtlVirtualUnwind");
    g_exeBase = (DWORD64)GetModuleHandleA(0);
    char full[MAX_PATH];
    full[0] = '\0';
    GetModuleFileNameA(0, full, sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';
    const char* leaf = full;
    for (const char* p = full; *p; ++p) if (*p == '\\' || *p == '/') leaf = p + 1;
    strncpy(g_exeLeaf, leaf, sizeof(g_exeLeaf) - 1);

    DWORD tid = 0;
    HANDLE t = CreateThread(0, 0, &watch, 0, 0, &tid);
    char b[240];
    if (!t) {
        _snprintf(b, sizeof(b) - 1, "[hang] main-thread watchdog NOT started (error %lu)",
                  (unsigned long)GetLastError());
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        return;
    }
    CloseHandle(t);
    // Its walk may touch a bad stack slot, handled on the spot; reporting that
    // through the VEH would take the log lock the suspended thread may hold.
    coop::crashdump::quietThread(tid);
    g_on = true;
    _snprintf(b, sizeof(b) - 1,
              "[hang] main-thread watchdog on (unwind=%d): stack sample after %lu ms "
              "without a gameplay tick, %lu ms after a load edge; KENSHICOOP_HANG_WATCH=0 "
              "turns it off",
              g_unwind ? 1 : 0, (unsigned long)STALL_MS, (unsigned long)LOAD_STALL_MS);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

void tick(bool gameplay) {
    if (!g_on) return;
    const DWORD now = GetTickCount();
    if (gameplay) {
        if (!g_captureTried) capture();
        if (g_loadSerial) {
            const unsigned int s = g_loadSerial();
            if (s != g_tickSerial || !g_serialSeen) {
                // The first sighting is the load that produced this world (it
                // already finished); only later edges start a load window.
                if (g_serialSeen) { g_loadMarkMs = now; g_hasLoadMark = 1; }
                g_serialSeen = true;
                g_tickSerial = s;
            }
        }
    }
    g_lastTickMs = now;
    g_armed = gameplay ? 1 : 0;
}

} // namespace hangwatch
} // namespace coop
