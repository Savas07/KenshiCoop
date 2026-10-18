#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "SteamInvite.h"
#include "SteamP2P.h"        // selfId() + accept() (shared Steam client)
#include "../CoopLog.h"
#include "../../netproto/Wire.h" // coop::PROTOCOL_VERSION (lobby-data version gate)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib> // atoi / _strtoui64 (version gate, launch argument)

namespace coop {
namespace steaminvite {
namespace {

// ---- Steamworks callback ABI (hand-rolled; no SDK headers) -------------------
// CCallbackBase (steam_api_common.h) layout the client's callback manager expects:
//   [0] vptr
//   [8] uint8  m_nCallbackFlags
//   [12] int   m_iCallback
// vtable order: Run(void*) / Run(void*,bool,SteamAPICall_t) / GetCallbackSizeBytes().
// We match it with a plain class whose 3 virtuals are declared in that exact order;
// MSVC lays vptr at 0 and packs the two members at 8/12. Our own fields live past
// offset 16 where the manager never looks. Dispatch routes on m_iCallback.

typedef unsigned long long SteamAPICall_t;

void dispatch(int iCallback, void* param, bool ioFailure); // fwd

class SteamCallback {
public:
    virtual void Run(void* param) { dispatch(m_iCallback, param, false); }
    virtual void Run(void* param, bool ioFailure, SteamAPICall_t) {
        dispatch(m_iCallback, param, ioFailure);
    }
    virtual int GetCallbackSizeBytes() { return m_size; }

    unsigned char m_nCallbackFlags; // offset 8  (written by RegisterCallback)
    int           m_iCallback;      // offset 12 (callback id)
    int           m_size;           // ours: sizeof the callback param struct

    void arm(int id, int sz) { m_nCallbackFlags = 0; m_iCallback = id; m_size = sz; }
};

// ---- Callback ids ------------------------------------------------------------
const int k_iSteamMatchmakingCallbacks = 500;
const int k_iSteamNetworkingCallbacks  = 1200;
const int cb_LobbyCreated           = k_iSteamMatchmakingCallbacks + 13; // 513
const int cb_LobbyEnter             = k_iSteamMatchmakingCallbacks + 4;  // 504
const int cb_GameLobbyJoinRequested = k_iSteamMatchmakingCallbacks + 33; // 533
const int cb_P2PSessionRequest      = k_iSteamNetworkingCallbacks  + 2;  // 1202

// ---- Callback param structs (Steamworks #pragma pack(8) layouts) -------------
#pragma pack(push, 8)
struct LobbyCreated_t          { int m_eResult; unsigned long long m_ulSteamIDLobby; };
struct GameLobbyJoinRequested_t{ unsigned long long m_steamIDLobby; unsigned long long m_steamIDFriend; };
struct P2PSessionRequest_t     { unsigned long long m_steamIDRemote; };
struct LobbyEnter_t {
    unsigned long long m_ulSteamIDLobby;
    unsigned int       m_rgfChatPermissions;
    unsigned char      m_bLocked;
    unsigned int       m_EChatRoomEnterResponse;
};
#pragma pack(pop)

const int      k_EResultOK                        = 1;
const int      k_ELobbyTypeFriendsOnly            = 1;
const unsigned k_EChatRoomEnterResponseSuccess    = 1;
const int      k_EFriendFlagImmediate             = 0x04;

// ---- Flat API surface --------------------------------------------------------
typedef int   (*GetHSteamFn)();
typedef void* (*SteamClientFn)();
typedef void* (*ClientGetIfaceFn)(void* client, int hUser, int hPipe, const char* version);

typedef SteamAPICall_t (*MmCreateLobbyFn)(void* mm, int eLobbyType, int cMaxMembers);
typedef SteamAPICall_t (*MmJoinLobbyFn)(void* mm, unsigned long long lobby);
typedef void  (*MmLeaveLobbyFn)(void* mm, unsigned long long lobby);
typedef bool  (*MmInviteToLobbyFn)(void* mm, unsigned long long lobby, unsigned long long invitee);
typedef bool  (*MmSetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k, const char* v);
typedef const char* (*MmGetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k);
typedef bool  (*MmRequestLobbyDataFn)(void* mm, unsigned long long lobby);
typedef unsigned long long (*MmGetLobbyOwnerFn)(void* mm, unsigned long long lobby);
typedef int   (*MmGetNumMembersFn)(void* mm, unsigned long long lobby);
typedef unsigned long long (*MmGetMemberByIndexFn)(void* mm, unsigned long long lobby, int i);
typedef int   (*FrGetCountFn)(void* friends, int eFriendFlags);
typedef unsigned long long (*FrGetByIndexFn)(void* friends, int i, int eFriendFlags);
typedef const char* (*FrGetNameFn)(void* friends, unsigned long long id);
typedef int   (*FrGetStateFn)(void* friends, unsigned long long id);

#pragma pack(push, 8)
struct FriendGameInfo_t {
    unsigned long long m_gameID;       // CGameID (appID in low 24 bits for app type)
    unsigned int       m_unGameIP;
    unsigned short     m_usGamePort;
    unsigned short     m_usQueryPort;
    unsigned long long m_steamIDLobby;
};
#pragma pack(pop)
typedef bool  (*FrGetGamePlayedFn)(void* friends, unsigned long long id, FriendGameInfo_t* out);
typedef const char* (*FrGetSelfNameFn)(void* friends);
typedef int   (*FrGetAvatarFn)(void* friends, unsigned long long id);
typedef bool  (*FrReqUserInfoFn)(void* friends, unsigned long long id, bool nameOnly);
typedef void* (*ClientGetUtilsFn)(void* client, int hPipe, const char* version);
typedef bool  (*UtGetImageSizeFn)(void* utils, int image, unsigned int* w, unsigned int* h);
typedef bool  (*UtGetImageRGBAFn)(void* utils, int image, unsigned char* dest, int destSize);

const unsigned KENSHI_APPID = 233860;
typedef void  (*RegisterCbFn)(void* cb, int iCallback);
typedef void  (*RegisterCrFn)(void* cb, SteamAPICall_t call, int iCallback);
typedef void  (*UnregisterCbFn)(void* cb);
typedef void  (*RunCallbacksFn)();

void*                 g_mm       = 0; // ISteamMatchmaking*
void*                 g_friends  = 0; // ISteamFriends*
MmCreateLobbyFn       g_createLobby = 0;
MmJoinLobbyFn         g_joinLobby   = 0;
MmLeaveLobbyFn        g_leaveLobby  = 0;
MmInviteToLobbyFn     g_inviteToLobby = 0;
MmSetLobbyDataFn      g_setData     = 0;
MmGetLobbyDataFn      g_getData     = 0;
MmRequestLobbyDataFn  g_reqData     = 0;
MmGetLobbyOwnerFn     g_getOwner    = 0;
MmGetNumMembersFn     g_getNum      = 0;
MmGetMemberByIndexFn  g_getMember   = 0;
FrGetCountFn          g_frCount     = 0;
FrGetByIndexFn        g_frByIndex   = 0;
FrGetNameFn           g_frName      = 0;
FrGetStateFn          g_frState     = 0;
FrGetGamePlayedFn     g_frGame      = 0;
FrGetSelfNameFn       g_frSelfName  = 0;
FrGetAvatarFn         g_frAvatar    = 0;
FrReqUserInfoFn       g_frReqInfo   = 0;
void*                 g_utils       = 0; // ISteamUtils* (avatars; optional)
UtGetImageSizeFn      g_utImgSize   = 0;
UtGetImageRGBAFn      g_utImgRGBA   = 0;
RegisterCbFn          g_regCb       = 0;
RegisterCrFn          g_regCr       = 0;
UnregisterCbFn        g_unregCb     = 0;
RunCallbacksFn        g_runCb       = 0;

bool      g_ready     = false;
ConnectFn g_onConnect = 0;

// Invite/lobby state (main thread only).
unsigned long long g_lobby        = 0;    // our active lobby (0 = none)
bool               g_weAreHost    = false;// created the lobby (vs joined a friend's)
bool               g_hostFired    = false;// host session started for the first arrival
// Host: lobby members already handed to the session (first one starts it, the
// rest join the running tunnel).
unsigned long long g_admitted[MAX_PLAYERS];
int                g_admittedN    = 0;
bool               g_hostReady    = false;// host's world is live: admit lobby members

} // namespace

void setHostReady(bool ready) {
    if (ready == g_hostReady) return;
    g_hostReady = ready;
    coop::logLine(ready ? "[invite] host READY (world live) - lobby members will be let in"
                        : "[invite] host NOT ready (menu/loading) - lobby members wait");
}

namespace {
bool               g_creating     = false;// CreateLobby call in flight
bool               g_watch        = false;// panel open: keep the friend list fresh
unsigned long long g_pendingInvitee = 0;  // friend clicked before the lobby existed
unsigned long long g_launchLobby  = 0;    // "+connect_lobby <id>" from the command line
char               g_status[128]  = {0};
// Host: last "kc_ready" value written to lobby data (-1 = not yet published).
int                g_readyPublished = -1;
// Host: member count last logged (-1 = none yet), for lobby membership logs.
int                g_lastMemberCount = -1;
// Join: the host we will connect to once its lobby says it is ready (0 = none).
unsigned long long g_joinOwner    = 0;
bool               g_joinWaitLogged = false;
// Friends invited into the current lobby (the window shows "invited" for them).
const int          MAX_INVITED = 32;
unsigned long long g_invited[MAX_INVITED];
int                g_invitedN     = 0;
// Bumped on every successful lobby entry as the joining player.
unsigned           g_joinedSerial = 0;

void noteInvited(unsigned long long id) {
    for (int i = 0; i < g_invitedN; ++i) if (g_invited[i] == id) return;
    if (g_invitedN < MAX_INVITED) g_invited[g_invitedN++] = id;
}

// Display name for logs/status (Steam caches names of friends + lobby members).
const char* personaName(unsigned long long id) {
    const char* n = (g_frName && id) ? g_frName(g_friends, id) : 0;
    return (n && n[0]) ? n : "?";
}

// Cached friend list for the panel (our own copy - GetFriendPersonaName returns a
// Steam-owned pointer only valid until the next call). Offline friends are never
// stored, so the cap only ever drops surplus ONLINE friends. Sorted hosting >
// in Kenshi > online. state: 1 online, 2 in Kenshi, 3 hosting co-op.
struct FriendRow {
    unsigned long long id;
    unsigned long long lobby;   // state 3 only: the friend's joinable lobby
    char               name[64];
    int                state;
    int                version; // state 3 only: 1 same protocol, -1 different, 0 unknown
};
const int          MAX_FRIENDS = 64;
FriendRow          g_friendRows[MAX_FRIENDS];
int                g_friendN     = 0;
unsigned int       g_lastFriendRefresh = 0;

SteamCallback g_cbLobbyCreated;
SteamCallback g_cbLobbyEnter;
SteamCallback g_cbJoinRequested;
SteamCallback g_cbP2PRequest;

void steamLog(const char* msg) {
    char b[256];
    _snprintf(b, sizeof(b) - 1, "[invite] %s", msg ? msg : "");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
void setStatus(const char* s) {
    _snprintf(g_status, sizeof(g_status) - 1, "%s", s ? s : "");
    g_status[sizeof(g_status) - 1] = '\0';
}

const FriendRow* findFriend(unsigned long long id) {
    for (int i = 0; i < g_friendN; ++i) if (g_friendRows[i].id == id) return &g_friendRows[i];
    return 0;
}

// Compare a lobby's advertised co-op protocol with ours. Lobby data for a lobby
// we are not in arrives asynchronously after RequestLobbyData; until then the
// value is empty and the answer is "unknown" (0) - the request is repeated on
// each refresh while it stays unknown.
int lobbyVersion(unsigned long long lobby) {
    if (lobby == 0 || !g_getData) return 0;
    const char* v = g_getData(g_mm, lobby, "kc_protocol");
    if (!v || !v[0]) {
        if (g_reqData) g_reqData(g_mm, lobby);
        return 0;
    }
    return ((unsigned)atoi(v) == (unsigned)coop::PROTOCOL_VERSION) ? 1 : -1;
}

// (Re)build the friend cache: enumerate immediate friends, skip offline ones,
// copy names into our own buffers, tag state (3 = hosting co-op, 2 = in Kenshi,
// 1 = online), and sort so the useful ones surface first. A Kenshi friend with a
// lobby is hosting co-op: Kenshi has no lobbies of its own, and friend game info
// only carries a lobby id when that lobby is joinable by us.
void refreshFriends() {
    g_friendN = 0;
    if (!g_frCount || !g_frByIndex || !g_frName || !g_frState) return;
    int n = g_frCount(g_friends, k_EFriendFlagImmediate);
    for (int i = 0; i < n && g_friendN < MAX_FRIENDS; ++i) {
        unsigned long long fid = g_frByIndex(g_friends, i, k_EFriendFlagImmediate);
        if (fid == 0) continue;
        if (g_frState(g_friends, fid) == 0) continue; // k_EPersonaStateOffline
        FriendRow& r = g_friendRows[g_friendN];
        r.id = fid;
        r.lobby = 0;
        r.version = 0;
        r.state = 1;
        FriendGameInfo_t gi;
        std::memset(&gi, 0, sizeof(gi));
        if (g_frGame && g_frGame(g_friends, fid, &gi) &&
            (unsigned)(gi.m_gameID & 0xFFFFFFu) == KENSHI_APPID) {
            r.state = 2;
            if (gi.m_steamIDLobby != 0) {
                r.state   = 3;
                r.lobby   = gi.m_steamIDLobby;
                r.version = lobbyVersion(r.lobby);
            }
        }
        const char* nm = g_frName(g_friends, fid);
        _snprintf(r.name, sizeof(r.name) - 1, "%s", nm ? nm : "(unknown)");
        r.name[sizeof(r.name) - 1] = '\0';
        ++g_friendN;
    }
    // Insertion sort by state descending (small N, stable for equal states).
    for (int i = 1; i < g_friendN; ++i) {
        FriendRow key = g_friendRows[i];
        int j = i - 1;
        while (j >= 0 && g_friendRows[j].state < key.state) { g_friendRows[j + 1] = g_friendRows[j]; --j; }
        g_friendRows[j + 1] = key;
    }
}

// Enter a lobby as the joining player. Leaves a lobby we were hosting first:
// accepting an invite while our own lobby is open must not leave it dangling
// (a friend could still enter it and get hosted for).
void enterLobby(unsigned long long lobby) {
    if (g_lobby != 0 && g_lobby != lobby && g_leaveLobby) {
        g_leaveLobby(g_mm, g_lobby);
        steamLog("left our own lobby to join a friend's");
    }
    g_lobby     = 0;
    g_weAreHost = false;
    g_hostFired = false;
    g_admittedN = 0;
    g_creating  = false;
    g_pendingInvitee = 0;
    g_joinOwner = 0;
    g_joinWaitLogged = false;
    g_invitedN = 0;
    g_joinLobby(g_mm, lobby);
}

// ---- Callback handlers (main thread, via our SteamAPI_RunCallbacks pump) -----

void onLobbyCreated(LobbyCreated_t* r, bool ioFailure) {
    g_creating = false;
    if (ioFailure || !r || r->m_eResult != k_EResultOK) {
        steamLog("lobby creation FAILED");
        setStatus("Could not open a Steam lobby - try again, or use Advanced");
        return;
    }
    g_lobby     = r->m_ulSteamIDLobby;
    g_weAreHost = true;
    g_hostFired = false;
    g_admittedN = 0;
    g_readyPublished = -1;   // publish kc_ready on the next tick
    g_lastMemberCount = -1;
    char pv[16];
    _snprintf(pv, sizeof(pv) - 1, "%u", (unsigned)coop::PROTOCOL_VERSION);
    pv[sizeof(pv) - 1] = '\0';
    if (g_setData) {
        g_setData(g_mm, g_lobby, "kc_protocol", pv);
        g_setData(g_mm, g_lobby, "kc_game", "KenshiCoop");
    }
    char b[96];
    _snprintf(b, sizeof(b) - 1, "lobby created %llu (protocol %s)", g_lobby, pv);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    // If a friend was picked before the lobby existed, invite them now.
    if (g_pendingInvitee != 0) {
        unsigned long long who = g_pendingInvitee;
        g_pendingInvitee = 0;
        if (g_inviteToLobby) g_inviteToLobby(g_mm, g_lobby, who);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "sent lobby invite to %llu", who);
        c[sizeof(c) - 1] = '\0';
        steamLog(c);
        setStatus("Invite sent - waiting for your friend to accept...");
    } else {
        setStatus("Open to friends - invite one below, or they Join from their F2");
    }
}

void onGameLobbyJoinRequested(GameLobbyJoinRequested_t* r) {
    if (!r || !g_joinLobby) return;
    // The player clicked a friend's invite / Steam "Join Game". Enter their lobby;
    // the actual connect happens in onLobbyEnter once membership + owner are known.
    char b[96];
    _snprintf(b, sizeof(b) - 1, "join requested for lobby %llu - entering...",
              r->m_steamIDLobby);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    setStatus("Joining friend's game...");
    enterLobby(r->m_steamIDLobby);
}

void onLobbyEnter(LobbyEnter_t* r) {
    if (!r) return;
    unsigned long long lobby = r->m_ulSteamIDLobby;
    if (r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "could not enter lobby %llu (response %u)",
                  lobby, r->m_EChatRoomEnterResponse);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
        setStatus("Could not join - the game is full or was closed");
        return;
    }
    g_lobby = lobby;
    unsigned long long owner = g_getOwner ? g_getOwner(g_mm, lobby) : 0;
    SteamId self = (SteamId)coop::steamp2p::selfId();

    if (owner == 0) { steamLog("entered lobby but owner unknown"); return; }
    if (owner == self) {
        // We are the host entering our own lobby - wait for the friend (tick()).
        steamLog("entered own lobby - waiting for friend to join");
        return;
    }

    // Joiner path: the owner is the host. Gate on protocol version.
    if (g_getData) {
        const char* hp = g_getData(g_mm, lobby, "kc_protocol");
        if (hp && hp[0]) {
            unsigned hv = (unsigned)atoi(hp);
            if (hv != (unsigned)coop::PROTOCOL_VERSION) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "protocol mismatch: host v%u, you v%u - update the mod on both sides",
                          hv, (unsigned)coop::PROTOCOL_VERSION);
                b[sizeof(b) - 1] = '\0';
                steamLog(b);
                setStatus("Different mod version than the host - both install the same release");
                if (g_leaveLobby) g_leaveLobby(g_mm, lobby);
                g_lobby = 0;
                return;
            }
        }
    }

    coop::steamp2p::accept(owner); // pre-accept the host's P2P session
    // Connect only once the host's world is live (its "kc_ready" lobby flag);
    // tick() polls it. Until then we sit in the lobby - no connect spam.
    g_joinOwner = owner;
    ++g_joinedSerial;
    g_joinWaitLogged = false;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "entered lobby of host %llu (%s) - waiting for host ready",
              owner, personaName(owner));
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    _snprintf(b, sizeof(b) - 1, "In %s's lobby - waiting for them to start the game",
              personaName(owner));
    b[sizeof(b) - 1] = '\0';
    setStatus(b);
}

void onP2PSessionRequest(P2PSessionRequest_t* r) {
    if (!r) return;
    // Accept any inbound session so the tunnel opens regardless of which side
    // sends first; the tunnel itself drops packets from anyone but its peer.
    coop::steamp2p::accept(r->m_steamIDRemote);
}

void dispatch(int iCallback, void* param, bool ioFailure) {
    switch (iCallback) {
        case cb_LobbyCreated:           onLobbyCreated((LobbyCreated_t*)param, ioFailure); break;
        case cb_LobbyEnter:             onLobbyEnter((LobbyEnter_t*)param); break;
        case cb_GameLobbyJoinRequested: onGameLobbyJoinRequested((GameLobbyJoinRequested_t*)param); break;
        case cb_P2PSessionRequest:      onP2PSessionRequest((P2PSessionRequest_t*)param); break;
        default: break;
    }
}

void* getIface(void* client, GetHSteamFn getUser, GetHSteamFn getPipe,
               ClientGetIfaceFn get, const char* const* versions, int n, const char** used) {
    void* p = 0;
    for (int i = 0; i < n && p == 0; ++i) {
        p = get(client, getUser(), getPipe(), versions[i]);
        if (p != 0 && used) *used = versions[i];
    }
    return p;
}

// Steam launches a game with "+connect_lobby <lobbyid64>" when the player accepts
// a lobby invite while the game is not running. Remember it; tick() joins once
// Steam is up. Absent (or not forwarded by the launcher chain) => no-op.
unsigned long long parseLaunchLobby() {
    const char* cmd = GetCommandLineA();
    if (!cmd) return 0;
    const char* p = strstr(cmd, "+connect_lobby");
    if (!p) return 0;
    p += strlen("+connect_lobby");
    while (*p == ' ' || *p == '\t' || *p == '"') ++p;
    return _strtoui64(p, 0, 10);
}

// Kick off the async friends-only 2-player lobby if we don't have one yet.
void createLobbyIfNeeded() {
    if (g_lobby != 0 || g_creating) return;
    g_creating = true;
    steamLog("creating friends-only co-op lobby...");
    SteamAPICall_t call = g_createLobby(g_mm, k_ELobbyTypeFriendsOnly, (int)MAX_PLAYERS);
    if (call == 0) {
        g_creating = false;
        steamLog("CreateLobby returned no call handle");
        setStatus("Could not open a Steam lobby - try again, or use Advanced");
        return;
    }
    // Re-arm the LobbyCreated call-result object for this specific async call.
    if (g_unregCb) g_unregCb(&g_cbLobbyCreated);
    g_cbLobbyCreated.arm(cb_LobbyCreated, sizeof(LobbyCreated_t));
    g_regCr(&g_cbLobbyCreated, call, cb_LobbyCreated);
}

} // namespace

// ---- Public API --------------------------------------------------------------

bool init(ConnectFn onConnect) {
    if (g_ready) { if (onConnect) g_onConnect = onConnect; return true; }
    g_onConnect = onConnect;

    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (mod == 0) { steamLog("init deferred: steam_api64.dll not loaded"); return false; }

    GetHSteamFn      getUser   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamUser");
    GetHSteamFn      getPipe   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamPipe");
    SteamClientFn    getClient = (SteamClientFn)   GetProcAddress(mod, "SteamClient");
    ClientGetIfaceFn getMmIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    ClientGetIfaceFn getFrIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamFriends");

    g_createLobby = (MmCreateLobbyFn)      GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_CreateLobby");
    g_joinLobby   = (MmJoinLobbyFn)        GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_JoinLobby");
    g_leaveLobby  = (MmLeaveLobbyFn)       GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    g_inviteToLobby = (MmInviteToLobbyFn)  GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    g_setData     = (MmSetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    g_getData     = (MmGetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    g_reqData     = (MmRequestLobbyDataFn) GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    g_getOwner    = (MmGetLobbyOwnerFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    g_getNum      = (MmGetNumMembersFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    g_getMember   = (MmGetMemberByIndexFn) GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    g_frCount     = (FrGetCountFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendCount");
    g_frByIndex   = (FrGetByIndexFn)       GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendByIndex");
    g_frName      = (FrGetNameFn)          GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_frState     = (FrGetStateFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    g_frGame      = (FrGetGamePlayedFn)    GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    // Lobby window extras (names + avatars). Optional: missing ones only cost
    // the avatar pictures / own name, never the lobby itself.
    g_frSelfName  = (FrGetSelfNameFn)      GetProcAddress(mod, "SteamAPI_ISteamFriends_GetPersonaName");
    g_frAvatar    = (FrGetAvatarFn)        GetProcAddress(mod, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    g_frReqInfo   = (FrReqUserInfoFn)      GetProcAddress(mod, "SteamAPI_ISteamFriends_RequestUserInformation");
    g_utImgSize   = (UtGetImageSizeFn)     GetProcAddress(mod, "SteamAPI_ISteamUtils_GetImageSize");
    g_utImgRGBA   = (UtGetImageRGBAFn)     GetProcAddress(mod, "SteamAPI_ISteamUtils_GetImageRGBA");
    ClientGetUtilsFn getUtIf = (ClientGetUtilsFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamUtils");
    g_regCb       = (RegisterCbFn)         GetProcAddress(mod, "SteamAPI_RegisterCallback");
    g_regCr       = (RegisterCrFn)         GetProcAddress(mod, "SteamAPI_RegisterCallResult");
    g_unregCb     = (UnregisterCbFn)       GetProcAddress(mod, "SteamAPI_UnregisterCallback");
    g_runCb       = (RunCallbacksFn)       GetProcAddress(mod, "SteamAPI_RunCallbacks");

    if (!getUser || !getPipe || !getClient || !getMmIf || !getFrIf || !g_createLobby ||
        !g_joinLobby || !g_setData || !g_getData || !g_getOwner || !g_getNum ||
        !g_getMember || !g_inviteToLobby || !g_frCount || !g_frByIndex || !g_frName ||
        !g_frState || !g_regCb || !g_regCr || !g_runCb) {
        steamLog("init FAILED: invite/lobby flat exports missing");
        return false;
    }

    void* client = getClient();
    if (client == 0) { steamLog("init FAILED: SteamClient() null (Steam not running?)"); return false; }
    if (getUser() == 0 || getPipe() == 0) { steamLog("init FAILED: no Steam user/pipe"); return false; }

    static const char* const mmVers[] = { "SteamMatchMaking009", "SteamMatchMaking008", "SteamMatchMaking010" };
    static const char* const frVers[] = { "SteamFriends015", "SteamFriends014", "SteamFriends017", "SteamFriends016" };
    const char* usedMm = 0; const char* usedFr = 0;
    g_mm      = getIface(client, getUser, getPipe, getMmIf, mmVers, 3, &usedMm);
    g_friends = getIface(client, getUser, getPipe, getFrIf, frVers, 4, &usedFr);
    if (g_mm == 0)      { steamLog("init FAILED: no ISteamMatchmaking interface"); return false; }
    if (g_friends == 0) { steamLog("init FAILED: no ISteamFriends interface"); return false; }

    // Register the persistent (non-call-result) callbacks. LobbyCreated is a call
    // result, registered per-CreateLobby in createLobbyIfNeeded().
    g_cbLobbyEnter.arm(cb_LobbyEnter, sizeof(LobbyEnter_t));
    g_cbJoinRequested.arm(cb_GameLobbyJoinRequested, sizeof(GameLobbyJoinRequested_t));
    g_cbP2PRequest.arm(cb_P2PSessionRequest, sizeof(P2PSessionRequest_t));
    g_regCb(&g_cbLobbyEnter, cb_LobbyEnter);
    g_regCb(&g_cbJoinRequested, cb_GameLobbyJoinRequested);
    g_regCb(&g_cbP2PRequest, cb_P2PSessionRequest);

    if (getUtIf) {
        static const char* const utVers[] = { "SteamUtils009", "SteamUtils008", "SteamUtils007", "SteamUtils010" };
        for (int i = 0; i < 4 && g_utils == 0; ++i) g_utils = getUtIf(client, getPipe(), utVers[i]);
    }
    if (!g_utils || !g_frAvatar || !g_utImgSize || !g_utImgRGBA)
        steamLog("avatars unavailable (ISteamUtils / avatar exports missing)");

    g_launchLobby = parseLaunchLobby();

    g_ready = true;
    char b[192];
    _snprintf(b, sizeof(b) - 1, "ready: mm=%s friends=%s launchLobby=%llu",
              usedMm ? usedMm : "?", usedFr ? usedFr : "?", g_launchLobby);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    return true;
}

bool ready() { return g_ready; }

void hostLobby() {
    if (!g_ready) { steamLog("hostLobby: Steam invite layer not ready"); setStatus("Steam unavailable - use Advanced"); return; }
    createLobbyIfNeeded();
    if (g_creating) setStatus("Opening your game to friends...");
    steamLog("host requested (friends-only lobby)");
}

bool hostingLobby() { return g_creating || (g_weAreHost && g_lobby != 0); }

void inviteFriend(SteamId id) {
    if (!g_ready || id == 0) return;
    createLobbyIfNeeded(); // no-op if already have one / creating
    noteInvited(id);
    const FriendRow* f = findFriend(id);
    const char* nm = f ? f->name : "friend";
    if (g_lobby != 0 && g_weAreHost && g_inviteToLobby) {
        g_inviteToLobby(g_mm, g_lobby, id);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "sent lobby invite to %llu (%s)", id, nm);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
    } else {
        // Lobby still being created; send the invite the moment it exists.
        g_pendingInvitee = id;
        steamLog("lobby not ready yet - invite queued");
    }
    char s[128];
    _snprintf(s, sizeof(s) - 1, "Invited %s - waiting for them to accept...", nm);
    s[sizeof(s) - 1] = '\0';
    setStatus(s);
}

bool    inLobby()   { return g_ready && g_lobby != 0; }
bool    lobbyHost() { return g_ready && g_lobby != 0 && g_weAreHost; }
SteamId lobbyOwner() {
    if (!g_ready || g_lobby == 0) return 0;
    if (g_weAreHost) return (SteamId)coop::steamp2p::selfId();
    return g_getOwner ? g_getOwner(g_mm, g_lobby) : 0;
}
int lobbyMemberCount() {
    return (g_ready && g_lobby != 0 && g_getNum) ? g_getNum(g_mm, g_lobby) : 0;
}
SteamId lobbyMember(int i) {
    if (!g_ready || g_lobby == 0 || !g_getMember || i < 0) return 0;
    return g_getMember(g_mm, g_lobby, i);
}

void nameOf(SteamId id, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (!g_ready || id == 0) return;
    const char* n = 0;
    if (id == (SteamId)coop::steamp2p::selfId() && g_frSelfName) n = g_frSelfName(g_friends);
    if ((!n || !n[0]) && g_frName) n = g_frName(g_friends, id);
    if (!n || !n[0] || (n[0] == '[' && strcmp(n, "[unknown]") == 0)) {
        // Not a friend and not cached yet (a lobby member we have never seen):
        // ask Steam once; the name shows up on a later refresh.
        if (g_frReqInfo) g_frReqInfo(g_friends, id, false);
        return;
    }
    _snprintf(out, cap - 1, "%s", n);
    out[cap - 1] = '\0';
}

// Log each user's avatar fetch outcome when it changes (first 24 users only):
// enough to see why a picture stays blank without flooding the log.
static void avatarTrace(SteamId id, int state, int img) {
    static SteamId s_ids[24];
    static int     s_state[24];
    static int     s_n = 0;
    int i = 0;
    while (i < s_n && s_ids[i] != id) ++i;
    if (i == s_n) {
        if (s_n >= 24) return;
        s_ids[s_n] = id; s_state[s_n] = -99; ++s_n;
    }
    if (s_state[i] == state) return;
    s_state[i] = state;
    static const char* const what[] = { "ok", "loading (-1)", "none cached (0)", "size failed", "rgba failed" };
    char b[128];
    _snprintf(b, sizeof(b) - 1, "avatar %llu: %s image=%d", id, what[state], img);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
}

bool avatarRGBA(SteamId id, unsigned char* out, int cap, int* w, int* h) {
    if (!g_ready || id == 0 || !out || !g_utils || !g_frAvatar || !g_utImgSize || !g_utImgRGBA)
        return false;
    const int img = g_frAvatar(g_friends, id);
    if (img == -1) { avatarTrace(id, 1, img); return false; } // downloading; asked already
    if (img == 0) {                       // none cached: request the persona
        avatarTrace(id, 2, img);
        if (g_frReqInfo) g_frReqInfo(g_friends, id, false);
        return false;
    }
    unsigned int iw = 0, ih = 0;
    if (!g_utImgSize(g_utils, img, &iw, &ih) || iw == 0 || ih == 0 || iw > 256 || ih > 256) {
        avatarTrace(id, 3, img);
        return false;
    }
    const int bytes = (int)(iw * ih * 4);
    if (bytes > cap || !g_utImgRGBA(g_utils, img, out, bytes)) { avatarTrace(id, 4, img); return false; }
    avatarTrace(id, 0, img);
    if (w) *w = (int)iw;
    if (h) *h = (int)ih;
    return true;
}

bool invited(SteamId id) {
    for (int i = 0; i < g_invitedN; ++i) if (g_invited[i] == id) return true;
    return false;
}

unsigned joinedSerial() { return g_joinedSerial; }

void joinFriend(SteamId id) {
    if (!g_ready || id == 0) return;
    const FriendRow* f = findFriend(id);
    char b[128];
    if (!f || f->state != 3 || f->lobby == 0) {
        _snprintf(b, sizeof(b) - 1, "%s is not hosting right now", f ? f->name : "That friend");
        b[sizeof(b) - 1] = '\0';
        setStatus(b);
        steamLog("join refused: friend has no joinable lobby");
        return;
    }
    if (f->version < 0) {
        setStatus("Different mod version than the host - both install the same release");
        steamLog("join refused: host lobby advertises a different protocol");
        return;
    }
    _snprintf(b, sizeof(b) - 1, "joining %llu (%s) via lobby %llu", f->id, f->name, f->lobby);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    _snprintf(b, sizeof(b) - 1, "Joining %s...", f->name);
    b[sizeof(b) - 1] = '\0';
    setStatus(b);
    enterLobby(f->lobby);
}

void watchFriends(bool on) {
    if (on == g_watch) return;
    g_watch = on;
    g_lastFriendRefresh = 0; // refresh on the very next tick when opened
    if (!on) g_friendN = 0;
}

int         friendCount()      { return g_friendN; }
SteamId     friendId(int i)    { return (i >= 0 && i < g_friendN) ? g_friendRows[i].id : 0; }
const char* friendName(int i)  { return (i >= 0 && i < g_friendN) ? g_friendRows[i].name : ""; }
int         friendState(int i) { return (i >= 0 && i < g_friendN) ? g_friendRows[i].state : 0; }
int         friendVersion(int i) { return (i >= 0 && i < g_friendN) ? g_friendRows[i].version : 0; }

void tick() {
    if (!g_ready) return;
    // Drive the Steam callback pump ourselves: Kenshi does not call
    // SteamAPI_RunCallbacks (its async CreateLobby result never dispatched until
    // we added this), so our invite/lobby/join/P2P callbacks + call-results only
    // fire from here. Safe even if the game did pump - RunCallbacks just drains
    // the message queue, so sequential main-thread calls can't double-deliver a
    // single posted callback. Main thread only (this runs inside mainLoop_hook).
    if (g_runCb) g_runCb();

    // Accepted an invite while the game was closed: join that lobby once.
    if (g_launchLobby != 0) {
        unsigned long long lobby = g_launchLobby;
        g_launchLobby = 0;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "launch argument +connect_lobby %llu - entering...", lobby);
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
        setStatus("Joining friend's game...");
        enterLobby(lobby);
    }

    // Refresh the friend list every ~3 s while the panel is open (online / in
    // Kenshi / hosting status changes over time).
    if (g_watch) {
        unsigned int now = GetTickCount();
        if (g_lastFriendRefresh == 0 || now - g_lastFriendRefresh >= 3000) {
            g_lastFriendRefresh = now;
            refreshFriends();
        }
    }

    // Host: publish readiness on the lobby (joins connect only once it is "1"),
    // and log every membership change with names.
    if (g_weAreHost && g_lobby != 0 && g_setData && g_readyPublished != (g_hostReady ? 1 : 0)) {
        g_readyPublished = g_hostReady ? 1 : 0;
        g_setData(g_mm, g_lobby, "kc_ready", g_hostReady ? "1" : "0");
        steamLog(g_hostReady ? "lobby flag kc_ready=1 published" : "lobby flag kc_ready=0 published");
    }
    if (g_weAreHost && g_lobby != 0 && g_getNum) {
        int n = g_getNum(g_mm, g_lobby);
        SteamId self = (SteamId)coop::steamp2p::selfId();
        if (n != g_lastMemberCount) {
            g_lastMemberCount = n;
            char b[256];
            int len = _snprintf(b, sizeof(b) - 1, "lobby members=%d:", n);
            for (int i = 0; i < n && len > 0 && len < (int)sizeof(b) - 40; ++i) {
                unsigned long long m = g_getMember(g_mm, g_lobby, i);
                len += _snprintf(b + len, sizeof(b) - 1 - len, " %s%s", personaName(m), m == self ? "(you)" : "");
            }
            b[sizeof(b) - 1] = '\0';
            steamLog(b);
            if (!g_hostReady && n > 1) {
                char s[128];
                _snprintf(s, sizeof(s) - 1, "%d friend(s) in your lobby - they join once your game is loaded", n - 1);
                s[sizeof(s) - 1] = '\0';
                setStatus(s);
            }
        }
        // Admission: whenever a friend is in the lobby and our world is live,
        // the first arrival starts the host session, later ones join the
        // running tunnel. (Avoids needing the LobbyChatUpdate callback.)
        for (int i = 0; g_hostReady && i < n; ++i) {
            unsigned long long m = g_getMember(g_mm, g_lobby, i);
            if (m == 0 || m == self) continue;
            bool known = false;
            for (int k = 0; k < g_admittedN; ++k) if (g_admitted[k] == m) { known = true; break; }
            if (known || g_admittedN >= (int)MAX_PLAYERS) continue;
            g_admitted[g_admittedN++] = m;
            coop::steamp2p::accept(m);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "admitting %llu (%s) - %s", m, personaName(m),
                      g_hostFired ? "adding to running session" : "starting host session");
            b[sizeof(b) - 1] = '\0';
            steamLog(b);
            if (!g_hostFired) {
                g_hostFired = true;
                setStatus("Friend joined - hosting...");
                if (g_onConnect) g_onConnect(true, true, m);
            } else {
                coop::steamp2p::addPeer(m);
                setStatus("Another friend joined");
            }
        }
    }

    // Join: connect once the host's lobby says its world is live.
    if (g_joinOwner != 0 && g_lobby != 0 && g_getData) {
        const char* r = g_getData(g_mm, g_lobby, "kc_ready");
        if (r && r[0] == '1') {
            unsigned long long owner = g_joinOwner;
            g_joinOwner = 0;
            char b[128];
            _snprintf(b, sizeof(b) - 1, "host %llu (%s) is ready - connecting as CLIENT",
                      owner, personaName(owner));
            b[sizeof(b) - 1] = '\0';
            steamLog(b);
            setStatus("Connecting to host...");
            if (g_onConnect) g_onConnect(false, true, owner);
        } else if (!g_joinWaitLogged) {
            g_joinWaitLogged = true;
            steamLog("host not ready yet (kc_ready != 1) - waiting in lobby");
        }
    }
}

const char* status() { return g_status; }

void reset() {
    if (g_ready && g_lobby != 0 && g_leaveLobby) g_leaveLobby(g_mm, g_lobby);
    g_lobby     = 0;
    g_weAreHost = false;
    g_hostFired = false;
    g_admittedN = 0;
    g_readyPublished = -1;
    g_lastMemberCount = -1;
    g_joinOwner = 0;
    g_joinWaitLogged = false;
    g_creating  = false;
    g_pendingInvitee = 0;
    g_lastFriendRefresh = 0;
    g_status[0] = '\0';
    g_invitedN = 0;
    if (g_ready && g_unregCb) g_unregCb(&g_cbLobbyCreated);
}

} // namespace steaminvite
} // namespace coop
