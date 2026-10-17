// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 (buttons are the only reliably interactive
// DatapanelGUI controls; MyGUI comboboxes/editboxes have no usable RVAs and don't
// receive keyboard focus during gameplay). Two views:
//   simple   - Host (opens a Steam lobby) + one-click Invite buttons for online
//              friends, or one-click Join buttons for friends who are hosting;
//              Leave / Stop hosting. No Steam IDs anywhere.
//   advanced - the manual fallback: Role / Transport / Online toggles, plus
//              "Copy my Steam ID" / "Paste friend's Steam ID" by clipboard
//              (per-session, never written to disk). The UDP endpoint (ip/port)
//              still comes from coop_config.json.
// The GUI layer stays session-agnostic: live status is passed IN via *st and
// the user's actions are handed BACK through *act (the plugin root owns the
// session/config/Steam wiring). Main-thread only; SEH-guarded.

// One online Steam friend for the simple view's Invite / Join lists.
struct CoopPanelFriend {
    unsigned long long id;
    const char*        name;    // UTF-8 persona name
    int                state;   // 1 online, 2 in Kenshi, 3 hosting co-op
    int                version; // hosting only: 1 same mod protocol, -1 different, 0 unknown
};

struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        detail;      // one-line status string for the panel/overlay
    // Join-side save-transfer status (null when not streaming): byte-level
    // progress the one-line detail above has no room for, shown on the F2 panel
    // while a join receives the host's world (e.g. "Streaming host world... 42%
    // (3.1/7.4 MB)"). Set by coopPanelDrive, rendered in dbgVal.
    const char*        transferDetail;
    bool               inWorld;      // a game is loaded (hosting needs one)
    int                playerCount;  // players in the session incl. us (1 = alone)
    bool               steamInvites; // Steam lobby/invite layer is up
    bool               lobbyHosting; // we own an open lobby (or one is opening)
    const char*        inviteStatus; // lobby/invite status line ("" when idle)
    const CoopPanelFriend* friends;  // online friends, hosting > in Kenshi > online
    int                friendCount;
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect; the UDP endpoint is re-read from the config there.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId);
typedef void (*CoopDisconnectFn)();
typedef void (*CoopFriendFn)(unsigned long long friendId);
struct CoopPanelActions {
    CoopConnectFn    connect;    // advanced view: Online toggle
    CoopDisconnectFn disconnect; // Leave / Stop hosting / Offline toggle
    void           (*host)();    // simple view: open this game to friends
    CoopFriendFn     invite;     // send a Steam invite to a friend
    CoopFriendFn     join;       // join a friend who is hosting
};
void coopPanelTick(const CoopPanelState* st, const CoopPanelActions* act);

// True while the F2 panel is open (the plugin keeps the friend list fresh then).
bool coopPanelIsOpen();

// Persistent co-op connection-status banner: a single screen-space label fixed 10
// px in from the top-left corner (a createFloatingLabel MyGUI::Window on the
// spike-48 screenshot-proven "Info" layer) whose caption shows the live session
// status, colored by state (0 = offline/red, 1 = waiting/yellow, 2 =
// connected/green). Needs no player character, so it also shows at the title
// screen; updated in place when the text/state changes and re-minted if the GUI
// destroyed the widget (world load). Pass show=false to remove it. Main-thread
// only; SEH-guarded.
void coopOverlayTick(const char* text, int state, bool show);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
