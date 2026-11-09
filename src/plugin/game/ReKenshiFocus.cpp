// ReKenshiFocus - see ReKenshiFocus.h.
//
// The vtable is located through RE_Kenshi's own MSVC RTTI, not a hard-coded
// offset, so an RE_Kenshi update that moves it still resolves (and one that
// drops the class simply leaves the guard uninstalled):
//   TypeDescriptor  : { pVFTable, spare, ".?AVWindowFocusListener@@" }
//   CompleteObjectLocator (x64): { signature=1, offset=0, cdOffset,
//                       typeDescriptor RVA, classDescriptor RVA, self RVA }
//   vtable[-1]      : the locator's address; vtable[5] is windowFocusChange
//                     (Ogre::WindowEventListener: dtor, windowMoved,
//                     windowResized, windowClosing, windowClosed,
//                     windowFocusChange).

#include "ReKenshiFocus.h"
#include "EngineInternal.h"
#include <kenshi/Globals.h> // ou

namespace coop {
namespace rekfocus {

namespace {

typedef void (*FocusChangeFn)(void* self, void* rw);

const unsigned int FOCUS_SLOT = 5;
const char kTypeName[] = ".?AVWindowFocusListener@@";

FocusChangeFn g_orig = 0;
unsigned long g_skipped = 0;

struct Section { const unsigned char* lo; const unsigned char* hi; bool exec; };

// Readable sections of the module at base (max 'cap'); 0 on a malformed header.
unsigned int sectionsOf(const unsigned char* base, Section* out, unsigned int cap) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    unsigned int n = 0;
    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections && n < cap; ++i) {
        if (!(s[i].Characteristics & IMAGE_SCN_MEM_READ) || s[i].Misc.VirtualSize == 0) continue;
        out[n].lo = base + s[i].VirtualAddress;
        out[n].hi = out[n].lo + s[i].Misc.VirtualSize;
        out[n].exec = (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        ++n;
    }
    return n;
}

// The vtable slot holding windowFocusChange, or 0. SEH-guarded by the caller.
void** findFocusSlot(const unsigned char* base) {
    Section sec[32];
    const unsigned int ns = sectionsOf(base, sec, 32);
    if (ns == 0) return 0;
    const size_t nameLen = sizeof(kTypeName); // with the terminator
    DWORD tdRva = 0;
    for (unsigned int i = 0; i < ns && !tdRva; ++i)
        for (const unsigned char* p = sec[i].lo; p + nameLen <= sec[i].hi; ++p)
            if (p[0] == '.' && memcmp(p, kTypeName, nameLen) == 0 && p - 0x10 >= base) {
                tdRva = (DWORD)(p - 0x10 - base);
                break;
            }
    if (!tdRva) return 0;
    const unsigned char* col = 0;
    for (unsigned int i = 0; i < ns && !col; ++i) {
        if (sec[i].exec) continue;
        for (const unsigned char* p = sec[i].lo; p + 24 <= sec[i].hi; p += 4) {
            const DWORD* d = (const DWORD*)p;
            if (d[0] == 1 && d[1] == 0 && d[3] == tdRva && d[5] == (DWORD)(p - base)) {
                col = p;
                break;
            }
        }
    }
    if (!col) return 0;
    for (unsigned int i = 0; i < ns; ++i) {
        if (sec[i].exec) continue;
        for (const unsigned char* p = sec[i].lo; p + 8 * (FOCUS_SLOT + 2) <= sec[i].hi; p += 8) {
            if (*(const unsigned char* const*)p != col) continue;
            void** vt = (void**)(p + 8);
            const unsigned char* fn = (const unsigned char*)vt[FOCUS_SLOT];
            for (unsigned int k = 0; k < ns; ++k)
                if (sec[k].exec && fn >= sec[k].lo && fn < sec[k].hi) return &vt[FOCUS_SLOT];
            return 0;
        }
    }
    return 0;
}

void** findFocusSlotSeh(const unsigned char* base) {
    __try { return findFocusSlot(base); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool playerPresent() {
    __try { return ou != 0 && ou->player != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void focusChange(void* self, void* rw) {
    if (!playerPresent()) {
        ++g_skipped;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "[rek-focus] window focus changed with no player - RE_Kenshi's "
                  "free-camera handler skipped (would fault; n=%lu)", g_skipped);
        b[sizeof(b) - 1] = '\0'; logLine(b);
        return;
    }
    g_orig(self, rw);
}

BOOL CALLBACK findOgreWindow(HWND h, LPARAM out) {
    char cls[32];
    if (GetClassNameA(h, cls, sizeof(cls)) && strcmp(cls, "OgreD3D11Wnd") == 0) {
        *(HWND*)out = h;
        return FALSE;
    }
    return TRUE;
}

} // namespace

void testFocusLossWithoutPlayer() {
    HWND w = 0;
    EnumThreadWindows(GetCurrentThreadId(), &findOgreWindow, (LPARAM)&w);
    if (!w || !ou || !ou->player) {
        logLine("[test] FOCUS-NOPLAYER skipped: no render window or no player to clear");
        return;
    }
    logLine("[test] FOCUS-NOPLAYER sending WM_ACTIVATE(WA_INACTIVE) with ou->player cleared");
    PlayerInterface* p = ou->player;
    ou->player = 0;
    SendMessageA(w, WM_ACTIVATE, WA_INACTIVE, 0);
    ou->player = p;
    SendMessageA(w, WM_ACTIVATE, WA_ACTIVE, 0);
    logLine("[test] FOCUS-NOPLAYER survived the focus loss");
}

bool install() {
    if (g_orig) return true;
    const unsigned char* base = (const unsigned char*)GetModuleHandleA("RE_Kenshi.dll");
    if (!base) { logLine("[rek-focus] RE_Kenshi.dll not loaded - focus guard not installed"); return false; }
    void** slot = findFocusSlotSeh(base);
    if (!slot) {
        logLine("[rek-focus] RE_Kenshi WindowFocusListener vtable not found - focus guard not installed");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        logLine("[rek-focus] VirtualProtect failed - focus guard not installed");
        return false;
    }
    g_orig = (FocusChangeFn)*slot;
    *slot = (void*)&focusChange;
    VirtualProtect(slot, sizeof(void*), old, &old);
    char b[200];
    _snprintf(b, sizeof(b) - 1,
              "[rek-focus] guarded RE_Kenshi WindowFocusListener::windowFocusChange "
              "(vtable slot RE_Kenshi.dll+0x%x, handler +0x%x)",
              (unsigned)((const unsigned char*)slot - base),
              (unsigned)((const unsigned char*)g_orig - base));
    b[sizeof(b) - 1] = '\0'; logLine(b);
    return true;
}

} // namespace rekfocus
} // namespace coop
