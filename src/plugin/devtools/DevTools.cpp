// DevTools.cpp - telemetry, command channel and desync watchdog (DevTools.h,
// docs/DEVTOOLS.md).
//
// Everything runs on the main (game) thread: the sockets are nonblocking and
// polled from tick()/idleTick(), so commands execute between engine ticks with
// no locking and no engine access from a foreign thread. Nothing here touches
// raw engine pointers; hand commands go through the SEH-guarded coop::engine API.

// winsock2 must precede windows.h (pulled in by NetLink.h via Replicator.h).
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <float.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "DevTools.h"
#include "PacketRecorder.h"
#include "../CoopLog.h"
#include "../game/Engine.h"
#include "../sync/Replicator.h"
#include "../../netproto/Wire.h"

// Not in every SDK's mstcpip.h: stop a UDP socket from reporting the ICMP
// port-unreachable of an earlier sendto as WSAECONNRESET on the next recvfrom
// (a dashboard that sent a command and quit would otherwise wedge polling).
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace coop {

// ---- Regime names ------------------------------------------------------------

const char* devRegimeName(int r) {
    static const char* const kNames[DR_COUNT] = {
        "none", "own", "paused", "down", "carried", "carrier", "furn",
        "crawl", "combat", "glide", "snap", "walk", "rest", "refused"
    };
    if (r < 0 || r >= DR_COUNT) return "?";
    return kNames[r];
}

namespace dev {

namespace {

// ---- Constants ----------------------------------------------------------------

const int   DEFAULT_TELEMETRY_PORT = 27850;
const int   DGRAM_MAX       = 1200;   // contract: one datagram <= 1200 bytes
const unsigned int BODY_PERIOD_MS   = 50;    // 20 Hz per body
const unsigned int GLOBAL_WORLD_MS  = 500;
const unsigned int GLOBAL_IDLE_MS   = 1000;
const unsigned int RING_SPAN_MS     = 6000;  // watchdog history per hand
const unsigned int RING_MIN_STEP_MS = 10;    // bounds memory at uncapped fps
const unsigned int RING_DROP_MS     = 10000; // hand absent this long -> ring dropped
const unsigned int TRIP_RATE_MS     = 10000; // one trip per hand+rule per 10 s
const int   CMD_PER_TICK    = 16;     // bound the work a command flood can cause

// ---- Watchdog rules -------------------------------------------------------------

enum Rule {
    R_MODEL_DETACHED = 0,
    R_NO_PHYSICS,
    R_FROZEN_COPY,
    R_BIG_GAP,
    R_STALE_GETUP,
    R_SLIDE_NO_ANIM,
    R_COUNT
};
const char* const kRuleNames[R_COUNT] = {
    "model_detached", "no_physics", "frozen_copy", "big_gap", "stale_getup", "slide_no_anim"
};

struct Sample {
    unsigned int t;
    float np[3], mesh[3], hk[3];
    float foot, gap, vlen;
    int   task;
    unsigned short bs, stask;
    unsigned char regime;
    bool  havePos, haveMesh, haveHk, smov, carried, carrying, own;
};

struct Ring {
    unsigned int hand[5];
    char  name[40];
    unsigned int lastSeen;
    std::deque<Sample> s;
    bool  active[R_COUNT];     // duration rule currently satisfied
    unsigned int since[R_COUNT];
    bool  tripped[R_COUNT];    // lastTrip valid
    unsigned int lastTrip[R_COUNT];
};

typedef unsigned long long HandKey;
typedef std::map<HandKey, Ring> RingMap;

// ---- Module state ---------------------------------------------------------------

bool        g_on = false;
DevConfig   g_cfg;
bool        g_isHost = false;
bool        g_overlayOn = false;
bool        g_watchdogOn = false;
bool        g_wsa = false;
SOCKET      g_tel = INVALID_SOCKET;
SOCKET      g_cmd = INVALID_SOCKET;
int         g_cmdPort = 0;
sockaddr_in g_telAddr;
LARGE_INTEGER g_qpf;
LARGE_INTEGER g_qp0;
unsigned int g_lastBodyMs = 0;
unsigned int g_lastGlobalMs = 0;
bool        g_sentBody = false;
bool        g_sentGlobal = false;
bool        g_hadWorld = false;
bool        g_keyWasDown = false;
DevGlobal   g_lastGlobal;
RingMap     g_rings;

// ---- Time / identity ------------------------------------------------------------

unsigned int nowMs() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (g_qpf.QuadPart <= 0) return 0;
    return (unsigned int)(((q.QuadPart - g_qp0.QuadPart) * 1000) / g_qpf.QuadPart);
}

const char* roleName() { return g_isHost ? "host" : "join"; }

HandKey keyOf(unsigned int index, unsigned int serial) {
    return ((HandKey)index << 32) | (HandKey)serial;
}

// ---- JSON writer ------------------------------------------------------------------
// Fixed-buffer builder: any overflow marks the buffer bad and the caller drops
// the datagram rather than sending truncated (unparseable) JSON.

struct JBuf {
    char* p;
    int   cap;
    int   len;
    bool  bad;
};

void jInit(JBuf& j, char* p, int cap) {
    j.p = p; j.cap = cap; j.len = 0; j.bad = false;
    if (cap > 0) p[0] = 0;
}

void jPut(JBuf& j, char c) {
    if (j.bad || j.len + 2 > j.cap) { j.bad = true; return; }
    j.p[j.len++] = c;
    j.p[j.len] = 0;
}

void jRaw(JBuf& j, const char* s) {
    if (j.bad) return;
    size_t n = strlen(s);
    if ((size_t)j.len + n + 1 > (size_t)j.cap) { j.bad = true; return; }
    memcpy(j.p + j.len, s, n);
    j.len += (int)n;
    j.p[j.len] = 0;
}

void jFmt(JBuf& j, const char* fmt, ...) {
    if (j.bad) return;
    int room = j.cap - j.len - 1;           // keep one byte for the terminator
    if (room <= 0) { j.bad = true; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(j.p + j.len, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0) { j.bad = true; j.p[j.len] = 0; return; }
    j.len += n;
    j.p[j.len] = 0;
}

// Length of a valid UTF-8 sequence starting at s (0 = invalid lead/continuation).
int utf8Len(const unsigned char* s, const unsigned char* end) {
    unsigned char c = s[0];
    int n;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    if (s + n > end) return 0;
    for (int i = 1; i < n; ++i)
        if ((s[i] & 0xC0) != 0x80) return 0;
    return n;
}

// Quoted, escaped string. Names come from the engine and may hold any byte:
// valid UTF-8 passes through, other high bytes are emitted as \u00XX (Latin-1)
// so the dashboard's strict UTF-8 JSON decode never fails. maxIn caps the
// input bytes consumed (keeps datagrams under the size limit).
void jStr(JBuf& j, const char* s, int maxIn) {
    jPut(j, '"');
    if (s) {
        const unsigned char* u = (const unsigned char*)s;
        size_t len = strlen(s);
        if (maxIn >= 0 && len > (size_t)maxIn) len = (size_t)maxIn;
        const unsigned char* end = u + len;
        while (u < end && !j.bad) {
            unsigned char c = *u;
            if (c == '"')       { jRaw(j, "\\\""); ++u; }
            else if (c == '\\') { jRaw(j, "\\\\"); ++u; }
            else if (c == '\n') { jRaw(j, "\\n"); ++u; }
            else if (c == '\r') { jRaw(j, "\\r"); ++u; }
            else if (c == '\t') { jRaw(j, "\\t"); ++u; }
            else if (c < 0x20 || c == 0x7F) { jFmt(j, "\\u%04x", (unsigned)c); ++u; }
            else if (c < 0x80)  { jPut(j, (char)c); ++u; }
            else {
                int n = utf8Len(u, end);
                if (n == 0) { jFmt(j, "\\u%04x", (unsigned)c); ++u; }
                else { for (int i = 0; i < n; ++i) jPut(j, (char)u[i]); u += n; }
            }
        }
    }
    jPut(j, '"');
}

bool finite3(const float* v) {
    return _finite(v[0]) && _finite(v[1]) && _finite(v[2]);
}

void jF1(JBuf& j, float v) {
    if (!_finite(v)) { jRaw(j, "null"); return; }
    jFmt(j, "%.1f", (double)v);
}

void jVec(JBuf& j, bool have, const float* v) {
    if (!have || !finite3(v)) { jRaw(j, "null"); return; }
    jFmt(j, "[%.1f,%.1f,%.1f]", (double)v[0], (double)v[1], (double)v[2]);
}

void jHead(JBuf& j, const char* kind, unsigned int t) {
    jRaw(j, "{\"k\":\""); jRaw(j, kind);
    jRaw(j, "\",\"role\":\""); jRaw(j, roleName());
    jFmt(j, "\",\"pid\":%lu,\"t\":%u", (unsigned long)GetCurrentProcessId(), t);
}

// ---- Datagram builders --------------------------------------------------------------

void buildBody(JBuf& j, const DevBody& b, unsigned int t) {
    jHead(j, "body", t);
    jFmt(j, ",\"hand\":\"%u,%u\",\"name\":", b.hand[3], b.hand[4]);
    jStr(j, b.name, (int)sizeof(b.name));
    jFmt(j, ",\"own\":%d,\"author\":%u,\"np\":", b.own ? 1 : 0, b.author);
    jVec(j, b.havePos, b.np);
    jRaw(j, ",\"mesh\":");  jVec(j, b.haveMesh, b.mesh);
    jRaw(j, ",\"foot\":");  jF1(j, b.foot);
    jRaw(j, ",\"hk\":");    jVec(j, b.haveHk, b.hk);
    jFmt(j, ",\"bs\":%u,\"task\":%d,\"carrying\":%d,\"carried\":%d,\"reg\":\"%s\",\"fresh\":%d,\"gap\":",
         (unsigned)b.bs, b.task, b.carrying ? 1 : 0, b.carried ? 1 : 0,
         devRegimeName(b.regime), b.fresh ? 1 : 0);
    jF1(j, b.gap);
    jRaw(j, ",\"vlen\":"); jF1(j, b.vlen);
    jRaw(j, ",\"cspd\":"); jF1(j, b.cSpeed);
    jFmt(j, ",\"smov\":%d,\"sbs\":%u,\"stask\":%u,\"glide\":%d,\"dest\":%d,\"newest\":",
         b.streamMoving ? 1 : 0, (unsigned)b.streamBs, (unsigned)b.streamTask,
         b.glide ? 1 : 0, b.haveDest ? 1 : 0);
    // Own bodies carry no drive state (zeros): null says "not applicable".
    jVec(j, !b.own, b.newest);
    jPut(j, '}');
}

void buildGlobalFields(JBuf& j, bool world) {
    const DevGlobal& g = g_lastGlobal;
    jRaw(j, "\"ver\":"); jStr(j, g_cfg.version.c_str(), 64);
    jFmt(j, ",\"cmdPort\":%d,\"host\":%d,\"localId\":%u,\"peers\":%u,\"net\":%d,\"world\":%d,\"slew\":",
         g_cmdPort, g_isHost ? 1 : 0, g.localId, g.peers, g.netRunning ? 1 : 0, world ? 1 : 0);
    jF1(j, g.timeSlew);
    jRaw(j, ",\"speed\":"); jF1(j, g.speed);
    jFmt(j, ",\"paused\":%d,\"overlay\":%d,\"watchdog\":%d,\"rec\":",
         g.paused ? 1 : 0, g_overlayOn ? 1 : 0, g_watchdogOn ? 1 : 0);
    const char* rp = rec::currentPath();
    jStr(j, rp ? rp : "", 600);
}

void buildGlobal(JBuf& j, bool world, unsigned int t) {
    jHead(j, "global", t);
    jPut(j, ',');
    buildGlobalFields(j, world);
    jPut(j, '}');
}

// ---- Sockets ------------------------------------------------------------------------------

void sendTel(const JBuf& j) {
    if (g_tel == INVALID_SOCKET || j.bad || j.len <= 0) return;
    // Nonblocking: a full send buffer (dashboard not reading) just drops data.
    sendto(g_tel, j.p, j.len, 0, (const sockaddr*)&g_telAddr, sizeof(g_telAddr));
}

void sendEvent(const char* ev, const char* rule, const char* hand,
               const char* msg, const char* dump) {
    char buf[DGRAM_MAX];
    JBuf j; jInit(j, buf, sizeof(buf));
    jHead(j, "event", nowMs());
    jRaw(j, ",\"ev\":");   jStr(j, ev, 16);
    jRaw(j, ",\"rule\":"); jStr(j, rule ? rule : "", 32);
    jRaw(j, ",\"hand\":"); jStr(j, hand ? hand : "", 32);
    jRaw(j, ",\"msg\":");  jStr(j, msg ? msg : "", 300);
    jRaw(j, ",\"dump\":"); jStr(j, dump ? dump : "", 300);
    jPut(j, '}');
    sendTel(j);
}

void closeSock(SOCKET& s) {
    if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
}

bool setNonBlocking(SOCKET s) {
    u_long nb = 1;
    return ioctlsocket(s, FIONBIO, &nb) == 0;
}

bool openSockets(int telPort) {
    if (!g_wsa) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        g_wsa = true;
    }
    memset(&g_telAddr, 0, sizeof(g_telAddr));
    g_telAddr.sin_family = AF_INET;
    g_telAddr.sin_port = htons((u_short)telPort);
    g_telAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    g_tel = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_tel == INVALID_SOCKET || !setNonBlocking(g_tel)) { closeSock(g_tel); return false; }

    g_cmd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_cmd == INVALID_SOCKET || !setNonBlocking(g_cmd)) { closeSock(g_cmd); closeSock(g_tel); return false; }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;                               // ephemeral; reported as cmdPort
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only: no remote control
    if (bind(g_cmd, (const sockaddr*)&a, sizeof(a)) != 0) {
        closeSock(g_cmd); closeSock(g_tel); return false;
    }
    int alen = sizeof(a);
    if (getsockname(g_cmd, (sockaddr*)&a, &alen) == 0) g_cmdPort = ntohs(a.sin_port);
    BOOL noReset = FALSE;
    DWORD ret = 0;
    WSAIoctl(g_cmd, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), NULL, 0, &ret, NULL, NULL);
    return true;
}

// ---- Watchdog ring buffer + dump files -----------------------------------------------------

void sampleOf(const DevBody& b, unsigned int t, Sample& s) {
    s.t = t;
    for (int i = 0; i < 3; ++i) { s.np[i] = b.np[i]; s.mesh[i] = b.mesh[i]; s.hk[i] = b.hk[i]; }
    s.foot = b.foot; s.gap = b.gap; s.vlen = b.vlen;
    s.task = b.task; s.bs = b.bs; s.stask = b.streamTask;
    s.regime = (unsigned char)((b.regime >= 0 && b.regime < DR_COUNT) ? b.regime : 0);
    s.havePos = b.havePos; s.haveMesh = b.haveMesh; s.haveHk = b.haveHk;
    s.smov = b.streamMoving; s.carried = b.carried; s.carrying = b.carrying; s.own = b.own;
}

void buildSample(JBuf& j, const Sample& s) {
    jFmt(j, "{\"t\":%u,\"np\":", s.t);
    jVec(j, s.havePos, s.np);
    jRaw(j, ",\"mesh\":"); jVec(j, s.haveMesh, s.mesh);
    jRaw(j, ",\"hk\":");   jVec(j, s.haveHk, s.hk);
    jRaw(j, ",\"foot\":"); jF1(j, s.foot);
    jFmt(j, ",\"reg\":\"%s\",\"gap\":", devRegimeName(s.regime));
    jF1(j, s.gap);
    jRaw(j, ",\"vlen\":"); jF1(j, s.vlen);
    jFmt(j, ",\"bs\":%u,\"task\":%d,\"own\":%d,\"smov\":%d,\"stask\":%u,\"carried\":%d,\"carrying\":%d}",
         (unsigned)s.bs, s.task, s.own ? 1 : 0, s.smov ? 1 : 0, (unsigned)s.stask,
         s.carried ? 1 : 0, s.carrying ? 1 : 0);
}

// Write <baseDir>\kc_dumps\YYYYMMDD_HHMMSS_<role>_<rule>_<index>_<serial>.json.
// Returns false (outPath empty) if the file could not be written.
bool writeDump(const Ring& r, const char* rule, const char* msg, char* outPath, int outCap) {
    outPath[0] = 0;
    std::string dir = g_cfg.baseDir.empty() ? std::string(".") : g_cfg.baseDir;
    dir += "\\kc_dumps";
    CreateDirectoryA(dir.c_str(), NULL);   // ERROR_ALREADY_EXISTS is fine
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH * 2];
    int n = _snprintf(path, sizeof(path) - 1, "%s\\%04u%02u%02u_%02u%02u%02u_%s_%s_%u_%u.json",
                      dir.c_str(), (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                      (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                      roleName(), rule, r.hand[3], r.hand[4]);
    if (n < 0) return false;
    path[sizeof(path) - 1] = 0;
    FILE* f = 0;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    char buf[2048];
    JBuf j; jInit(j, buf, sizeof(buf));
    jRaw(j, "{\"rule\":"); jStr(j, rule, 32);
    jRaw(j, ",\"role\":"); jStr(j, roleName(), 8);
    jRaw(j, ",\"version\":"); jStr(j, g_cfg.version.c_str(), 64);
    jFmt(j, ",\"hand\":\"%u,%u\",\"hand5\":[%u,%u,%u,%u,%u],\"name\":",
         r.hand[3], r.hand[4], r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4]);
    jStr(j, r.name, (int)sizeof(r.name));
    jFmt(j, ",\"t\":%u,\"msg\":", nowMs());
    jStr(j, msg ? msg : "", 400);
    jRaw(j, ",\"global\":{");
    buildGlobalFields(j, g_hadWorld);
    jRaw(j, "},\n\"samples\":[\n");
    fwrite(j.p, 1, (size_t)j.len, f);

    for (size_t i = 0; i < r.s.size(); ++i) {
        char sb[640];
        JBuf js; jInit(js, sb, sizeof(sb));
        buildSample(js, r.s[i]);
        if (i + 1 < r.s.size()) jPut(js, ',');
        jPut(js, '\n');
        if (!js.bad) fwrite(js.p, 1, (size_t)js.len, f);
    }
    fputs("]}\n", f);
    bool ok = ferror(f) == 0;
    fclose(f);
    if (!ok) return false;
    _snprintf(outPath, (size_t)outCap - 1, "%s", path);
    outPath[outCap - 1] = 0;
    return true;
}

// Duration-gated condition: true once cond has held continuously for need ms.
bool held(Ring& r, int rule, bool cond, unsigned int now, unsigned int need, unsigned int& dur) {
    if (!cond) { r.active[rule] = false; dur = 0; return false; }
    if (!r.active[rule]) { r.active[rule] = true; r.since[rule] = now; }
    dur = now - r.since[rule];
    return dur >= need;
}

// Index of the newest sample at or before (now - w): the window [i..end] then
// spans at least w ms. -1 when the ring is younger than w.
int windowStart(const Ring& r, unsigned int now, unsigned int w) {
    if (now < w) return -1;
    unsigned int edge = now - w;
    for (int i = (int)r.s.size() - 1; i >= 0; --i)
        if (r.s[(size_t)i].t <= edge) return i;
    return -1;
}

float dist3(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Kenshi (Ogre) is Y-up: horizontal = x/z.
float distXZ(const float* a, const float* b) {
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

bool isGetupTask(int t) { return t == 28 || t == 65 || t == 66; }

// Log lines carry engine names and free command text: a control byte (e.g. a
// newline) would split or corrupt the line-oriented log, so blank them out.
void logPlain(char* line) {
    for (char* p = line; *p; ++p)
        if ((unsigned char)*p < 0x20) *p = '?';
    logLine(line);
}

void trip(Ring& r, int rule, unsigned int now, const char* detail) {
    if (r.tripped[rule] && now - r.lastTrip[rule] < TRIP_RATE_MS) return;
    r.tripped[rule] = true;
    r.lastTrip[rule] = now;
    char dump[MAX_PATH * 2];
    writeDump(r, kRuleNames[rule], detail, dump, sizeof(dump));
    char line[768];
    _snprintf(line, sizeof(line) - 1, "[watchdog] TRIP rule=%s hand=%u,%u name='%s' %s%s%s",
              kRuleNames[rule], r.hand[3], r.hand[4], r.name, detail,
              dump[0] ? " dump=" : " dump=FAILED", dump);
    line[sizeof(line) - 1] = 0;
    logPlain(line);
    char hand[32];
    _snprintf(hand, sizeof(hand) - 1, "%u,%u", r.hand[3], r.hand[4]);
    hand[sizeof(hand) - 1] = 0;
    sendEvent("trip", kRuleNames[rule], hand, detail, dump);
}

void evaluate(Ring& r, unsigned int now) {
    if (r.s.empty()) return;
    const Sample& c = r.s.back();
    char d[256];
    unsigned int dur = 0;

    // R1: the rendered model is not where the character is (nameplate moved,
    // skeleton stayed) - the classic "frozen model". Own bodies included.
    {
        float sep = (c.haveMesh && c.havePos) ? distXZ(c.mesh, c.np) : 0.0f;
        bool cond = !c.carried && c.haveMesh && c.havePos && sep > 15.0f;
        if (held(r, R_MODEL_DETACHED, cond, now, 500, dur)) {
            _snprintf(d, sizeof(d) - 1, "sep=%.1f np=(%.1f,%.1f) mesh=(%.1f,%.1f) held=%ums reg=%s",
                      (double)sep, (double)c.np[0], (double)c.np[2],
                      (double)c.mesh[0], (double)c.mesh[2], dur, devRegimeName(c.regime));
            d[sizeof(d) - 1] = 0;
            trip(r, R_MODEL_DETACHED, now, d);
        }
    }
    if (c.own) {
        // Every other rule is about driven copies; keep their timers clear.
        for (int k = R_NO_PHYSICS; k < R_COUNT; ++k) r.active[k] = false;
        return;
    }

    // R2: a standing, unattached driven copy without a Havok character: placement
    // can only move the nameplate, the model cannot follow.
    {
        bool cond = !c.haveHk && !c.carried && !c.carrying && !bodyIsDown(c.bs) && c.regime != DR_FURN;
        if (held(r, R_NO_PHYSICS, cond, now, 1000, dur)) {
            _snprintf(d, sizeof(d) - 1, "bs=0x%04x task=%d reg=%s held=%ums",
                      (unsigned)c.bs, c.task, devRegimeName(c.regime), dur);
            d[sizeof(d) - 1] = 0;
            trip(r, R_NO_PHYSICS, now, d);
        }
    }

    // R3: the owner is moving but our copy has not. The stream condition must
    // hold across the whole window, otherwise every start-of-walk (interp delay)
    // would look frozen.
    {
        int i0 = windowStart(r, now, 700);
        if (i0 >= 0 && c.smov && c.vlen > 30.0f && c.havePos) {
            bool all = true;
            float moved = 0.0f;
            for (size_t i = (size_t)i0; i < r.s.size(); ++i) {
                const Sample& s = r.s[i];
                if (!s.smov || !(s.vlen > 30.0f) || !s.havePos) { all = false; break; }
                float m = dist3(s.np, r.s[(size_t)i0].np);
                if (m > moved) moved = m;
            }
            if (all && moved < 1.0f) {
                _snprintf(d, sizeof(d) - 1, "vlen=%.1f moved=%.2f gap=%.1f reg=%s window=%ums",
                          (double)c.vlen, (double)moved, (double)c.gap, devRegimeName(c.regime),
                          now - r.s[(size_t)i0].t);
                d[sizeof(d) - 1] = 0;
                trip(r, R_FROZEN_COPY, now, d);
            }
        }
    }

    // R4: copy far from the newest stream sample.
    if (c.gap > 100.0f) {
        _snprintf(d, sizeof(d) - 1, "gap=%.1f vlen=%.1f reg=%s", (double)c.gap,
                  (double)c.vlen, devRegimeName(c.regime));
        d[sizeof(d) - 1] = 0;
        trip(r, R_BIG_GAP, now, d);
    }

    // R5: our copy is stuck in a get-up task while the owner has moved on.
    {
        bool cond = isGetupTask(c.task) && c.smov && !isGetupTask((int)c.stask);
        if (held(r, R_STALE_GETUP, cond, now, 1000, dur)) {
            _snprintf(d, sizeof(d) - 1, "task=%d stask=%u vlen=%.1f held=%ums",
                      c.task, (unsigned)c.stask, (double)c.vlen, dur);
            d[sizeof(d) - 1] = 0;
            trip(r, R_STALE_GETUP, now, d);
        }
    }

    // R6: moving copy whose feet do not swing = no gait clip (sliding statue).
    // Carried bodies move rigidly on the carrier's shoulder by design, and a
    // down/KO body is co-located by placement while it lies still (in-game:
    // tripped at reg=down during a carry KO): both excluded.
    if (c.haveMesh && c.havePos && !c.carried && !coop::bodyIsDown(c.bs) &&
        c.regime != DR_DOWN && c.regime != DR_CARRIED) {
        int i0 = windowStart(r, now, 1000);
        if (i0 >= 0) {
            const Sample& a = r.s[(size_t)i0];
            unsigned int span = c.t - a.t;
            // A loading hitch stretches the window: skip rather than misjudge.
            if (a.havePos && span >= 1000 && span <= 1500) {
                float speed = dist3(c.np, a.np) * 1000.0f / (float)span;
                double sum = 0.0, sum2 = 0.0;
                int n = 0;
                for (size_t i = (size_t)i0; i < r.s.size(); ++i) {
                    float f = r.s[i].foot;
                    if (f < 0.0f || !_finite(f)) continue;
                    sum += f; sum2 += (double)f * f; ++n;
                }
                if (n >= 5 && speed > 20.0f) {
                    double mean = sum / n;
                    double var = sum2 / n - mean * mean;
                    double sd = var > 0.0 ? sqrt(var) : 0.0;
                    if (sd < 0.15) {
                        _snprintf(d, sizeof(d) - 1, "speed=%.1f footStd=%.3f footMean=%.2f n=%d reg=%s",
                                  (double)speed, sd, mean, n, devRegimeName(c.regime));
                        d[sizeof(d) - 1] = 0;
                        trip(r, R_SLIDE_NO_ANIM, now, d);
                    }
                }
            }
        }
    }
}

// Record this tick's samples (always while devtools is on, so `dump <hand>`
// works with the watchdog off) and evaluate rules when the watchdog is on.
void watchdogTick(const std::vector<DevBody>& bodies, unsigned int now) {
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const DevBody& b = bodies[bi];
        HandKey k = keyOf(b.hand[3], b.hand[4]);
        RingMap::iterator it = g_rings.find(k);
        if (it == g_rings.end()) {
            Ring fresh;
            memset(fresh.hand, 0, sizeof(fresh.hand));
            fresh.name[0] = 0;
            fresh.lastSeen = now;
            for (int i = 0; i < R_COUNT; ++i) {
                fresh.active[i] = false; fresh.since[i] = 0;
                fresh.tripped[i] = false; fresh.lastTrip[i] = 0;
            }
            it = g_rings.insert(std::make_pair(k, fresh)).first;
        }
        Ring& r = it->second;
        memcpy(r.hand, b.hand, sizeof(r.hand));
        memcpy(r.name, b.name, sizeof(r.name));
        r.name[sizeof(r.name) - 1] = 0;
        r.lastSeen = now;
        if (!r.s.empty() && now - r.s.back().t < RING_MIN_STEP_MS) continue;
        Sample s;
        sampleOf(b, now, s);
        r.s.push_back(s);
        while (!r.s.empty() && now - r.s.front().t > RING_SPAN_MS) r.s.pop_front();
        if (g_watchdogOn) evaluate(r, now);
    }
}

void pruneRings(unsigned int now) {
    RingMap::iterator it = g_rings.begin();
    while (it != g_rings.end()) {
        if (now - it->second.lastSeen > RING_DROP_MS) g_rings.erase(it++);
        else ++it;
    }
}

void resetRuleTimers() {
    for (RingMap::iterator it = g_rings.begin(); it != g_rings.end(); ++it)
        for (int i = 0; i < R_COUNT; ++i) it->second.active[i] = false;
}

// ---- Command channel ----------------------------------------------------------------------

const char* const kHelp =
    "ping | help | restore <i,s> | endaction <i,s> | knockdown <i,s> <0|1> | halt <i,s> | "
    "move <i,s> <dx> <dz> | glide <on|off> | overlay <on|off> | watchdog <on|off> | dump <i,s> | "
    "mark <text> | record <start|stop>";

struct CmdCtx {
    GameWorld* gw;                          // null from idleTick
    Replicator* repl;                       // null from idleTick
    const std::vector<DevBody>* bodies;     // null from idleTick
};

void reply(const sockaddr_in& to, const char* cmd, bool ok, const char* msg) {
    char buf[DGRAM_MAX];
    JBuf j; jInit(j, buf, sizeof(buf));
    jHead(j, "reply", nowMs());
    jRaw(j, ",\"cmd\":"); jStr(j, cmd, 32);
    jFmt(j, ",\"ok\":%d,\"msg\":", ok ? 1 : 0);
    jStr(j, msg, 900);
    jPut(j, '}');
    if (!j.bad && g_cmd != INVALID_SOCKET)
        sendto(g_cmd, j.p, j.len, 0, (const sockaddr*)&to, sizeof(to));
}

// Parse "on"/"off"/"1"/"0".
bool parseOnOff(const char* s, bool& v) {
    if (!s) return false;
    if (_stricmp(s, "on") == 0 || strcmp(s, "1") == 0)  { v = true;  return true; }
    if (_stricmp(s, "off") == 0 || strcmp(s, "0") == 0) { v = false; return true; }
    return false;
}

// Split off the next whitespace-delimited token; returns the token (in place)
// and advances *rest past it. Empty string when none.
char* nextTok(char** rest) {
    char* s = *rest;
    while (*s == ' ' || *s == '\t') ++s;
    char* t = s;
    while (*s && *s != ' ' && *s != '\t') ++s;
    if (*s) *s++ = 0;
    while (*s == ' ' || *s == '\t') ++s;
    *rest = s;
    return t;
}

// Find this tick's DevBody for "index,serial" and resolve it to a Character.
// The DevBody supplies the full 5-part hand (type/container fields).
Character* resolveHandArg(const char* arg, const CmdCtx& ctx, const DevBody** outB,
                          char* err, int errCap) {
    *outB = 0;
    unsigned int idx = 0, ser = 0;
    if (!arg || sscanf_s(arg, "%u,%u", &idx, &ser) != 2) {
        _snprintf(err, (size_t)errCap - 1, "bad hand '%s' (want index,serial)", arg ? arg : "");
        err[errCap - 1] = 0;
        return 0;
    }
    const DevBody* found = 0;
    for (size_t i = 0; i < ctx.bodies->size(); ++i) {
        const DevBody& b = (*ctx.bodies)[i];
        if (b.hand[3] == idx && b.hand[4] == ser) { found = &b; break; }
    }
    if (!found) {
        _snprintf(err, (size_t)errCap - 1, "hand %u,%u not in this tick's squad snapshot", idx, ser);
        err[errCap - 1] = 0;
        return 0;
    }
    *outB = found;
    const unsigned int* h = found->hand;
    Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
    if (!c) {
        _snprintf(err, (size_t)errCap - 1, "hand %u,%u did not resolve", idx, ser);
        err[errCap - 1] = 0;
    }
    return c;
}

void execCommand(char* text, const sockaddr_in& from, const CmdCtx& ctx) {
    // Trim trailing whitespace / newlines (netcat-style senders append \n).
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ' || text[n - 1] == '\t'))
        text[--n] = 0;
    char original[300];
    _snprintf(original, sizeof(original) - 1, "%s", text);
    original[sizeof(original) - 1] = 0;

    char* rest = text;
    char* verb = nextTok(&rest);
    for (char* p = verb; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

    char msg[900];
    msg[0] = 0;
    bool ok = false;
    bool quiet = false;               // ping/help: no log line, no event
    bool world = ctx.bodies != 0;
    char dumpPath[MAX_PATH * 2];
    dumpPath[0] = 0;
    const char* evRule = "";
    char evHand[32];
    evHand[0] = 0;

    if (verb[0] == 0) {
        _snprintf(msg, sizeof(msg) - 1, "empty command; try help");
        quiet = true;
    } else if (strcmp(verb, "ping") == 0) {
        ok = true; quiet = true;
        _snprintf(msg, sizeof(msg) - 1, "pong");
    } else if (strcmp(verb, "help") == 0) {
        ok = true; quiet = true;
        _snprintf(msg, sizeof(msg) - 1, "%s", kHelp);
    } else if (strcmp(verb, "restore") == 0 || strcmp(verb, "endaction") == 0 ||
               strcmp(verb, "knockdown") == 0 || strcmp(verb, "halt") == 0 ||
               strcmp(verb, "move") == 0 || strcmp(verb, "dump") == 0) {
        char* handArg = nextTok(&rest);
        _snprintf(evHand, sizeof(evHand) - 1, "%s", handArg);
        evHand[sizeof(evHand) - 1] = 0;
        if (!world) {
            _snprintf(msg, sizeof(msg) - 1, "no world");
        } else if (strcmp(verb, "dump") == 0) {
            unsigned int idx = 0, ser = 0;
            RingMap::iterator it = g_rings.end();
            if (sscanf_s(handArg, "%u,%u", &idx, &ser) == 2) it = g_rings.find(keyOf(idx, ser));
            if (it == g_rings.end()) {
                _snprintf(msg, sizeof(msg) - 1, "no samples for hand '%s'", handArg);
            } else {
                evRule = "manual";
                ok = writeDump(it->second, "manual", "manual dump", dumpPath, sizeof(dumpPath));
                _snprintf(msg, sizeof(msg) - 1, ok ? "%s" : "dump write failed%s", dumpPath);
            }
        } else {
            const DevBody* b = 0;
            char err[200];
            Character* c = resolveHandArg(handArg, ctx, &b, err, sizeof(err));
            if (!c) {
                _snprintf(msg, sizeof(msg) - 1, "%s", err);
            } else if (strcmp(verb, "restore") == 0) {
                if (engine::hasPhysicsBody(c)) {
                    // restoreMovement leaves an existing controller alone.
                    ok = true;
                    _snprintf(msg, sizeof(msg) - 1, "'%s' already has a physics body (nothing to restore)", b->name);
                } else {
                    ok = engine::restoreMovement(c);
                    _snprintf(msg, sizeof(msg) - 1, "restoreMovement '%s' -> %d (hk now %d)", b->name,
                              ok ? 1 : 0, engine::hasPhysicsBody(c) ? 1 : 0);
                }
            } else if (strcmp(verb, "endaction") == 0) {
                ok = engine::endAction(c);
                _snprintf(msg, sizeof(msg) - 1, "endAction '%s' -> %d", b->name, ok ? 1 : 0);
            } else if (strcmp(verb, "halt") == 0) {
                ok = engine::haltMovement(c);
                _snprintf(msg, sizeof(msg) - 1, "haltMovement '%s' -> %d", b->name, ok ? 1 : 0);
            } else if (strcmp(verb, "move") == 0) {
                // Player move order relative to where the body stands now: a
                // scripted walk for reproducing a session from the dashboard.
                float dx = 0.0f, dz = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
                const char* sx = nextTok(&rest);
                const char* sz = nextTok(&rest);
                if (sscanf_s(sx, "%f", &dx) != 1 || sscanf_s(sz, "%f", &dz) != 1) {
                    _snprintf(msg, sizeof(msg) - 1, "usage: move <i,s> <dx> <dz>");
                } else if (!engine::readPos(c, &x, &y, &z)) {
                    _snprintf(msg, sizeof(msg) - 1, "move: cannot read position of '%s'", b->name);
                } else {
                    ok = engine::orderMoveTo(c, x + dx, y, z + dz);
                    _snprintf(msg, sizeof(msg) - 1, "orderMoveTo '%s' (%.0f,%.0f) -> %d", b->name,
                              x + dx, z + dz, ok ? 1 : 0);
                }
            } else {
                bool on = false;
                if (!parseOnOff(nextTok(&rest), on)) {
                    _snprintf(msg, sizeof(msg) - 1, "usage: knockdown <i,s> <0|1>");
                } else {
                    ok = engine::knockDown(c, on);
                    _snprintf(msg, sizeof(msg) - 1, "knockDown '%s' %d -> %d", b->name, on ? 1 : 0, ok ? 1 : 0);
                }
            }
        }
    } else if (strcmp(verb, "glide") == 0) {
        bool on = false;
        if (!parseOnOff(nextTok(&rest), on)) {
            _snprintf(msg, sizeof(msg) - 1, "usage: glide <on|off>");
        } else if (!ctx.repl) {
            _snprintf(msg, sizeof(msg) - 1, "no world");
        } else {
            ctx.repl->setGlide(on);
            ok = true;
            _snprintf(msg, sizeof(msg) - 1, "glide %s", on ? "on" : "off");
        }
    } else if (strcmp(verb, "overlay") == 0) {
        bool on = false;
        if (!parseOnOff(nextTok(&rest), on)) {
            _snprintf(msg, sizeof(msg) - 1, "usage: overlay <on|off>");
        } else {
            g_overlayOn = on; ok = true;
            _snprintf(msg, sizeof(msg) - 1, "overlay %s", on ? "on" : "off");
        }
    } else if (strcmp(verb, "watchdog") == 0) {
        bool on = false;
        if (!parseOnOff(nextTok(&rest), on)) {
            _snprintf(msg, sizeof(msg) - 1, "usage: watchdog <on|off>");
        } else {
            // Fresh timers: a condition that held while off must not trip instantly.
            if (on != g_watchdogOn) resetRuleTimers();
            g_watchdogOn = on; ok = true;
            _snprintf(msg, sizeof(msg) - 1, "watchdog %s", on ? "on" : "off");
        }
    } else if (strcmp(verb, "mark") == 0) {
        char line[400];
        _snprintf(line, sizeof(line) - 1, "[dev] MARK %s", rest);
        line[sizeof(line) - 1] = 0;
        logPlain(line);
        sendEvent("mark", "", "", rest, "");
        ok = true; quiet = true;      // the mark event is the record
        _snprintf(msg, sizeof(msg) - 1, "marked");
    } else if (strcmp(verb, "record") == 0) {
        char* what = nextTok(&rest);
        if (_stricmp(what, "start") == 0) {
            std::string dir = g_cfg.baseDir.empty() ? std::string(".") : g_cfg.baseDir;
            dir += "\\kc_records";
            ok = rec::startNow(dir.c_str(), g_isHost, g_cfg.version.c_str(), "manual");
            const char* p = rec::currentPath();
            _snprintf(msg, sizeof(msg) - 1, ok ? "recording %s" : "record start failed (%s)",
                      ok ? (p ? p : "") : dir.c_str());
        } else if (_stricmp(what, "stop") == 0) {
            rec::stopNow();
            ok = true;
            _snprintf(msg, sizeof(msg) - 1, "recording stopped");
        } else {
            _snprintf(msg, sizeof(msg) - 1, "usage: record <start|stop>");
        }
    } else {
        _snprintf(msg, sizeof(msg) - 1, "unknown command '%s'; try help", verb);
    }
    msg[sizeof(msg) - 1] = 0;

    reply(from, verb, ok, msg);
    if (!quiet) {
        char line[1200];
        _snprintf(line, sizeof(line) - 1, "[dev] cmd '%s' ok=%d %s", original, ok ? 1 : 0, msg);
        line[sizeof(line) - 1] = 0;
        logPlain(line);
        char evMsg[400];
        _snprintf(evMsg, sizeof(evMsg) - 1, "%s -> ok=%d %s", original, ok ? 1 : 0, msg);
        evMsg[sizeof(evMsg) - 1] = 0;
        sendEvent("cmd", evRule, evHand, evMsg, dumpPath);
    }
}

void pollCommands(const CmdCtx& ctx) {
    if (g_cmd == INVALID_SOCKET) return;
    for (int i = 0; i < CMD_PER_TICK; ++i) {
        char buf[512];
        sockaddr_in from;
        int flen = sizeof(from);
        int n = recvfrom(g_cmd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            // A stale ICMP reset or an oversized datagram is per-datagram noise.
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
            return;                        // WSAEWOULDBLOCK: queue drained
        }
        buf[n] = 0;
        execCommand(buf, from, ctx);
    }
}

// ---- Overlay hotkey ---------------------------------------------------------------------------

// Ctrl+F9 rising edge, only while the game window has focus (GetAsyncKeyState
// is global: without this an alt-tabbed editor's Ctrl+F9 would toggle it).
void pollHotkey() {
    DWORD fgPid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    bool down = fgPid == GetCurrentProcessId() &&
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !g_keyWasDown) {
        g_overlayOn = !g_overlayOn;
        logLine(g_overlayOn ? "[dev] overlay on (Ctrl+F9)" : "[dev] overlay off (Ctrl+F9)");
    }
    g_keyWasDown = down;
}

void sendGlobalIfDue(bool world, unsigned int now) {
    unsigned int period = world ? GLOBAL_WORLD_MS : GLOBAL_IDLE_MS;
    if (g_sentGlobal && now - g_lastGlobalMs < period) return;
    g_sentGlobal = true;
    g_lastGlobalMs = now;
    char buf[DGRAM_MAX];
    JBuf j; jInit(j, buf, sizeof(buf));
    buildGlobal(j, world, now);
    sendTel(j);
}

} // namespace

// ---- Public API -------------------------------------------------------------------------------

void init(const DevConfig& cfg) {
    if (g_on) shutdown();
    g_cfg = cfg;
    g_isHost = cfg.isHost;
    if (!cfg.enabled) return;              // off: no sockets, no files, nothing

    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qp0);
    g_overlayOn = cfg.overlay;
    g_watchdogOn = cfg.watchdog;
    g_sentBody = g_sentGlobal = false;
    g_hadWorld = false;
    g_keyWasDown = false;
    memset(&g_lastGlobal, 0, sizeof(g_lastGlobal));
    g_lastGlobal.isHost = cfg.isHost;
    g_lastGlobal.timeSlew = 1.0f;
    g_lastGlobal.speed = 1.0f;
    g_rings.clear();

    int port = cfg.telemetryPort > 0 && cfg.telemetryPort < 65536 ? cfg.telemetryPort : DEFAULT_TELEMETRY_PORT;
    if (!openSockets(port)) {
        char line[160];
        _snprintf(line, sizeof(line) - 1, "[dev] devtools: socket setup failed (wsa %d); telemetry/commands off",
                  WSAGetLastError());
        line[sizeof(line) - 1] = 0;
        logErrLine(line);
    }
    g_on = true;   // watchdog/overlay still work without sockets
    char line[200];
    _snprintf(line, sizeof(line) - 1, "[dev] devtools on: telemetry 127.0.0.1:%d cmdPort %d overlay=%d watchdog=%d",
              port, g_cmdPort, g_overlayOn ? 1 : 0, g_watchdogOn ? 1 : 0);
    line[sizeof(line) - 1] = 0;
    logLine(line);
}

void shutdown() {
    closeSock(g_cmd);
    closeSock(g_tel);
    g_cmdPort = 0;
    if (g_wsa) { WSACleanup(); g_wsa = false; }
    g_rings.clear();
    g_on = false;
}

bool enabled() { return g_on; }

void setRole(bool isHost) {
    g_isHost = isHost;
    g_lastGlobal.isHost = isHost;
}

void tick(GameWorld* gw, Replicator& repl, const std::vector<DevBody>& bodies, const DevGlobal& g) {
    if (!g_on) return;
    unsigned int now = nowMs();
    g_lastGlobal = g;
    g_isHost = g.isHost;
    g_hadWorld = true;

    watchdogTick(bodies, now);
    pruneRings(now);

    CmdCtx ctx;
    ctx.gw = gw; ctx.repl = &repl; ctx.bodies = &bodies;
    pollCommands(ctx);

    if (!g_sentBody || now - g_lastBodyMs >= BODY_PERIOD_MS) {
        g_sentBody = true;
        g_lastBodyMs = now;
        for (size_t i = 0; i < bodies.size(); ++i) {
            char buf[DGRAM_MAX];
            JBuf j; jInit(j, buf, sizeof(buf));
            buildBody(j, bodies[i], now);
            sendTel(j);
        }
    }
    sendGlobalIfDue(true, now);

    pollHotkey();
    overlay::tick(gw, bodies, g_overlayOn);
}

void idleTick() {
    if (!g_on) return;
    unsigned int now = nowMs();
    if (g_hadWorld) {
        // World went away: drop labels once, and stop the in-world global cadence.
        overlay::clear();
        g_hadWorld = false;
        g_sentGlobal = false;
    }
    pruneRings(now);
    CmdCtx ctx;
    ctx.gw = 0; ctx.repl = 0; ctx.bodies = 0;
    pollCommands(ctx);
    sendGlobalIfDue(false, now);
}

} // namespace dev
} // namespace coop
