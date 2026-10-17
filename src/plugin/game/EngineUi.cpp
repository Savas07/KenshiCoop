// EngineUi.cpp - the in-game co-op UI plane: the debug marker HUD labels, the
// F2 co-op session panel (native DatapanelGUI), and the persistent status
// overlay. Split out of EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so
// the entity capture/resolve/apply TU stays focused on sync and the UI/render
// surface lives with its public header (EngineUi.h).
//
// Owner state: section-private statics / anon-namespace SEH shims only (marker
// colour + create/update/destroy shims, panel widget pointers, overlay label).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them), install
// hooks, or change any log string - "[coop-ui] ..." phrasing is API consumed by
// the harness. The public marker* declarations stay in Engine.h (the Replicator
// uses them for KENSHICOOP_DEBUG_MARKERS); only their definitions moved here.

#include "EngineInternal.h"

// In-game co-op session panel: the native DatapanelGUI window + its interactive
// rows and Win32 key capture. EngineInternal.h already pulls Globals.h (::gui),
// ForgottenGUI.h and MyGUI_Button.h; these add the panel row types, the
// free-function delegate factory, and GetAsyncKeyState/VK_* for the F2 toggle +
// digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>
#include <deque>
#include <vector>

#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)
#include "../core/Version.h" // KENSHICOOP_VERSION (panel title)

namespace coop {
namespace engine {

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is this label one the GUI still owns? ForgottenGUI keeps the authoritative
// registry of live ScreenLabels (plus deferred add/remove queues), and that
// registry - not our cached pointer - is the only ground truth available: the
// GUI destroys labels on its own schedule and notifies nobody, so a handle held
// across ticks dangles silently. Measured 2026-08-03: the join minted three
// proxies in one burst, debugMark found stale map entries sitting at RECYCLED
// Character* addresses, and setCaption on the previous occupants' dead labels
// faulted twice inside ScreenLabel::setCaption (kenshi+0x6e451e) moments before
// the process died. Same lesson as the world-item proxy hands - never
// dereference a cached engine pointer without re-asking the engine.
//
// Deliberately unlocked. guiScreenLabelsMutex guards these lektors, but taking
// an engine shared_mutex from inside a detour is its own class of hazard, and a
// torn read here is harmless BECAUSE we test for an exact pointer match: garbage
// answers "not present", which costs one discarded marker, whereas a false
// "alive" would need the torn word to equal the very pointer we are asking
// about. The safe direction is the likely one.
bool labelListHas(const lektor<ScreenLabelInterface*>* v, const void* l) {
    __try {
        ScreenLabelInterface* const* p = v->stuff;
        unsigned int n = v->count;
        if (!p || n > 8192u) return false;   // bound a torn/garbage count
        for (unsigned int i = 0; i < n; ++i)
            if ((const void*)p[i] == l) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerAlive(void* label) {
    if (!label) return false;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    // Queued for removal counts as dead: the GUI has already decided, and we
    // would only be racing its next flush.
    if (labelListHas(&g->guiScreenLabelsToRemove, label)) return false;
    // A freshly created label sits in the add queue until the GUI flushes it, so
    // both lists are "alive" or markerCreate's own handle would look dead.
    return labelListHas(&g->guiScreenLabels, label) ||
           labelListHas(&g->guiScreenLabelsToAdd, label);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text || !markerAlive(label)) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    // Handing the GUI a label it has already destroyed is the same
    // use-after-free as updating one; prune paths reach here with handles whose
    // Character went away, which is exactly when the GUI has cleaned up too.
    if (!markerAlive(label)) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

// ---- In-game co-op session panel (spike-50 DatapanelGUI stack) ---------------
// A native DatapanelGUI window toggled with F2, built only from BUTTON and text
// rows (buttons are the only DatapanelGUI control with a callable RVA callback;
// MyGUI comboboxes/editboxes have no reachable getters and never receive keyboard
// focus during gameplay). The simple view hosts / invites / joins through the
// Steam lobby layer with no IDs; the advanced view keeps the manual Role /
// Transport / Online toggles and the clipboard Steam-ID exchange. The GUI layer is
// session-agnostic: live status arrives via *st; the user's actions leave via
// *act (the plugin root owns the net/session/config/Steam wiring).
//
// Button callbacks run inside MyGUI's input dispatch, so they never act directly:
// they queue a PanelAction that the next coopPanelTick executes. That keeps a
// rebuild (which clears every row) from destroying the very button whose callback
// is still on the stack.
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The rows are therefore described by a POD RowSpec
// array whose strings are owned by the outer frame. The one exception is
// createDatapanel's BY-VALUE std::string 'layer' arg (an unwindable temporary =>
// C2712), so the window is created in the outer, non-SEH function; ::gui is
// verified non-null first and the createScreenLabel/createFloatingLabel factory
// family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          advanced;      // advanced (manual Steam ID / LAN) view shown
    bool          hostFlag;      // advanced: true = HOST role armed
    bool          steamFlag;     // advanced: true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    int           page;          // simple view: friend-list page
    unsigned int  lastHash;      // fingerprint of everything the rows show (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), advanced(false), hostFlag(true),
          steamFlag(true), connectedFlag(false), lastConnected(false),
          lastChkVal(false), needsRebuild(false), f2Down(false), page(0),
          lastHash(0) {}
};

CoopPanelUi             g_panel;
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Friend's SteamID pasted in-panel this session (0 = none). Per-session by
// design: it lives only in memory, so relaunching Kenshi clears it and the
// friend's id is re-pasted (nothing is written to disk). Passed to onConnect,
// where it overrides the (usually empty) config steamPeer.
unsigned long long      g_pastedPeer   = 0;
bool                    g_pasteFailed  = false; // last paste wasn't a valid Steam ID

// Simple-view button actions, queued by onActionBtn and run by coopPanelTick.
enum PanelAction {
    PA_NONE, PA_HOST, PA_INVITE, PA_JOIN, PA_LEAVE, PA_STOP_HOST,
    PA_MORE, PA_ADVANCED, PA_SIMPLE
};

// Friend rows shown per page in the simple view (keeps the window a fixed size).
const int FRIENDS_PER_PAGE = 6;

// The live action buttons of the current build: the callback gets only the
// DataPanelLine*, so it maps the pressed line back to its action + friend here.
struct ButtonRow { DataPanelLine* line; int action; unsigned long long friendId; };
const int          MAX_BUTTON_ROWS = 32;
ButtonRow          g_btnRows[MAX_BUTTON_ROWS];
int                g_btnRowN = 0;
int                g_pendingAction = PA_NONE;
unsigned long long g_pendingFriend = 0;

void onActionBtn(DataPanelLine* line) {
    for (int i = 0; i < g_btnRowN; ++i) {
        if (g_btnRows[i].line == line) {
            g_pendingAction = g_btnRows[i].action;
            g_pendingFriend = g_btnRows[i].friendId;
            return;
        }
    }
}

// Advanced-view button callbacks (free functions - MyGUI::newDelegate wraps them
// without any raw-MyGUI link). A press flips the armed flag and requests a
// rebuild so the caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Paste the friend's SteamID from the clipboard: read text, extract + validate a
// SteamID64, and store it as the session peer (used on the next Connect). No
// typing, no config edit. Rejects arbitrary clipboard junk (g_pasteFailed drives
// the peer-row hint).
void onPasteIdBtn(DataPanelLine*) {
    std::string clip;
    unsigned long long id = 0;
    if (clipboardGetText(clip) && coop::parseSteamId64(clip, id)) {
        g_pastedPeer  = id;
        g_pasteFailed = false;
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste friend id=%llu ok=1", id);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    } else {
        g_pasteFailed = true;
        coop::logLine("[coop-ui] paste friend id=0 ok=0 (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}

// One panel row. POD so the build SEH frame holds no unwindable object; the
// strings are owned by the outer frame's RowList.
enum RowKind { ROW_BUTTON, ROW_LINE, ROW_SPACE };
struct RowSpec {
    int                kind;
    const std::string* key;      // unique per panel (DatapanelGUI keys rows by it)
    const std::string* val;      // button caption / line value
    void             (*cb)(DataPanelLine*); // buttons only
    int                action;   // onActionBtn rows: PanelAction
    unsigned long long friendId; // PA_INVITE / PA_JOIN rows
    bool               amber;    // lines only: tint the value amber (in progress)
    DataPanelLine*     line;     // out: the created row
};

// Row storage for one build (outer frame). std::deque keeps string addresses
// stable while rows are appended.
struct RowList {
    std::deque<std::string> strs;
    std::vector<RowSpec>    rows;
    const std::string* keep(const std::string& s) { strs.push_back(s); return &strs.back(); }
    void add(int kind, const std::string& key, const std::string& val,
             void (*cb)(DataPanelLine*), int action, unsigned long long fid, bool amber) {
        RowSpec r;
        r.kind = kind; r.key = keep(key); r.val = keep(val); r.cb = cb;
        r.action = action; r.friendId = fid; r.amber = amber; r.line = 0;
        rows.push_back(r);
    }
    void button(const std::string& key, const std::string& cap, void (*cb)(DataPanelLine*)) {
        add(ROW_BUTTON, key, cap, cb, PA_NONE, 0, false);
    }
    void action(const std::string& key, const std::string& cap, int act, unsigned long long fid) {
        add(ROW_BUTTON, key, cap, &onActionBtn, act, fid, false);
    }
    void line(const std::string& key, const std::string& val, bool amber) {
        add(ROW_LINE, key, val, 0, PA_NONE, 0, amber);
    }
    void space() { add(ROW_SPACE, std::string(), std::string(), 0, PA_NONE, 0, false); }
};

struct PanelBuild {
    const std::string* title;
    const std::string* empty;
    RowSpec*           rows;
    int                count;
};

void panelBuildSeh(DatapanelGUI* p, PanelBuild* b) {
    __try {
        p->_NV_clear();
        p->setCaption(*b->title);
        for (int i = 0; i < b->count; ++i) {
            RowSpec& r = b->rows[i];
            if (r.kind == ROW_BUTTON)
                r.line = p->setLineButton(*r.key, *r.val, 0);
            else if (r.kind == ROW_LINE)
                r.line = p->setLine(*r.key, *r.val, *b->empty, 0, false, true);
            else
                p->addSpace(0, 0.35f);
        }
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
// yellow=true tints the value column amber - used for the join's live
// "Streaming host world..." transfer line so it reads as in-progress activity.
void dbgColourSeh(DataPanelLine* line, bool yellow) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        MyGUI::Colour amber(1.0f, 0.82f, 0.20f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(yellow ? amber : white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Arm a freshly-minted panel: register it for ForgottenGUI's per-frame refresh
// AND make it visible. createDatapanel returns a built-but-hidden window; without
// this pair the F2 toggle logs open/close yet nothing ever draws (the render bug
// in the reconstruction). PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        g->addDatapanelToUpdateList(p);
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Panel content ----------------------------------------------------------

// FNV-1a over everything the rows display: the rows are rebuilt only when this
// fingerprint moves, so an open panel costs no allocation on a quiet frame.
unsigned int fnv(unsigned int h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}
unsigned int fnvStr(unsigned int h, const char* s) {
    if (!s) s = "";
    return fnv(h, s, strlen(s) + 1); // include the terminator as a separator
}

unsigned int panelHash(const CoopPanelState* st) {
    unsigned int h = 2166136261u;
    bool flags[11] = { g_panel.advanced, g_panel.hostFlag, g_panel.steamFlag,
                       g_panel.connectedFlag, g_pasteFailed, st->running,
                       st->peerPresent, st->isHost, st->inWorld, st->steamInvites,
                       st->lobbyHosting };
    h = fnv(h, flags, sizeof(flags));
    h = fnv(h, &g_panel.page, sizeof(g_panel.page));
    h = fnv(h, &st->transportSel, sizeof(st->transportSel));
    h = fnv(h, &st->playerCount, sizeof(st->playerCount));
    h = fnv(h, &g_pastedPeer, sizeof(g_pastedPeer));
    h = fnv(h, &st->selfSteamId, sizeof(st->selfSteamId));
    h = fnv(h, &st->peerSteamId, sizeof(st->peerSteamId));
    h = fnvStr(h, st->detail);
    h = fnvStr(h, st->transferDetail);
    h = fnvStr(h, st->inviteStatus);
    for (int i = 0; i < st->friendCount; ++i) {
        const CoopPanelFriend& f = st->friends[i];
        h = fnv(h, &f.id, sizeof(f.id));
        h = fnv(h, &f.state, sizeof(f.state));
        h = fnv(h, &f.version, sizeof(f.version));
        h = fnvStr(h, f.name);
    }
    return h;
}

// Which friend list the simple view shows: 0 none, 1 Invite (hosting, waiting
// for friends, including while a session runs - more can still join), 2 Join
// (idle). The advanced view and a join's live session show none.
int listMode(const CoopPanelState* st) {
    if (g_panel.advanced || !st->steamInvites) return 0;
    if (st->lobbyHosting) return 1;
    return st->running ? 0 : 2;
}

// Friends relevant to a list: everyone online for Invite, only hosts for Join.
bool friendListed(const CoopPanelFriend& f, int mode) {
    return mode == 1 || (mode == 2 && f.state == 3);
}
int listedCount(const CoopPanelState* st, int mode) {
    int n = 0;
    for (int i = 0; i < st->friendCount; ++i) if (friendListed(st->friends[i], mode)) ++n;
    return n;
}

// Persona name as a caption: '#' starts a MyGUI colour tag, so it is doubled
// (MyGUI's literal-'#' escape); long names are cut on a UTF-8 boundary.
std::string displayName(const char* n) {
    const size_t MAX_BYTES = 24;
    size_t len = n ? strlen(n) : 0;
    size_t cut = len;
    if (len > MAX_BYTES) {
        cut = MAX_BYTES;
        while (cut > 0 && ((unsigned char)n[cut] & 0xC0) == 0x80) --cut;
    }
    std::string out;
    for (size_t i = 0; i < cut; ++i) {
        if (n[i] == '#') out += "##";
        else out += n[i];
    }
    if (cut < len) out += "...";
    if (out.empty()) out = "(unnamed)";
    return out;
}

const char* stateLabel(int state) {
    return state == 3 ? "hosting" : (state == 2 ? "in Kenshi" : "online");
}

// One page of Invite / Join buttons (+ a page cycler when they don't fit).
void addFriendPage(RowList& rl, const CoopPanelState* st, int mode) {
    int shown = 0, seen = 0;
    const int first = g_panel.page * FRIENDS_PER_PAGE;
    for (int i = 0; i < st->friendCount && shown < FRIENDS_PER_PAGE; ++i) {
        const CoopPanelFriend& f = st->friends[i];
        if (!friendListed(f, mode)) continue;
        if (seen++ < first) continue;
        char key[16];
        _snprintf(key, sizeof(key) - 1, "friend%d", shown);
        key[sizeof(key) - 1] = '\0';
        std::string cap;
        if (mode == 1) {
            cap = std::string("Invite   ") + displayName(f.name) + "   (" + stateLabel(f.state) + ")";
            rl.action(key, cap, PA_INVITE, f.id);
        } else {
            cap = std::string("Join   ") + displayName(f.name);
            if (f.version < 0) cap += "   - different mod version";
            rl.action(key, cap, PA_JOIN, f.id);
        }
        ++shown;
    }
    int total = listedCount(st, mode);
    int pages = (total + FRIENDS_PER_PAGE - 1) / FRIENDS_PER_PAGE;
    if (pages > 1) {
        char cap[64];
        _snprintf(cap, sizeof(cap) - 1, "More friends   (page %d of %d)", g_panel.page + 1, pages);
        cap[sizeof(cap) - 1] = '\0';
        rl.action("more", cap, PA_MORE, 0);
    }
}

// Simple view: one status line, then Leave (live session) / Invite list + Stop
// hosting (lobby open) / Host + Join list (idle), then the Advanced switch.
// Text rows show only their value column, so each carries its own label.
void buildSimpleRows(const CoopPanelState* st, RowList& rl, const std::string& transfer) {
    std::string text;
    bool amber = false;
    if (!transfer.empty()) {
        text = "World transfer:  " + transfer; amber = true;
    } else if (st->running) {
        char pc[96];
        if (st->peerPresent)
            _snprintf(pc, sizeof(pc) - 1, st->isHost ? "Status:  hosting - %d players in the game"
                                                     : "Status:  connected - %d players in the game",
                      st->playerCount);
        else
            _snprintf(pc, sizeof(pc) - 1, "%s", st->isHost ? "Status:  hosting - waiting for friends..."
                                                           : "Status:  connecting to your friend...");
        pc[sizeof(pc) - 1] = '\0';
        text = pc;
        amber = !st->peerPresent;
    } else if (st->inviteStatus && st->inviteStatus[0]) {
        text = std::string("Status:  ") + st->inviteStatus; amber = true;
    } else if (st->lobbyHosting) {
        text = "Status:  open to friends"; amber = true;
    } else {
        text = "Status:  not connected";
    }
    rl.line("status", text, amber);
    rl.space();

    int mode = listMode(st);
    if (st->running) {
        if (mode == 1) {
            rl.line("invitehdr", "Invite more friends  (they get a Steam invite):", false);
            if (listedCount(st, mode) == 0) rl.line("nofriends", "Nobody on your friends list is online", false);
            else addFriendPage(rl, st, mode);
            rl.space();
        }
        rl.action("leave", "Leave co-op session", PA_LEAVE, 0);
    } else if (!st->steamInvites) {
        rl.line("steam", "Steam invites unavailable - use Advanced below", false);
    } else if (mode == 1) {
        if (!st->inWorld)
            rl.line("loadhint", "Next: start or load your game - friends get in once it's loaded", true);
        rl.line("invitehdr", "Invite a friend  (they get a Steam invite):", false);
        if (listedCount(st, mode) == 0) rl.line("nofriends", "Nobody on your friends list is online", false);
        else addFriendPage(rl, st, mode);
        rl.space();
        rl.action("stophost", "Stop hosting", PA_STOP_HOST, 0);
    } else {
        rl.action("host", st->inWorld ? "Host co-op   -   open this game to friends"
                                      : "Host co-op   -   invite friends, then start your game",
                  PA_HOST, 0);
        rl.space();
        int hosts = listedCount(st, mode);
        rl.line("joinhdr", hosts ? "Join a friend who is hosting:"
                                 : "Join a friend:  nobody is hosting right now", false);
        addFriendPage(rl, st, mode);
    }
    rl.space();
    rl.action("adv", "Advanced   (Steam ID / LAN)", PA_ADVANCED, 0);
}

// Advanced view: the manual role/transport/online toggles + clipboard Steam-ID
// exchange (LAN, or when Steam invites are unavailable).
void buildAdvancedRows(const CoopPanelState* st, RowList& rl, const std::string& transfer) {
    rl.action("back", "<<   Back", PA_SIMPLE, 0);
    rl.space();
    rl.button("role",  std::string("Role: ") + (g_panel.hostFlag ? "HOST" : "JOIN") + "    (switch)", &onRoleBtn);
    rl.button("trans", std::string("Transport: ") + (g_panel.steamFlag ? "STEAM" : "UDP") + "    (switch)", &onTransBtn);
    rl.button("conn",  std::string("Connection: ") + (g_panel.connectedFlag ? "ONLINE" : "OFFLINE") + "    (switch)", &onConnBtn);
    rl.space();

    // Describes the live connection state + type. Reflects the ACTUAL running
    // session when online; the armed toggles when offline. A join streaming the
    // host's world at the menu has no leader for the screen overlay, so the live
    // progress replaces it (amber).
    std::string transStr = (st->transportSel == 0) ? "Steam" : "UDP";
    std::string dbgVal;
    if (st->running) {
        if (st->peerPresent)
            dbgVal = (st->isHost ? std::string("Hosting") : std::string("Joining")) +
                     " over " + transStr + " - peer connected";
        else if (st->isHost)
            dbgVal = std::string("Hosting over ") + transStr + " - waiting for peer...";
        else
            dbgVal = std::string("Joining over ") + transStr + " - connecting to host...";
    } else {
        dbgVal = std::string("Offline - will ") + (g_panel.hostFlag ? "host" : "join") +
                 " over " + (g_panel.steamFlag ? "Steam" : "UDP") + " on Connect";
    }
    if (!transfer.empty()) rl.line("dbg", "World transfer:  " + transfer, true);
    else                   rl.line("dbg", "Status:  " + dbgVal, false);
    rl.space();

    // Friend's SteamID: prefer the value pasted in-panel this session; fall back
    // to the config (steamPeer). Both id rows show only the last 4 digits - the
    // panel is often on screen while streaming. Nothing needs the full digits by
    // eye: Copy puts the real id on the clipboard and Paste takes it back off.
    std::string peerVal;
    unsigned long long peerShown = g_pastedPeer ? g_pastedPeer : st->peerSteamId;
    if (peerShown != 0)      peerVal = coop::maskSteamId64(peerShown);
    else if (g_pasteFailed)  peerVal = "(clipboard was not a Steam ID - copy theirs and retry)";
    else                     peerVal = "(click Paste friend's Steam ID)";
    rl.line("peer", "Friend's Steam ID:  " + peerVal, false);
    rl.button("pasteid", "Paste friend's Steam ID", &onPasteIdBtn);
    rl.space();
    rl.line("self", std::string("Your Steam ID:  ") +
                    (st->selfSteamId ? coop::maskSteamId64(st->selfSteamId)
                                     : std::string("(Steam not running)")), false);
    rl.button("copyid", "Copy my Steam ID", &onCopyIdBtn);
}

// Execute the button press queued by onActionBtn (see the header note on why
// callbacks never act directly).
void runPendingAction(const CoopPanelActions* act) {
    int a = g_pendingAction;
    unsigned long long fid = g_pendingFriend;
    g_pendingAction = PA_NONE;
    g_pendingFriend = 0;
    char b[96];
    switch (a) {
    case PA_HOST:
        coop::logLine("[coop-ui] host -> open Steam lobby");
        if (act->host) act->host();
        break;
    case PA_INVITE:
        _snprintf(b, sizeof(b) - 1, "[coop-ui] invite friend=%llu", fid);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (act->invite) act->invite(fid);
        break;
    case PA_JOIN:
        _snprintf(b, sizeof(b) - 1, "[coop-ui] join friend=%llu", fid);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (act->join) act->join(fid);
        break;
    case PA_LEAVE:
        // The Online/Offline edge below performs the disconnect.
        coop::logLine("[coop-ui] leave requested");
        g_panel.connectedFlag = false;
        break;
    case PA_STOP_HOST:
        coop::logLine("[coop-ui] stop hosting");
        if (act->disconnect) act->disconnect();
        break;
    case PA_MORE:
        ++g_panel.page;
        break;
    case PA_ADVANCED:
        g_panel.advanced = true;
        coop::logLine("[coop-ui] view -> advanced");
        break;
    case PA_SIMPLE:
        g_panel.advanced = false;
        coop::logLine("[coop-ui] view -> simple");
        break;
    default:
        return;
    }
    g_panel.needsRebuild = true;
}

} // namespace

void coopPanelTick(const CoopPanelState* st, const CoopPanelActions* act) {
    if (!st || !act) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // F2 rising edge toggles the panel open/closed.
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !g_panel.f2Down) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_panel.page          = 0;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            panelDestroySeh(g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_btnRowN = 0;
            g_pendingAction = PA_NONE;
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    if (!g_panel.open) return;

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a lobby-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the rows reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    runPendingAction(act);

    // Friends come and go: keep the page inside the current list.
    {
        int mode = listMode(st);
        int n = mode ? listedCount(st, mode) : 0;
        int pages = n ? (n + FRIENDS_PER_PAGE - 1) / FRIENDS_PER_PAGE : 1;
        if (g_panel.page >= pages) g_panel.page = 0;
    }

    unsigned int hash = panelHash(st);
    if (hash != g_panel.lastHash) g_panel.needsRebuild = true;

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        g_panel.panel = g->createDatapanel(0.22f, 0.30f, 0.30f, 0.50f, false, layer, true);
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else if (!uiPanelArmSeh(g, g_panel.panel)) {
            coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
        }
    }

    // (Re)populate the rows when anything visible changed.
    if (g_panel.panel && (g_panel.needsRebuild || !g_panel.built)) {
        std::string transfer = st->transferDetail ? std::string(st->transferDetail)
                                                   : std::string();
        RowList rl;
        if (g_panel.advanced) buildAdvancedRows(st, rl, transfer);
        else                  buildSimpleRows(st, rl, transfer);
        std::string title = "Co-op Session  v" KENSHICOOP_VERSION "    -    F2 to close";
        std::string empty;
        PanelBuild pb;
        pb.title = &title;
        pb.empty = &empty;
        pb.rows  = rl.rows.empty() ? 0 : &rl.rows[0];
        pb.count = (int)rl.rows.size();
        g_btnRowN = 0;
        panelBuildSeh(g_panel.panel, &pb);

        // Delegate assignment + colouring live OUTSIDE the SEH frame (pointer
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        for (int i = 0; i < pb.count; ++i) {
            const RowSpec& r = rl.rows[i];
            if (!r.line) continue;
            if (r.kind == ROW_BUTTON && r.cb) {
                r.line->callback = MyGUI::newDelegate(r.cb);
                if (r.cb == &onActionBtn && g_btnRowN < MAX_BUTTON_ROWS) {
                    g_btnRows[g_btnRowN].line     = r.line;
                    g_btnRows[g_btnRowN].action   = r.action;
                    g_btnRows[g_btnRowN].friendId = r.friendId;
                    ++g_btnRowN;
                }
            } else if (r.kind == ROW_LINE) {
                dbgColourSeh(r.line, r.amber);
            }
        }

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastHash = hash;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend id (0 if none) is handed to the plugin, which lets a non-zero
    // value override the config steamPeer; UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp");
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            if (act->connect) act->connect(g_panel.hostFlag, g_panel.steamFlag, g_pastedPeer);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (act->disconnect) act->disconnect();
        }
    }
}

bool coopPanelIsOpen() { return g_panel.open; }

// ---- Persistent co-op status overlay ----------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// Unlike the character-tracked ScreenLabel this replaces, it needs no player
// character and holds its place while the camera moves - which also makes it
// visible at the title screen, where a join has no leader while it streams the
// host's world. Removed when show=false.
//
// Two widgets, because neither factory alone does the job. Measured 2026-08-04:
// createFloatingLabel hands back a bare MyGUI::Window that IS visible and
// layer-attached at the coords we ask for, but its skin carries no text region at
// all - setCaption is silently dropped (getCaption().size() stays 0) and it has no
// children, so it draws nothing. It is still the only way to get a widget parented
// to a screen layer instead of to another window, so we keep it as an invisible
// container and put Kenshi's own label factory inside it (createLabelAbs ->
// MyGUI::TextBox with a text-bearing skin, the one the datapanel rows use).
// Caption + colour go to the child; the container is only geometry.

namespace {
// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox   = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay      = 0; // the label that actually draws the text
int             g_overlayState = -1;
std::string     g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// dbgColourSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(MyGUI::Window* box, const std::string* text) {
    __try {
        box->setCoord(kOverlayX, kOverlayY, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = ::gui->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
                                                  *text, MyGUI::Align::Left);
        if (l) {
            l->setTextAlign(MyGUI::Align::Left);
            l->setVisible(true);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Caption + colour in place. MyGUI::UString owns a buffer (destructor => C2712),
// so the caller builds it outside this frame and passes a pointer. false = the
// widget faulted; the caller then treats the pointer as dead.
bool overlayUpdateSeh(MyGUI::TextBox* l, const MyGUI::UString* text,
                      const MyGUI::Colour* col) {
    __try {
        l->setCaption(*text);
        l->setTextColour(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Destroying the container takes its label child with it (MyGUI owns the subtree).
void overlayDestroySeh(ForgottenGUI* g, MyGUI::Window* box) {
    __try { g->destroyWidget(box); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

void coopOverlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel above; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); g_overlayBox = 0; }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            coop::logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g_overlayBox, &t);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (!g_overlay) {
            coop::logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently (same lesson as the ScreenLabel registry
            // note above). Forget them and re-mint on the next tick.
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
    }
}

} // namespace engine
} // namespace coop
