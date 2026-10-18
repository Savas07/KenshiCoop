// SteamInvite - Steam lobby based hosting, invites and joining (no Steam IDs typed).
//
// The two-code exchange (SteamP2P's setPeer) is callback-free but forces players
// to copy/paste 17-digit SteamIDs. This module layers Steam's lobby flow on top
// of the SAME P2P tunnel so nobody handles an ID:
//
//   HOST : hostLobby() creates a 2-player friends-only Steam lobby. Friends see
//          it three ways: the F2 panel's "Join a friend" list (friend game info
//          carries the lobby id), Steam's own friends-list "Join Game", and a
//          direct invite sent from the panel (inviteFriend). When the friend
//          enters the lobby the host learns their SteamID from lobby membership
//          and connects.
//   JOIN : joinFriend() (panel button), an accepted Steam invite / "Join Game"
//          (GameLobbyJoinRequested) or a "+connect_lobby <id>" launch argument
//          enters the lobby; the owner (= host SteamID) is read and the join
//          connects as the client.
//
// Everything runs on the MAIN thread: tick() drives the Steam callback pump
// itself (SteamAPI_RunCallbacks) because Kenshi does not - its async CreateLobby
// result never dispatched until we added the call. This is safe even if the game
// also pumped: RunCallbacks just drains the message queue, so sequential
// main-thread calls can't double-deliver a single posted callback. tick() also
// polls lobby membership. The resolved peer is handed back through the ConnectFn
// (the plugin's coopUiConnect), which reuses the normal Steam-transport connect
// path. Manual ID entry stays as a fallback (the panel's advanced view).

#ifndef KENSHICOOP_STEAMINVITE_H
#define KENSHICOOP_STEAMINVITE_H

namespace coop {
namespace steaminvite {

typedef unsigned long long SteamId;

// Fired (on the main thread, from the Steam callback pump / tick) when an invite
// resolves a peer. Matches Plugin.cpp's coopUiConnect(isHost, useSteam, peerId).
typedef void (*ConnectFn)(bool isHost, bool useSteam, SteamId peerId);

// Resolve ISteamMatchmaking/ISteamFriends from the game's steam_api64.dll and
// register the invite/lobby/P2P Steam callbacks. Idempotent; safe to call every
// frame. Requires steamp2p::init() to have succeeded first (shared Steam client).
// Returns false (and logs why) when the interfaces are unavailable.
bool init(ConnectFn onConnect);
bool ready();

// Host action: open a friends-only 2-player lobby (async) so friends can join
// from the panel or Steam, and so inviteFriend has a lobby to invite into.
void hostLobby();

// True while we own a lobby (or one is being created) - the host is "open".
bool hostingLobby();

// Host: lobby members are only admitted (session started / tunnel peer added)
// while this is true - i.e. once the host's world is live. Opening the lobby
// and inviting can happen at the main menu; friends wait in the lobby until
// the host has started or loaded its game (never during character creation).
void setHostReady(bool ready);

// Send a direct Steam lobby invite to a specific friend. The friend gets a Steam
// notification; clicking it fires GameLobbyJoinRequested on their side and
// auto-joins. If the lobby is still being created, the invite is queued and
// sent on completion.
void inviteFriend(SteamId id);

// Join action: enter the lobby a friend is hosting (from the friend list's game
// info). Refuses (with a status line) when the friend is not hosting or runs a
// different co-op protocol version.
void joinFriend(SteamId id);

// Friend list for the panel (main thread). Refreshed every ~3 s while watching
// (the panel is open); cleared otherwise. Offline friends are skipped. Names are
// cached in our own buffers (Steam's GetFriendPersonaName pointer is transient).
// Sorted hosting > in Kenshi > online.
//   friendState:   1 = online, 2 = playing Kenshi, 3 = hosting co-op (joinable)
//   friendVersion: hosting friends only - 1 = same protocol, -1 = different,
//                  0 = unknown yet (lobby data still arriving)
void        watchFriends(bool on);
int         friendCount();
SteamId     friendId(int i);
const char* friendName(int i);
int         friendState(int i);
int         friendVersion(int i);

// Main-thread housekeeping: pumps Steam callbacks, refreshes the friend list,
// honors a pending launch-argument lobby, and polls host-side lobby membership
// for the arriving friend. Call every frame.
void tick();

// One-line human status for the panel/overlay (never null; "" when idle).
const char* status();

// ---- Lobby view for the main-menu lobby window (main thread) ----------------
// The lobby we are in (as host or joining player): owner + members straight
// from Steam lobby membership, so the window shows exactly who is in the game.
bool        inLobby();          // in a lobby (host or join)
bool        lobbyHost();        // ... and we own it
SteamId     lobbyOwner();       // 0 when not in a lobby
int         lobbyMemberCount(); // members incl. us (0 = no lobby)
SteamId     lobbyMember(int i);
// UTF-8 persona name of a user Steam knows (friend, lobby member, ourselves),
// copied into out ("" when unknown).
void        nameOf(SteamId id, char* out, int cap);
// The user's Steam medium avatar as tightly packed RGBA (64x64 on Steam).
// False while Steam is still downloading it (the first miss asks Steam for it)
// or when the user has none - call again later. out must hold w*h*4 bytes.
bool        avatarRGBA(SteamId id, unsigned char* out, int cap, int* w, int* h);
// We sent this friend an invite into the current lobby.
bool        invited(SteamId id);
// Bumped each time we enter a friend's lobby as the joining player (accepted
// invite, Join button, launch argument) - the menu opens the lobby window then.
unsigned    joinedSerial();

// Leave any lobby and clear invite state (called on disconnect / teardown).
void reset();

} // namespace steaminvite
} // namespace coop

#endif // KENSHICOOP_STEAMINVITE_H
