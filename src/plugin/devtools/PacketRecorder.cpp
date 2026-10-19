// PacketRecorder.cpp - .kcrec writer (format: PacketRecorder.h, readers:
// tools/devkit/replay). packet() runs on the NetLink thread for every ENet
// packet in or out, so the inactive path is ONE interlocked read and the
// active path is two buffered fwrites under a lock (no syscall per packet:
// the 64 KB stdio buffer is flushed at most once per second).

#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf / fopen are fine here; silence VC10 C4996

#include "PacketRecorder.h"
#include "../CoopLog.h"
#include "../../netproto/Wire.h"

#include <windows.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace coop {
namespace rec {

namespace {

// ---- on-disk layout (must match tools/devkit/replay/kcrec.h) ----------------
#pragma pack(push, 1)
struct FileHeader {
    char               magic[8];     // "KCREC01\0"
    u32                headerSize;   // sizeof(FileHeader): readers skip unknown tails
    u32                protocol;     // PROTOCOL_VERSION of the recording build
    u8                 isHost;
    u8                 pad[3];
    unsigned long long startWallMs;  // UTC ms since 1970
    char               version[32];  // plugin version string
    char               note[64];     // free text (who/where)
};
struct RecordHeader {
    u32 tMs;      // ms since the file was opened (monotonic, QPC)
    u8  dir;      // DIR_IN / DIR_OUT
    u8  peerId;   // see PacketRecorder.h
    u8  channel;
    u8  flags;    // bit0 = reliable
    u32 len;      // payload bytes that follow
};
#pragma pack(pop)

const unsigned int IO_BUF_BYTES   = 64 * 1024;
const DWORD        FLUSH_EVERY_MS = 1000;

// The lock must exist before the first packet()/arm() call; a namespace-scope
// object is constructed by the CRT at DLL load, before any thread can call in.
struct Lock {
    CRITICAL_SECTION cs;
    Lock()  { InitializeCriticalSection(&cs); }
    ~Lock() { DeleteCriticalSection(&cs); }
};
Lock g_lock;

struct Guard {
    Guard()  { EnterCriticalSection(&g_lock.cs); }
    ~Guard() { LeaveCriticalSection(&g_lock.cs); }
};

// Everything below is guarded by g_lock except g_active (interlocked) and
// g_pathMain (main thread only).
volatile LONG g_active = 0;           // 1 while g_fp is open
FILE*         g_fp = 0;
static char   g_ioBuf[IO_BUF_BYTES];  // stdio buffer for g_fp (one file at a time)
LARGE_INTEGER g_qpcFreq = { 0 };
LARGE_INTEGER g_qpcStart = { 0 };
DWORD         g_lastFlush = 0;
unsigned int  g_records = 0;
bool          g_armed = false;
bool          g_fromSession = false;  // opened by sessionBegin (vs startNow)
char          g_armDir[MAX_PATH] = { 0 };
char          g_version[32] = { 0 };
char          g_path[MAX_PATH] = { 0 };
char          g_pathMain[MAX_PATH] = { 0 };

void copyStr(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) src = "";
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void logf(const char* fmt, const char* a, unsigned int n) {
    char b[MAX_PATH + 96];
    _snprintf(b, sizeof(b) - 1, fmt, a ? a : "", n);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

unsigned long long utcMsNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // FILETIME = 100 ns ticks since 1601; shift to the Unix epoch.
    return t / 10000ULL - 11644473600000ULL;
}

u32 msSinceStart() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (g_qpcFreq.QuadPart == 0) return 0;
    return (u32)(((unsigned long long)(c.QuadPart - g_qpcStart.QuadPart) * 1000ULL) /
                 (unsigned long long)g_qpcFreq.QuadPart);
}

// CreateDirectoryA makes one level only; walk the path so a nested
// "recordings\\today" works on a fresh install.
void makeDirs(const char* dir) {
    char p[MAX_PATH];
    copyStr(p, sizeof(p), dir);
    size_t n = std::strlen(p);
    for (size_t i = 1; i <= n; ++i) {
        if (p[i] == '\\' || p[i] == '/' || p[i] == '\0') {
            char c = p[i];
            p[i] = '\0';
            // Skip bare drive roots ("C:"); everything else may already exist.
            if (!(i == 2 && p[1] == ':')) CreateDirectoryA(p, 0);
            p[i] = c;
        }
    }
}

// Caller holds g_lock.
void closeLocked(const char* why) {
    if (!g_fp) return;
    InterlockedExchange(&g_active, 0);
    std::fflush(g_fp);
    std::fclose(g_fp);
    g_fp = 0;
    char b[MAX_PATH + 96];
    _snprintf(b, sizeof(b) - 1, "[rec] closed %s (%u records, %s)", g_path, g_records, why);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    g_path[0] = '\0';
    g_records = 0;
    g_fromSession = false;
}

// Caller holds g_lock. Opens <dir>\YYYYMMDD_HHMMSS_<host|join>.kcrec (a
// "_2", "_3".. suffix when a reconnect lands in the same second).
bool openLocked(const char* dir, bool isHost, const char* version, const char* note) {
    closeLocked("superseded");
    if (!dir || !dir[0]) dir = ".";
    makeDirs(dir);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    for (int k = 1; k < 100; ++k) {
        char suffix[8] = "";
        if (k > 1) _snprintf(suffix, sizeof(suffix) - 1, "_%d", k);
        suffix[sizeof(suffix) - 1] = '\0';
        _snprintf(path, sizeof(path) - 1, "%s\\%04u%02u%02u_%02u%02u%02u_%s%s.kcrec",
                  dir, (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                  isHost ? "host" : "join", suffix);
        path[sizeof(path) - 1] = '\0';
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) break;
    }

    FILE* fp = std::fopen(path, "wb");
    if (!fp) { logf("[rec] cannot open %s (errno %u)", path, (unsigned)errno); return false; }
    std::setvbuf(fp, g_ioBuf, _IOFBF, sizeof(g_ioBuf));

    FileHeader h;
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, "KCREC01", 8); // includes the terminating NUL
    h.headerSize  = (u32)sizeof(FileHeader);
    h.protocol    = (u32)PROTOCOL_VERSION;
    h.isHost      = isHost ? 1 : 0;
    h.startWallMs = utcMsNow();
    copyStr(h.version, sizeof(h.version), version);
    copyStr(h.note, sizeof(h.note), note);
    if (std::fwrite(&h, sizeof(h), 1, fp) != 1) {
        std::fclose(fp);
        logf("[rec] header write failed: %s (%u)", path, (unsigned)sizeof(h));
        return false;
    }
    std::fflush(fp);

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_fp        = fp;
    g_records   = 0;
    g_lastFlush = GetTickCount();
    copyStr(g_path, sizeof(g_path), path);
    InterlockedExchange(&g_active, 1);
    logf("[rec] recording %s (protocol %u)", path, (unsigned)PROTOCOL_VERSION);
    return true;
}

} // namespace

// ---- main thread: arm / disarm ------------------------------------------------
void arm(const char* dir, const char* version) {
    Guard g;
    copyStr(g_armDir, sizeof(g_armDir), (dir && dir[0]) ? dir : ".");
    copyStr(g_version, sizeof(g_version), version);
    g_armed = true;
}

void disarm() {
    Guard g;
    g_armed = false;
    // A file opened by an armed session ends with the arming; a manual
    // startNow() recording is the command channel's to stop.
    if (g_fromSession) closeLocked("disarmed");
}

bool armed() {
    Guard g;
    return g_armed;
}

// ---- NetLink thread: session edges ----------------------------------------------
void sessionBegin(bool isHost, const char* note) {
    Guard g;
    if (!g_armed) return;
    if (openLocked(g_armDir, isHost, g_version, note)) g_fromSession = true;
}

void sessionEnd() {
    Guard g;
    closeLocked("session end");
}

// ---- main thread: command channel ---------------------------------------------
bool startNow(const char* dir, bool isHost, const char* version, const char* note) {
    Guard g;
    if (g_fp) return true; // already recording: keep the running file
    return openLocked(dir, isHost, version, note);
}

void stopNow() {
    Guard g;
    closeLocked("stopped");
}

// ---- any thread ---------------------------------------------------------------
bool active() {
    return InterlockedCompareExchange(&g_active, 0, 0) != 0;
}

void packet(unsigned char dir, unsigned int peerId, unsigned char channel,
            unsigned int flags, const void* data, unsigned int len) {
    if (!active()) return;
    Guard g;
    if (!g_fp) return; // closed between the check and the lock
    RecordHeader r;
    r.tMs     = msSinceStart();
    r.dir     = dir;
    r.peerId  = (u8)(peerId > 255u ? (unsigned)PEER_ANY : peerId);
    r.channel = channel;
    r.flags   = (u8)(flags & 1u);
    r.len     = data ? len : 0;
    bool ok = std::fwrite(&r, sizeof(r), 1, g_fp) == 1;
    if (ok && r.len > 0) ok = std::fwrite(data, 1, r.len, g_fp) == r.len;
    if (!ok) { closeLocked("write failed (disk full?)"); return; }
    ++g_records;
    DWORD now = GetTickCount();
    if (now - g_lastFlush >= FLUSH_EVERY_MS) {
        g_lastFlush = now;
        std::fflush(g_fp);
    }
}

const char* currentPath() {
    // Copy under the lock into a main-thread-only buffer so the caller can hold
    // the pointer while the net thread closes / reopens the file.
    Guard g;
    copyStr(g_pathMain, sizeof(g_pathMain), g_path);
    return g_pathMain;
}

} // namespace rec
} // namespace coop
