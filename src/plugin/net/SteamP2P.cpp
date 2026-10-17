#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "SteamP2P.h"
#include "../CoopLog.h"

#include <windows.h>
#include <enet/enet.h>
#include <cstring>
#include <cstdio>

namespace coop {
namespace steamp2p {
namespace {

// ---- Legacy Steamworks flat-API surface (declared by hand: no SDK headers; the
// v100 toolchain predates the SDK's C++11 requirements anyway). x64 has a single
// calling convention; CSteamID crosses the flat ABI as a plain uint64.
// -----------------------------------------------------------------------------

// P2PSessionState_t (legacy SDK layout, naturally aligned, 20 bytes).
struct SessionState {
    unsigned char  connectionActive;   // !=0 when the session is up
    unsigned char  connecting;         // !=0 while brokering
    unsigned char  lastError;          // EP2PSessionError (0 = none)
    unsigned char  usingRelay;         // !=0 when routed via a Valve relay
    int            bytesQueuedForSend;
    int            packetsQueuedForSend;
    unsigned int   remoteIP;
    unsigned short remotePort;
};

// EP2PSend: 0 = unreliable (buffered while the session is still brokering -
// exactly what the ENet connect/handshake needs, so we never use NoDelay).
const int SEND_UNRELIABLE = 0;

const int CH_TUNNEL = 0; // ENet datagrams
const int CH_SPIKE  = 1; // ping/echo spike harness

typedef int   (*GetHSteamFn)();
typedef void* (*SteamClientFn)();
typedef void* (*ClientGetIfaceFn)(void* client, int hUser, int hPipe, const char* version);
typedef unsigned long long (*UserGetSteamIDFn)(void* user);
typedef bool  (*UserBLoggedOnFn)(void* user);
typedef bool  (*NetSendP2PFn)(void* net, unsigned long long id, const void* data,
                              unsigned int len, int sendType, int channel);
typedef bool  (*NetIsAvailFn)(void* net, unsigned int* msgSize, int channel);
typedef bool  (*NetReadP2PFn)(void* net, void* dest, unsigned int destSize,
                              unsigned int* msgSize, unsigned long long* sender, int channel);
typedef bool  (*NetSteamIdBoolFn)(void* net, unsigned long long id);
typedef bool  (*NetSessionStateFn)(void* net, unsigned long long id, SessionState* state);
typedef bool  (*NetAllowRelayFn)(void* net, bool allow);

void*             g_iface       = 0; // ISteamNetworking*
void*             g_user        = 0; // ISteamUser*
NetSendP2PFn      g_send        = 0;
NetIsAvailFn      g_isAvail     = 0;
NetReadP2PFn      g_read        = 0;
NetSteamIdBoolFn  g_accept      = 0;
NetSteamIdBoolFn  g_close       = 0;
NetSessionStateFn g_sessionState = 0;
UserBLoggedOnFn   g_loggedOn    = 0;

bool    g_ready    = false;
SteamId g_selfId   = 0;
SteamId g_pingPeer = 0; // spike peer (channel 1)

// Tunnel peers (channel 0), one per slot. A client has exactly one (the host,
// slot 0); a host has one slot per connected join. Written on the MAIN thread
// (setPeer before the net thread starts; addPeer while it runs), read on the
// NET thread: an aligned 64-bit store/load is atomic on x64 and the volatile
// keeps the net thread from caching a slot, so no lock is needed. Slots are
// only ever filled while a session runs (cleared by setPeer / shutdown), so a
// reader never sees a slot change identity under it.
volatile SteamId g_peers[MAX_TUNNEL_PEERS];

// Fake identity the tunnel reports to ENet for slot k: "1.0.k.1":port.
// ENetAddress.host is a raw in_addr (network byte order); on little-endian x64
// the u32 0x01000001 is the byte sequence {1,0,0,1}, so slot 0 is "1.0.0.1" -
// the address the client connects to.
const enet_uint32 FAKE_HOST = 0x01000001u;
unsigned short    g_fakePort = 27800;

enet_uint32 fakeHost(int slot) { return FAKE_HOST | ((enet_uint32)slot << 16); }
int slotOfHost(enet_uint32 host) {
    if ((host & 0xFF00FFFFu) != FAKE_HOST) return -1;
    int slot = (int)((host >> 16) & 0xFFu);
    return (slot < MAX_TUNNEL_PEERS) ? slot : -1;
}
int slotOf(SteamId id) {
    if (id == 0) return -1;
    for (int i = 0; i < MAX_TUNNEL_PEERS; ++i) if (g_peers[i] == id) return i;
    return -1;
}

// Net-thread-only diagnostics state.
bool  g_loggedFirstRecv   = false;
bool  g_loggedStraySender = false;
DWORD g_lastStateTick     = 0;
SessionState g_lastState[MAX_TUNNEL_PEERS]; // zero-initialised (file scope)
bool  g_haveLastState[MAX_TUNNEL_PEERS];
DWORD g_lastPingTick      = 0;
unsigned int g_pingSeq    = 0;

void steamLog(const char* msg) {
    char b[256];
    _snprintf(b, sizeof(b) - 1, "[steam] %s", msg ? msg : "");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// ---- Steam teardown guard ------------------------------------------------------
// The flat ISteamNetworking calls dispatch through steamclient64.dll's vtable. When
// the game shuts Steam down (SteamAPI_Shutdown on exit, or the Steam client going
// away) that module is unloaded while our NET thread is still polling in hookWait,
// and the next call reads unmapped memory (2026-10-17 real session: AV at
// steam_api64.dll+0x48e3 from hookWait/g_isAvail). Every Steam call goes through
// these SEH thunks; the first fault marks the tunnel dead (g_ready=false), so the
// hooks report "no traffic" and ENet times the peer out instead of the process
// dying. No objects with destructors inside the __try bodies (C2712).
volatile LONG g_steamFaulted = 0;

void markSteamDead() {
    g_ready = false;
    if (InterlockedExchange(&g_steamFaulted, 1) == 0)
        steamLog("Steam interface faulted (Steam shut down / unloaded?) - tunnel disabled");
}

bool isAvailSeh(unsigned int* avail, int ch) {
    if (!g_ready) return false;
    bool ok = false, bad = false;
    __try { ok = g_isAvail(g_iface, avail, ch); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
    if (bad) { markSteamDead(); return false; }
    return ok;
}

bool readSeh(void* buf, unsigned int cap, unsigned int* got,
             unsigned long long* sender, int ch) {
    if (!g_ready) return false;
    bool ok = false, bad = false;
    __try { ok = g_read(g_iface, buf, cap, got, sender, ch); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
    if (bad) { markSteamDead(); return false; }
    return ok;
}

bool sendSeh(SteamId to, const void* buf, unsigned int len, int type, int ch) {
    if (!g_ready) return false;
    bool ok = false, bad = false;
    __try { ok = g_send(g_iface, to, buf, len, type, ch); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
    if (bad) { markSteamDead(); return false; }
    return ok;
}

bool sessionStateSeh(SteamId peer, SessionState* st) {
    if (!g_ready) return false;
    bool ok = false, bad = false;
    __try { ok = g_sessionState(g_iface, peer, st); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
    if (bad) { markSteamDead(); return false; }
    return ok;
}

// ---- ENet socket hooks (net thread only) -------------------------------------
// One fake socket handle; a send goes to the SteamID in the destination
// address's slot, a receive is accepted only from a SteamID that holds a slot
// and is reported to ENet from that slot's fabricated address.

const ENetSocket FAKE_SOCKET = (ENetSocket)0x51EAD;

ENetSocket ENET_CALLBACK hookCreate(ENetSocketType type) {
    (void)type;
    return FAKE_SOCKET;
}

int ENET_CALLBACK hookBind(ENetSocket s, const ENetAddress* address) {
    (void)s; (void)address;
    return 0;
}

int ENET_CALLBACK hookSend(ENetSocket s, const ENetAddress* address,
                           const ENetBuffer* buffers, size_t bufferCount) {
    unsigned char buf[4096];
    unsigned int  len = 0;
    size_t i;
    (void)s;
    if (!g_ready || address == 0) return -1;
    int slot = slotOfHost(address->host);
    if (slot < 0) return -1;
    SteamId to = g_peers[slot];
    if (to == 0) return -1;
    for (i = 0; i < bufferCount; ++i) {
        if (len + buffers[i].dataLength > sizeof(buf)) return -1;
        std::memcpy(buf + len, buffers[i].data, buffers[i].dataLength);
        len += (unsigned int)buffers[i].dataLength;
    }
    if (!sendSeh(to, buf, len, SEND_UNRELIABLE, CH_TUNNEL)) return -1;
    return (int)len;
}

int ENET_CALLBACK hookReceive(ENetSocket s, ENetAddress* address,
                              ENetBuffer* buffers, size_t bufferCount) {
    unsigned int avail = 0, got = 0;
    unsigned long long sender = 0;
    unsigned char buf[4096];
    (void)s;
    if (!g_ready || bufferCount < 1) return 0;
    if (!isAvailSeh(&avail, CH_TUNNEL)) return 0;
    if (!readSeh(buf, sizeof(buf), &got, &sender, CH_TUNNEL)) return 0;
    int slot = slotOf(sender);
    if (slot < 0) {
        // Someone who is not one of our co-op peers sent to us: drop.
        if (!g_loggedStraySender) {
            g_loggedStraySender = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "dropping packets from unexpected peer %llu", sender);
            b[sizeof(b) - 1] = '\0';
            steamLog(b);
        }
        return 0;
    }
    if (got > buffers[0].dataLength) return -2; // oversized (like WSAEMSGSIZE)
    std::memcpy(buffers[0].data, buf, got);
    if (address != 0) {
        address->host = fakeHost(slot);
        address->port = g_fakePort;
    }
    if (!g_loggedFirstRecv) {
        g_loggedFirstRecv = true;
        steamLog("first tunnel packet received from peer");
    }
    return (int)got;
}

int ENET_CALLBACK hookWait(ENetSocket s, enet_uint32* condition, enet_uint32 timeout) {
    enet_uint32 want = *condition;
    DWORD deadline = GetTickCount() + timeout;
    (void)s;
    *condition = ENET_SOCKET_WAIT_NONE;
    if (want & ENET_SOCKET_WAIT_SEND) {
        // The tunnel is always writable (Steam buffers internally).
        *condition |= ENET_SOCKET_WAIT_SEND;
        return 0;
    }
    if (!(want & ENET_SOCKET_WAIT_RECEIVE)) {
        if (timeout > 0) Sleep(timeout);
        return 0;
    }
    for (;;) {
        unsigned int avail = 0;
        if (isAvailSeh(&avail, CH_TUNNEL)) {
            *condition |= ENET_SOCKET_WAIT_RECEIVE;
            return 0;
        }
        if ((long)(deadline - GetTickCount()) <= 0) return 0;
        Sleep(5); // sub-tick poll; the net loop services at 20 Hz
    }
}

void ENET_CALLBACK hookDestroy(ENetSocket s) {
    (void)s;
}

ENetSocketHooks g_hooks; // filled in installEnetHooks

// ---- Session-state + spike logging (net thread, via tick()) -------------------

void logSessionState(int slot, SteamId peer) {
    SessionState st;
    std::memset(&st, 0, sizeof(st));
    if (!sessionStateSeh(peer, &st)) return;
    SessionState& last = g_lastState[slot];
    if (g_haveLastState[slot] &&
        st.connectionActive == last.connectionActive &&
        st.connecting       == last.connecting &&
        st.lastError        == last.lastError &&
        st.usingRelay       == last.usingRelay) {
        return; // unchanged; stay quiet
    }
    last = st;
    g_haveLastState[slot] = true;
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              "session peer=%llu active=%u connecting=%u relay=%u err=%u",
              peer, (unsigned)st.connectionActive, (unsigned)st.connecting,
              (unsigned)st.usingRelay, (unsigned)st.lastError);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
}

// Spike wire format (channel 1): 'KCSP' | seq | senderTick  (ping)
//                                'KCSQ' | seq | senderTick  (echo)
struct SpikeMsg { char tag[4]; unsigned int seq; unsigned int tick; };

void spikeTick() {
    DWORD now = GetTickCount();

    // Drain echoes/pings.
    for (;;) {
        unsigned int avail = 0, got = 0;
        unsigned long long sender = 0;
        SpikeMsg m;
        if (!isAvailSeh(&avail, CH_SPIKE)) break;
        if (!readSeh(&m, sizeof(m), &got, &sender, CH_SPIKE)) break;
        if (got != sizeof(m)) continue;
        if (std::memcmp(m.tag, "KCSP", 4) == 0) {
            // Ping: echo it back with the sender's own timestamp intact.
            std::memcpy(m.tag, "KCSQ", 4);
            sendSeh(sender, &m, sizeof(m), SEND_UNRELIABLE, CH_SPIKE);
        } else if (std::memcmp(m.tag, "KCSQ", 4) == 0) {
            SessionState st;
            std::memset(&st, 0, sizeof(st));
            sessionStateSeh(sender, &st);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "ping seq=%u rtt=%lu ms relay=%u",
                      m.seq, (unsigned long)(now - m.tick), (unsigned)st.usingRelay);
            b[sizeof(b) - 1] = '\0';
            steamLog(b);
        }
    }

    // Send the next ping every ~2 s.
    if (g_pingPeer != 0 && (g_lastPingTick == 0 || now - g_lastPingTick >= 2000)) {
        SpikeMsg m;
        g_lastPingTick = now;
        std::memcpy(m.tag, "KCSP", 4);
        m.seq  = ++g_pingSeq;
        m.tick = now;
        sendSeh(g_pingPeer, &m, sizeof(m), SEND_UNRELIABLE, CH_SPIKE);
    }
}

void clearPeers() {
    for (int i = 0; i < MAX_TUNNEL_PEERS; ++i) {
        g_peers[i] = 0;
        g_haveLastState[i] = false;
    }
}

} // namespace

// ---- Public API ---------------------------------------------------------------

bool init() {
    if (g_ready) return true;

    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (mod == 0) mod = LoadLibraryA("steam_api64.dll");
    if (mod == 0) { steamLog("init FAILED: steam_api64.dll not loaded"); return false; }

    GetHSteamFn      getUser   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamUser");
    GetHSteamFn      getPipe   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamPipe");
    SteamClientFn    getClient = (SteamClientFn)   GetProcAddress(mod, "SteamClient");
    ClientGetIfaceFn getNetIf  = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamNetworking");
    ClientGetIfaceFn getUserIf = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamUser");
    UserGetSteamIDFn getId     = (UserGetSteamIDFn)GetProcAddress(mod, "SteamAPI_ISteamUser_GetSteamID");
    NetAllowRelayFn  allowRelay= (NetAllowRelayFn) GetProcAddress(mod, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");

    g_loggedOn     = (UserBLoggedOnFn)  GetProcAddress(mod, "SteamAPI_ISteamUser_BLoggedOn");
    g_send         = (NetSendP2PFn)     GetProcAddress(mod, "SteamAPI_ISteamNetworking_SendP2PPacket");
    g_isAvail      = (NetIsAvailFn)     GetProcAddress(mod, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    g_read         = (NetReadP2PFn)     GetProcAddress(mod, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    g_accept       = (NetSteamIdBoolFn) GetProcAddress(mod, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    g_close        = (NetSteamIdBoolFn) GetProcAddress(mod, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    g_sessionState = (NetSessionStateFn)GetProcAddress(mod, "SteamAPI_ISteamNetworking_GetP2PSessionState");

    if (!getUser || !getPipe || !getClient || !getNetIf || !getUserIf || !getId ||
        !g_send || !g_isAvail || !g_read || !g_accept || !g_close || !g_sessionState) {
        steamLog("init FAILED: flat exports missing from steam_api64.dll");
        return false;
    }

    void* client = getClient();
    if (client == 0) { steamLog("init FAILED: SteamClient() returned null (Steam not running?)"); return false; }
    int hUser = getUser();
    int hPipe = getPipe();
    if (hUser == 0 || hPipe == 0) {
        steamLog("init FAILED: no Steam user/pipe (game not launched under Steam?)");
        return false;
    }

    g_user = getUserIf(client, hUser, hPipe, "SteamUser019");
    if (g_user == 0) { steamLog("init FAILED: SteamUser019 interface unavailable"); return false; }

    // Kenshi's DLL is SteamClient017-era, whose flat ISteamNetworking accessors
    // target SteamNetworking005; try neighbours in case the client returns them.
    const char* versions[] = { "SteamNetworking005", "SteamNetworking006", "SteamNetworking004" };
    const char* used = 0;
    int i;
    for (i = 0; i < 3 && g_iface == 0; ++i) {
        g_iface = getNetIf(client, hUser, hPipe, versions[i]);
        if (g_iface != 0) used = versions[i];
    }
    if (g_iface == 0) { steamLog("init FAILED: no ISteamNetworking interface"); return false; }

    g_selfId = getId(g_user);
    if (allowRelay != 0) allowRelay(g_iface, true); // Valve-relay fallback ON

    g_ready = true;
    {
        bool on = (g_loggedOn != 0) ? g_loggedOn(g_user) : false;
        char b[160];
        _snprintf(b, sizeof(b) - 1, "id=%llu loggedOn=%d iface=%s", g_selfId, on ? 1 : 0, used);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
        if (!on) steamLog("WARNING: Steam is in offline mode - P2P will not connect");
    }
    return true;
}

bool ready()      { return g_ready; }
SteamId selfId()  { return g_selfId; }

void setPeer(SteamId id) {
    clearPeers();
    if (id == 0) return;
    g_peers[0] = id;
    if (g_ready) {
        g_accept(g_iface, id); // proactive accept (two-code exchange, no callbacks)
        char b[96];
        _snprintf(b, sizeof(b) - 1, "tunnel peer=%llu (session pre-accepted)", id);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
    }
}

bool addPeer(SteamId id) {
    if (id == 0) return false;
    if (slotOf(id) >= 0) return true;
    for (int i = 0; i < MAX_TUNNEL_PEERS; ++i) {
        if (g_peers[i] != 0) continue;
        if (g_ready) g_accept(g_iface, id);
        g_peers[i] = id;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "tunnel peer=%llu added (slot %d)", id, i);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
        return true;
    }
    steamLog("tunnel full - peer not added");
    return false;
}

void accept(SteamId id) {
    if (!g_ready || id == 0) return;
    g_accept(g_iface, id);
    char b[96];
    _snprintf(b, sizeof(b) - 1, "accepted inbound session from %llu", id);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
}

void setPingPeer(SteamId id) {
    g_pingPeer = id;
    if (g_ready && id != 0) {
        g_accept(g_iface, id);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "spike ping peer=%llu (channel 1, 2 s cadence)", id);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
    }
}

void tick() {
    if (!g_ready) return;
    DWORD now = GetTickCount();
    // Session-state transitions, logged at most every ~1 s (and only on change).
    if (now - g_lastStateTick >= 1000) {
        g_lastStateTick = now;
        bool any = false;
        for (int i = 0; i < MAX_TUNNEL_PEERS; ++i) {
            SteamId p = g_peers[i];
            if (p != 0) { logSessionState(i, p); any = true; }
        }
        if (!any && g_pingPeer != 0) logSessionState(0, g_pingPeer);
    }
    if (g_pingPeer != 0) spikeTick();
}

bool installEnetHooks(int port) {
    if (!g_ready || g_peers[0] == 0) return false;
    g_fakePort = (unsigned short)port;
    std::memset(&g_hooks, 0, sizeof(g_hooks));
    g_hooks.socket_create  = &hookCreate;
    g_hooks.socket_bind    = &hookBind;
    g_hooks.socket_send    = &hookSend;
    g_hooks.socket_receive = &hookReceive;
    g_hooks.socket_wait    = &hookWait;
    g_hooks.socket_destroy = &hookDestroy;
    enet_set_socket_hooks(&g_hooks);
    steamLog("ENet socket hooks installed (tunnel on P2P channel 0)");
    return true;
}

void removeEnetHooks() {
    enet_set_socket_hooks(0);
}

void shutdown() {
    if (!g_ready) return;
    for (int i = 0; i < MAX_TUNNEL_PEERS; ++i) {
        SteamId p = g_peers[i];
        if (p != 0 && p != g_pingPeer) g_close(g_iface, p);
    }
    if (g_pingPeer != 0) g_close(g_iface, g_pingPeer);
    clearPeers();
}

} // namespace steamp2p
} // namespace coop
