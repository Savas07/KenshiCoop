#include "FocusGuard.h"
#include "../CoopLog.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

namespace coop {
namespace focusguard {
namespace {

typedef BOOL (WINAPI* QueryImageNameFn)(HANDLE, DWORD, LPSTR, PDWORD);

const unsigned int MAX_LOG = 8;      // per kind of event, so a run's log stays readable
const DWORD        SLOW_MS = 200;    // z-order check period
const DWORD        HAND_MS = 100;    // minimum spacing of foreground hand-backs

bool             g_on = false;
HHOOK            g_hooks[2]   = { 0, 0 };
DWORD            g_threads[2] = { 0, 0 };   // threads a hook was attempted on
QueryImageNameFn g_queryName = 0;
HWND             g_returnTo = 0;            // last foreground window outside every Kenshi process
HWND             g_lastFg = 0;
bool             g_lastFgOurs = false;
DWORD            g_lastSlowMs = 0, g_lastHandMs = 0;
unsigned int     g_refused = 0, g_refusedLogged = 0;   // written by the hook (hooked threads only)
unsigned int     g_handedBack = 0, g_pushedDown = 0;

void logf(const char* fmt, unsigned long a, unsigned long b, unsigned long c, unsigned long d) {
    char buf[200];
    _snprintf(buf, sizeof(buf) - 1, fmt, a, b, c, d);
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}

bool isKenshiPid(DWORD pid) {
    if (pid == GetCurrentProcessId()) return true;
    if (!g_queryName || pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;  // e.g. a protected game: never a Kenshi test instance
    char path[MAX_PATH];
    DWORD n = MAX_PATH;
    bool kenshi = false;
    if (g_queryName(h, 0, path, &n)) {
        path[MAX_PATH - 1] = '\0';
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        kenshi = (_stricmp(base, "kenshi_x64.exe") == 0);
    }
    CloseHandle(h);
    return kenshi;
}

LRESULT CALLBACK cbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        ++g_refused;
        return 1;  // refuse: the window stays inactive, the foreground stays where it is
    }
    if (code == HCBT_CREATEWND) {
        CBT_CREATEWND* cw = reinterpret_cast<CBT_CREATEWND*>(lp);
        if (cw && cw->lpcs && !(cw->lpcs->style & WS_CHILD)) cw->hwndInsertAfter = HWND_BOTTOM;
    }
    return CallNextHookEx(0, code, wp, lp);
}

// Hook the calling thread once. Returns whether this call installed a hook.
bool hookThisThread() {
    DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < 2; ++i)
        if (g_threads[i] == tid) return false;
    for (int i = 0; i < 2; ++i) {
        if (g_threads[i] != 0) continue;
        g_threads[i] = tid;  // attempted, even if it fails: never retried per tick
        g_hooks[i] = SetWindowsHookExA(WH_CBT, cbtProc, 0, tid);
        return g_hooks[i] != 0;
    }
    return false;
}

struct PushCtx {
    HWND         below;
    unsigned int pushed;
};

// Put each visible top-level window of this process that sits above `below`
// at the bottom of the z-order, without activating anything.
BOOL CALLBACK pushProc(HWND w, LPARAM lp) {
    PushCtx* c = reinterpret_cast<PushCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    HWND x = w;
    for (int guard = 0; guard < 4096; ++guard) {
        x = GetWindow(x, GW_HWNDNEXT);
        if (!x) break;
        if (x == c->below) {
            SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            ++c->pushed;
            break;
        }
    }
    return TRUE;
}

BOOL CALLBACK countProc(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w)) ++*reinterpret_cast<unsigned int*>(lp);
    return TRUE;
}

} // namespace

void install(bool enabled) {
    if (!enabled || g_on) return;
    g_on = true;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_queryName = k32 ? reinterpret_cast<QueryImageNameFn>(GetProcAddress(k32, "QueryFullProcessImageNameA")) : 0;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && !isKenshiPid(pid)) g_returnTo = fg;
    bool hooked = hookThisThread();
    unsigned int own = 0;
    EnumWindows(countProc, reinterpret_cast<LPARAM>(&own));
    logf("[focus] guard ON (KENSHICOOP_NO_FOCUS): hook=%lu thread=%lu ownWindows=%lu returnTo=%lu",
         hooked ? 1ul : 0ul, (unsigned long)GetCurrentThreadId(), (unsigned long)own,
         g_returnTo ? 1ul : 0ul);
}

void tick() {
    if (!g_on) return;
    if (hookThisThread())
        logf("[focus] hooked thread %lu (refused so far=%lu)",
             (unsigned long)GetCurrentThreadId(), (unsigned long)g_refused, 0, 0);
    DWORD now = GetTickCount();
    HWND fg = GetForegroundWindow();
    if (fg != g_lastFg) {
        g_lastFg = fg;
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        g_lastFgOurs = fg && pid == GetCurrentProcessId();
        if (fg && !isKenshiPid(pid)) g_returnTo = fg;
    }
    if (g_lastFgOurs && g_returnTo && IsWindow(g_returnTo) && now - g_lastHandMs >= HAND_MS) {
        g_lastHandMs = now;
        // Allowed: this process holds the foreground, so it may give it away.
        BOOL ok = SetForegroundWindow(g_returnTo);
        g_lastFg = 0;  // re-read next call
        if (++g_handedBack <= MAX_LOG)
            logf("[focus] had the foreground; handed it back (ok=%lu n=%lu)",
                 ok ? 1ul : 0ul, (unsigned long)g_handedBack, 0, 0);
    }
    if (now - g_lastSlowMs < SLOW_MS) return;
    g_lastSlowMs = now;
    if (g_refused != g_refusedLogged && g_refusedLogged < MAX_LOG) {
        g_refusedLogged = g_refused < MAX_LOG ? g_refused : MAX_LOG;
        logf("[focus] refused %lu activation(s) so far", (unsigned long)g_refused, 0, 0, 0);
    }
    if (g_returnTo && IsWindow(g_returnTo)) {
        PushCtx c = { g_returnTo, 0 };
        EnumWindows(pushProc, reinterpret_cast<LPARAM>(&c));
        if (c.pushed) {
            g_pushedDown += c.pushed;
            if (g_pushedDown <= MAX_LOG)
                logf("[focus] window was above the user's; moved to the bottom (n=%lu)",
                     (unsigned long)g_pushedDown, 0, 0, 0);
        }
    }
}

} // namespace focusguard
} // namespace coop
