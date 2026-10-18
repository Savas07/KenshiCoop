// EngineLobby.cpp - the main-menu Multiplayer lobby (contract in EngineUi.h): a
// MULTIPLAYER button injected into Kenshi's title menu and the lobby window it
// opens. Plain MyGUI (MyGUIEngine_x64.dll exports), parented to the title
// screen's background image: Kenshi's own New Game / Load windows live on the
// higher "Window" layer and draw above it, and it hides with the menu.
//
// Widget lifetime: MyGUI owns every widget and the game may destroy the whole
// title screen (our button + window with it) whenever it likes. A WidgetManager
// unlinker hears about every destruction and forgets our pointers, so nothing
// here is ever used after MyGUI freed it.
//
// Clicks never act inside MyGUI's input dispatch: the handler only records the
// button's action string and the next lobbyTick runs it - a rebuild must not
// destroy the button whose callback is still on the stack (the F2 panel rule).

#define _CRT_SECURE_NO_WARNINGS 1

#include "EngineInternal.h"
#include "EngineUi.h"

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_WidgetManager.h>
#include <mygui/MyGUI_IUnlinkWidget.h>
#include <mygui/MyGUI_RenderManager.h>
#include <mygui/MyGUI_ITexture.h>
#include <mygui/MyGUI_ImageBox.h>
#include <mygui/MyGUI_TextBox.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_TabControl.h>

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../core/Version.h"
#include "../../netproto/Wire.h" // coop::MAX_PLAYERS

namespace coop {
namespace engine {
namespace {

// ---- MyGUI singletons --------------------------------------------------------
// MyGUIEngine exports each Singleton<T>::msInstance as DATA (the import library
// only has the __imp_ symbol), but the MSVC branch of MyGUI_Singleton.h does not
// declare the template dllimport, so the header's getInstance() cannot link from
// a plugin. Read the exported pointer variables directly.
void** myguiData(const char* sym) {
    HMODULE m = GetModuleHandleA("MyGUIEngine_x64.dll");
    return m ? (void**)GetProcAddress(m, sym) : 0;
}
MyGUI::Gui* myGui() {
    static void** p = myguiData("?msInstance@?$Singleton@VGui@MyGUI@@@MyGUI@@0PEAVGui@2@EA");
    return p ? (MyGUI::Gui*)*p : 0;
}
MyGUI::WidgetManager* myWidgets() {
    static void** p = myguiData("?msInstance@?$Singleton@VWidgetManager@MyGUI@@@MyGUI@@0PEAVWidgetManager@2@EA");
    return p ? (MyGUI::WidgetManager*)*p : 0;
}
MyGUI::RenderManager* myRender() {
    static void** p = myguiData("?msInstance@?$Singleton@VRenderManager@MyGUI@@@MyGUI@@0PEAVRenderManager@2@EA");
    return p ? (MyGUI::RenderManager*)*p : 0;
}

void lobbyLog(const std::string& s) { coop::logLine(("[lobby] " + s).c_str()); }

// ---- Look ---------------------------------------------------------------------
const char* const SKIN_PANEL  = "Kenshi_FloatingPanelSkin";
const char* const SKIN_CARD   = "Kenshi_GenericTextBoxFlat";
const char* const SKIN_TEXT   = "Kenshi_TextboxStandardText";
const char* const SKIN_TITLE  = "Kenshi_TextboxPaintedText";
const char* const SKIN_BUTTON = "Kenshi_Button1";
const char* const FONT_TITLE  = "Kenshi_PaintedTextFont_Large";
const char* const FONT_HEAD   = "Kenshi_PaintedTextFont_Medium";
const char* const FONT_BUTTON = "Kenshi_PaintedTextFont_Small";
const char* const FONT_TEXT   = "Kenshi_StandardFont_Medium";
const char* const FONT_SMALL  = "Kenshi_StandardFont_Small";

enum Col { COL_WHITE, COL_BEIGE, COL_GREY, COL_GREEN, COL_AMBER, COL_RED };
MyGUI::Colour colour(int c) {
    switch (c) {
    case COL_BEIGE: return MyGUI::Colour(0.93f, 0.86f, 0.70f, 1.0f);
    case COL_GREY:  return MyGUI::Colour(0.66f, 0.66f, 0.66f, 1.0f);
    case COL_GREEN: return MyGUI::Colour(0.50f, 0.95f, 0.50f, 1.0f);
    case COL_AMBER: return MyGUI::Colour(1.00f, 0.80f, 0.25f, 1.0f);
    case COL_RED:   return MyGUI::Colour(1.00f, 0.45f, 0.40f, 1.0f);
    default:        return MyGUI::Colour(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

// MyGUI reads '#' as the start of a colour tag: double it. Long names are cut
// on a UTF-8 boundary.
std::string esc(const char* s, size_t maxBytes) {
    std::string out;
    if (!s) return out;
    const size_t len = strlen(s);
    size_t cut = len;
    if (cut > maxBytes) {
        cut = maxBytes;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    }
    for (size_t i = 0; i < cut; ++i) { if (s[i] == '#') out += '#'; out += s[i]; }
    if (cut < len) out += "...";
    return out;
}

std::string u64str(unsigned long long v) {
    char b[32];
    _snprintf(b, sizeof(b) - 1, "%llu", v);
    b[sizeof(b) - 1] = '\0';
    return b;
}

// ---- UI state -------------------------------------------------------------------
enum View { VIEW_MAIN, VIEW_SAVES };

struct LobbyUi {
    MyGUI::Widget* root;    // title layout root ("<prefix>Root")
    MyGUI::Widget* bg;      // title background image (parent of the menu column)
    MyGUI::Widget* menuBtn; // our MULTIPLAYER button
    MyGUI::Widget* win;     // lobby window frame
    MyGUI::Widget* content; // every dynamic widget lives under this one
    std::string    prefix;  // title layout widget-name prefix
    bool           open;
    int            view;
    int            page;
    unsigned       hash;    // fingerprint of what the content shows
    unsigned       joinedSeen;
    bool           joinRunning; // auto-open edge: a join connection came up
    LobbyUi() : root(0), bg(0), menuBtn(0), win(0), content(0), open(false),
                view(VIEW_MAIN), page(0), hash(0), joinedSeen(0), joinRunning(false) {}
};
LobbyUi g_ui;

// Live action buttons of the current build (click -> action; the test driver
// finds buttons by action too).
struct ActionButton { MyGUI::Widget* w; std::string action; };
std::vector<ActionButton> g_buttons;
std::string g_pending; // clicked action, run by the next lobbyTick

// New Game window driver: the start to preselect once Kenshi's window is up.
std::string g_ngTarget;
int         g_ngWaitTicks = 0;

class LobbyUnlinker : public MyGUI::IUnlinkWidget {
public:
    virtual void _unlinkWidget(MyGUI::Widget* w) {
        if (w == g_ui.content) { g_ui.content = 0; g_buttons.clear(); }
        if (w == g_ui.win)     { g_ui.win = 0; g_ui.content = 0; g_buttons.clear(); }
        if (w == g_ui.menuBtn) g_ui.menuBtn = 0;
        if (w == g_ui.bg)      { g_ui.bg = 0; g_ui.win = 0; g_ui.content = 0; g_ui.menuBtn = 0; g_buttons.clear(); }
        if (w == g_ui.root)    g_ui.root = 0;
    }
};
LobbyUnlinker g_unlinker;
bool          g_unlinkerOn = false;

void onActionClick(MyGUI::Widget* w) {
    for (size_t i = 0; i < g_buttons.size(); ++i)
        if (g_buttons[i].w == w) { g_pending = g_buttons[i].action; return; }
}
void onMenuClick(MyGUI::Widget*) { g_pending = "open"; }

// Find a widget of one of Kenshi's loaded layouts. Layout widgets are named
// <prefix><name> with a per-instance prefix and every layout root is
// "<prefix>Root". 'also' must exist in the same layout (names like CancelButton
// are shared by several layouts).
MyGUI::Widget* findLayoutWidget(const char* name, const char* also, MyGUI::Widget** rootOut) {
    MyGUI::Gui* gui = myGui();
    if (!gui) return 0;
    MyGUI::EnumeratorWidgetPtr e = gui->getEnumerator();
    while (e.next()) {
        MyGUI::Widget* r = e.current();
        if (!r) continue;
        const std::string& n = r->getName();
        if (n.size() < 4 || n.compare(n.size() - 4, 4, "Root") != 0) continue;
        const std::string pfx = n.substr(0, n.size() - 4);
        MyGUI::Widget* w = r->findWidget(pfx + name);
        if (!w) continue;
        if (also && !r->findWidget(pfx + also)) continue;
        if (rootOut) *rootOut = r;
        return w;
    }
    return 0;
}

void fireClick(MyGUI::Widget* w) { if (w) w->eventMouseButtonClick(w); }

// ---- Avatars --------------------------------------------------------------------
// Steam's medium avatar uploaded once per user into a MyGUI texture named
// kc_avatar_<steamid>; retried with a growing pause (1 s doubling to 30 s) until
// Steam has it, so users without a picture do not cost a Steam call a second.
struct AvatarTex { bool ready; DWORD lastTry; int misses; };
std::map<unsigned long long, AvatarTex> g_avatars;
unsigned char g_avatarBuf[256 * 256 * 4];

std::string avatarTexture(unsigned long long id) { return "kc_avatar_" + u64str(id); }

bool avatarReady(unsigned long long id, const LobbyActions* act) {
    if (id == 0 || !act->avatar) return false;
    AvatarTex& a = g_avatars[id];
    if (a.ready) return true;
    const DWORD now = GetTickCount();
    const DWORD wait = a.misses >= 5 ? 30000u : (1000u << a.misses);
    if (a.lastTry != 0 && now - a.lastTry < wait) return false;
    a.lastTry = now ? now : 1;
    ++a.misses;
    int w = 0, h = 0;
    if (!act->avatar(id, g_avatarBuf, (int)sizeof(g_avatarBuf), &w, &h)) return false;
    MyGUI::RenderManager* rm = myRender();
    if (!rm) return false;
    const std::string name = avatarTexture(id);
    MyGUI::ITexture* tex = rm->getTexture(name);
    if (!tex) {
        tex = rm->createTexture(name);
        if (!tex) return false;
        tex->createManual(w, h, MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write,
                          MyGUI::PixelFormat::R8G8B8A8);
    }
    if (tex->getWidth() != w || tex->getHeight() != h) return false;
    unsigned char* dst = (unsigned char*)tex->lock(MyGUI::TextureUsage::Write);
    if (!dst) return false;
    // MyGUI's Ogre platform stores R8G8B8A8 as PF_A8R8G8B8: B,G,R,A in memory.
    const unsigned char* src = g_avatarBuf;
    for (int i = 0; i < w * h; ++i, src += 4, dst += 4) {
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
    }
    tex->unlock();
    a.ready = true;
    char b[96];
    _snprintf(b, sizeof(b) - 1, "avatar ready %llu (%dx%d)", id, w, h);
    b[sizeof(b) - 1] = '\0';
    lobbyLog(b);
    return true;
}

// ---- Model: everything the window shows, built fresh each tick ------------------
struct Row {
    unsigned long long id;     // avatar owner (0 = no picture)
    bool               pic;    // avatar texture ready
    std::string        name;   // escaped
    std::string        sub;    // second line
    int                subCol;
    std::string        action; // button action ("" = none)
    std::string        caption;
    bool               enabled;
    Row() : id(0), pic(false), subCol(COL_GREY), enabled(true) {}
};
struct Btn { std::string caption, action; bool enabled; };

struct Model {
    std::string      status;
    int              statusCol;
    std::vector<Row> members;
    std::string      header, empty;
    std::vector<Row> rows;
    std::vector<Btn> left, right;
};

void addBtn(std::vector<Btn>& v, const char* caption, const char* action, bool enabled) {
    Btn b; b.caption = caption; b.action = action; b.enabled = enabled;
    v.push_back(b);
}

const char* friendStateText(int state) {
    return state == 3 ? "hosting KenshiCoop" : (state == 2 ? "playing Kenshi" : "online");
}

void buildModel(const LobbyState* st, const LobbyActions* act, Model& m) {
    m.status    = st->transfer ? st->transfer : (st->status ? st->status : "");
    m.statusCol = st->transfer ? COL_AMBER : (st->peerPresent || st->inLobby ? COL_GREEN : COL_BEIGE);

    for (int i = 0; i < st->memberCount; ++i) {
        const LobbyMember& lm = st->members[i];
        Row r;
        r.id     = lm.steamId;
        r.pic    = avatarReady(lm.steamId, act);
        r.name   = esc(lm.name, 28);
        r.sub    = lm.tag ? lm.tag : "";
        r.subCol = strstr(r.sub.c_str(), "host") ? COL_AMBER
                 : strstr(r.sub.c_str(), "connected") ? COL_GREEN
                 : strstr(r.sub.c_str(), "you") ? COL_BEIGE : COL_GREY;
        m.members.push_back(r);
    }

    if (g_ui.view == VIEW_SAVES) {
        m.header = "LOAD A SAVE - everyone plays in this world";
        m.empty  = "No saves found.";
        for (int i = 0; i < st->saveCount; ++i) {
            Row r;
            r.name    = esc(st->saves[i].display, 40);
            r.sub     = st->saves[i].when ? st->saves[i].when : "";
            r.action  = std::string("load:") + st->saves[i].name;
            r.caption = "LOAD";
            m.rows.push_back(r);
        }
        addBtn(m.left, "BACK", "back", true);
    } else if (st->lobbyHost || st->opening) {
        m.header = "INVITE FRIENDS";
        m.empty  = st->opening ? "Opening your lobby..." : "No friends online right now.";
        for (int i = 0; i < st->friendCount; ++i) {
            const CoopPanelFriend& f = st->friends[i];
            const int inv = st->invited ? st->invited[i] : 0;
            Row r;
            r.id     = f.id;
            r.pic    = avatarReady(f.id, act);
            r.name   = esc(f.name, 28);
            r.sub    = friendStateText(f.state);
            r.subCol = f.state >= 2 ? COL_GREEN : COL_GREY;
            if (inv == 2) {
                r.sub += " - in your lobby";
                r.subCol = COL_GREEN;
            } else {
                if (inv == 1) { r.sub += " - invited"; r.subCol = COL_AMBER; }
                r.action  = "invite:" + u64str(f.id);
                r.caption = inv == 1 ? "AGAIN" : "INVITE";
                r.enabled = st->lobbyHost;
            }
            m.rows.push_back(r);
        }
        addBtn(m.left, "NEW GAME", "newgame", st->lobbyHost);
        addBtn(m.left, "LOAD GAME", "saves", st->lobbyHost);
    } else if (st->inLobby) {
        m.header = "WAITING FOR THE HOST";
        m.empty  = std::string("You are in ") + esc(st->hostName && st->hostName[0] ? st->hostName : "the host", 28) +
                   "'s lobby. You join their world automatically as soon as they start or load a game.";
    } else if (st->running) {
        m.header = "DIRECT SESSION";
        m.empty  = st->isHost
            ? "Hosting a direct (LAN / Steam ID) session. Start or load a game - connected players join it."
            : "Connected to the host - you join their world as soon as it is running.";
        if (st->isHost) {
            addBtn(m.left, "HOST LOBBY", "host", st->steamUp);
            addBtn(m.left, "NEW GAME", "newgame", true);
            addBtn(m.left, "LOAD GAME", "saves", true);
        }
    } else {
        m.header = "FRIENDS HOSTING";
        m.empty  = st->steamUp
            ? "No friends are hosting right now. Host a game and invite them, or wait for their invite."
            : "Steam is not running - start Steam, or use F2 > Advanced for LAN play.";
        for (int i = 0; i < st->friendCount; ++i) {
            const CoopPanelFriend& f = st->friends[i];
            if (f.state != 3) continue;
            Row r;
            r.id      = f.id;
            r.pic     = avatarReady(f.id, act);
            r.name    = esc(f.name, 28);
            r.sub     = f.version < 0 ? "different mod version - update to the same release" : "hosting KenshiCoop";
            r.subCol  = f.version < 0 ? COL_RED : COL_GREEN;
            r.action  = "join:" + u64str(f.id);
            r.caption = "JOIN";
            r.enabled = f.version >= 0;
            m.rows.push_back(r);
        }
        addBtn(m.left, "HOST GAME", "host", st->steamUp);
    }
    if (st->inLobby || st->running) addBtn(m.right, "LEAVE", "leave", true);
    addBtn(m.right, "CLOSE", "close", true);
}

unsigned fnv(unsigned h, const std::string& s) {
    for (size_t i = 0; i <= s.size(); ++i) { h ^= (unsigned char)(i < s.size() ? s[i] : 0); h *= 16777619u; }
    return h;
}
unsigned fnvRow(unsigned h, const Row& r) {
    h = fnv(h, u64str(r.id));
    h = fnv(h, r.pic ? "p" : "-");
    h = fnv(h, r.name); h = fnv(h, r.sub); h = fnv(h, r.action); h = fnv(h, r.caption);
    h = fnv(h, r.enabled ? "e" : "d");
    return h;
}
unsigned modelHash(const Model& m, int w, int h) {
    unsigned x = 2166136261u;
    char b[48];
    _snprintf(b, sizeof(b) - 1, "%d/%d/%d/%d/%d", g_ui.view, g_ui.page, w, h, m.statusCol);
    b[sizeof(b) - 1] = '\0';
    x = fnv(x, b); x = fnv(x, m.status); x = fnv(x, m.header); x = fnv(x, m.empty);
    for (size_t i = 0; i < m.members.size(); ++i) x = fnvRow(x, m.members[i]);
    for (size_t i = 0; i < m.rows.size(); ++i)    x = fnvRow(x, m.rows[i]);
    for (size_t i = 0; i < m.left.size(); ++i)    { x = fnv(x, m.left[i].caption);  x = fnv(x, m.left[i].enabled ? "e" : "d"); }
    for (size_t i = 0; i < m.right.size(); ++i)   { x = fnv(x, m.right[i].caption); x = fnv(x, m.right[i].enabled ? "e" : "d"); }
    return x;
}

// ---- Widgets ----------------------------------------------------------------------
MyGUI::TextBox* addText(MyGUI::Widget* parent, int x, int y, int w, int h, const std::string& caption,
                        const char* skin, const char* font, int col, MyGUI::Align align) {
    MyGUI::TextBox* t = parent->createWidget<MyGUI::TextBox>(skin, MyGUI::IntCoord(x, y, w, h),
                                                             MyGUI::Align::Default);
    if (!t) return 0;
    if (font) t->setFontName(font);
    t->setTextAlign(align);
    t->setTextColour(colour(col));
    t->setCaption(MyGUI::UString(caption));
    t->setNeedMouseFocus(false);
    return t;
}

MyGUI::Widget* addButton(MyGUI::Widget* parent, int x, int y, int w, int h, const std::string& caption,
                         const std::string& action, bool enabled) {
    MyGUI::Button* b = parent->createWidget<MyGUI::Button>(SKIN_BUTTON, MyGUI::IntCoord(x, y, w, h),
                                                           MyGUI::Align::Default);
    if (!b) return 0;
    b->setFontName(FONT_BUTTON);
    b->setCaption(MyGUI::UString(caption));
    b->setEnabled(enabled);
    b->eventMouseButtonClick += MyGUI::newDelegate(&onActionClick);
    ActionButton ab;
    ab.w = b;
    ab.action = action;
    g_buttons.push_back(ab);
    return b;
}

void addAvatar(MyGUI::Widget* parent, int x, int y, int sz, const Row& r) {
    MyGUI::Widget* frame = parent->createWidget<MyGUI::Widget>(SKIN_CARD, MyGUI::IntCoord(x, y, sz, sz),
                                                               MyGUI::Align::Default);
    if (frame) frame->setNeedMouseFocus(false);
    if (!r.pic) return;
    MyGUI::ImageBox* img = parent->createWidget<MyGUI::ImageBox>("ImageBox",
        MyGUI::IntCoord(x + 2, y + 2, sz - 4, sz - 4), MyGUI::Align::Default);
    if (!img) return;
    img->setImageTexture(avatarTexture(r.id));
    img->setNeedMouseFocus(false);
}

const int PAD = 14;

void build(const Model& m) {
    MyGUI::Gui* gui = myGui();
    if (!gui || !g_ui.win) return;
    if (g_ui.content) gui->destroyWidget(g_ui.content); // the unlinker clears content + buttons
    g_ui.content = 0;
    g_buttons.clear();

    const MyGUI::IntCoord cc = g_ui.win->getClientCoord();
    const int W = cc.width, H = cc.height;
    MyGUI::Widget* c = g_ui.win->createWidget<MyGUI::Widget>("PanelEmpty", MyGUI::IntCoord(0, 0, W, H),
                                                             MyGUI::Align::Stretch);
    if (!c) return;
    g_ui.content = c;

    // Title + version.
    addText(c, PAD, 8, W - 2 * PAD, 40, "MULTIPLAYER", SKIN_TITLE, FONT_TITLE, COL_WHITE,
            MyGUI::Align::Left | MyGUI::Align::VCenter);
    addText(c, W - PAD - 200, 18, 200, 24, "KenshiCoop v" KENSHICOOP_VERSION, SKIN_TEXT, FONT_SMALL,
            COL_GREY, MyGUI::Align::Right | MyGUI::Align::VCenter);
    int y = 52;
    addText(c, PAD, y, W - 2 * PAD, 24, m.status, SKIN_TEXT, FONT_TEXT, m.statusCol,
            MyGUI::Align::Left | MyGUI::Align::VCenter);
    y += 32;

    // Players: two columns of cards (avatar, name, role).
    char hb[64];
    _snprintf(hb, sizeof(hb) - 1, "PLAYERS  %d/%d", (int)m.members.size(), (int)MAX_PLAYERS);
    hb[sizeof(hb) - 1] = '\0';
    addText(c, PAD, y, W - 2 * PAD, 26, hb, SKIN_TITLE, FONT_HEAD, COL_BEIGE,
            MyGUI::Align::Left | MyGUI::Align::VCenter);
    y += 30;
    const int cardW = (W - 2 * PAD - 10) / 2, cardH = 52;
    for (size_t i = 0; i < m.members.size() && i < (size_t)MAX_PLAYERS; ++i) {
        const Row& r = m.members[i];
        const int x  = PAD + (int)(i % 2) * (cardW + 10);
        const int cy = y + (int)(i / 2) * (cardH + 6);
        MyGUI::Widget* card = c->createWidget<MyGUI::Widget>(SKIN_CARD, MyGUI::IntCoord(x, cy, cardW, cardH),
                                                             MyGUI::Align::Default);
        if (card) card->setNeedMouseFocus(false);
        addAvatar(c, x + 4, cy + 4, 44, r);
        addText(c, x + 56, cy + 4, cardW - 62, 24, r.name, SKIN_TEXT, FONT_TEXT, COL_WHITE,
                MyGUI::Align::Left | MyGUI::Align::VCenter);
        addText(c, x + 56, cy + 27, cardW - 62, 20, r.sub, SKIN_TEXT, FONT_SMALL, r.subCol,
                MyGUI::Align::Left | MyGUI::Align::VCenter);
    }
    const int memberRows = ((int)m.members.size() + 1) / 2;
    y += (memberRows > 0 ? memberRows : 1) * (cardH + 6) + 8;

    // Section: header (+ pager), rows or the explanatory text.
    const int bottomY = H - 54;
    const int rowH = 46;
    int perPage = (bottomY - (y + 32)) / rowH;
    if (perPage < 1) perPage = 1;
    const int nRows = (int)m.rows.size();
    const int pages = nRows > 0 ? (nRows + perPage - 1) / perPage : 1;
    if (g_ui.page >= pages) g_ui.page = pages - 1;
    if (g_ui.page < 0) g_ui.page = 0;
    addText(c, PAD, y, W - 2 * PAD - 150, 26, m.header, SKIN_TITLE, FONT_HEAD, COL_BEIGE,
            MyGUI::Align::Left | MyGUI::Align::VCenter);
    if (pages > 1) {
        char pb[32];
        _snprintf(pb, sizeof(pb) - 1, "%d/%d", g_ui.page + 1, pages);
        pb[sizeof(pb) - 1] = '\0';
        addButton(c, W - PAD - 146, y, 40, 26, "<", "prev", g_ui.page > 0);
        addText(c, W - PAD - 102, y, 58, 26, pb, SKIN_TEXT, FONT_SMALL, COL_GREY, MyGUI::Align::Center);
        addButton(c, W - PAD - 40, y, 40, 26, ">", "next", g_ui.page + 1 < pages);
    }
    y += 32;
    if (nRows == 0) {
        // Explanations wrap: a read-only static EditBox (Kenshi's word-wrap skin).
        MyGUI::EditBox* t = c->createWidget<MyGUI::EditBox>("Kenshi_WordWrapEmpty",
            MyGUI::IntCoord(PAD, y, W - 2 * PAD, bottomY - y - 8), MyGUI::Align::Default);
        if (t) {
            t->setEditMultiLine(true);
            t->setEditWordWrap(true);
            t->setEditReadOnly(true);
            t->setEditStatic(true);
            t->setFontName(FONT_TEXT);
            t->setTextColour(colour(COL_GREY));
            t->setCaption(MyGUI::UString(m.empty));
            t->setNeedMouseFocus(false);
        }
    }
    const int btnW = 110;
    for (int i = g_ui.page * perPage; i < nRows && i < (g_ui.page + 1) * perPage; ++i) {
        const Row& r = m.rows[i];
        const int ry = y + (i - g_ui.page * perPage) * rowH;
        MyGUI::Widget* card = c->createWidget<MyGUI::Widget>(SKIN_CARD,
            MyGUI::IntCoord(PAD, ry, W - 2 * PAD, rowH - 6), MyGUI::Align::Default);
        if (card) card->setNeedMouseFocus(false);
        int x0 = PAD + 10;
        if (r.id != 0) { addAvatar(c, PAD + 4, ry + 3, 34, r); x0 = PAD + 46; }
        const int textW = W - PAD - x0 - (r.action.empty() ? 10 : btnW + 16);
        addText(c, x0, ry + 2, textW, 20, r.name, SKIN_TEXT, FONT_TEXT, COL_WHITE,
                MyGUI::Align::Left | MyGUI::Align::VCenter);
        addText(c, x0, ry + 21, textW, 18, r.sub, SKIN_TEXT, FONT_SMALL, r.subCol,
                MyGUI::Align::Left | MyGUI::Align::VCenter);
        if (!r.action.empty())
            addButton(c, W - PAD - btnW - 6, ry + 4, btnW, rowH - 14, r.caption, r.action, r.enabled);
    }

    // Bottom bar: context actions left, LEAVE / CLOSE right.
    const int nb = (int)(m.left.size() + m.right.size());
    int bw = nb > 0 ? (W - 2 * PAD - 8 * (nb - 1) - 20) / nb : 120;
    if (bw > 170) bw = 170;
    int bx = PAD;
    for (size_t i = 0; i < m.left.size(); ++i, bx += bw + 8)
        addButton(c, bx, bottomY + 6, bw, 40, m.left[i].caption, m.left[i].action, m.left[i].enabled);
    bx = W - PAD - (int)m.right.size() * (bw + 8) + 8;
    for (size_t i = 0; i < m.right.size(); ++i, bx += bw + 8)
        addButton(c, bx, bottomY + 6, bw, 40, m.right[i].caption, m.right[i].action, m.right[i].enabled);
}

// ---- Title menu button + window frame ---------------------------------------------
// Re-space Kenshi's menu column (CONTINUE ... EXIT keep their end points) to make
// room for MULTIPLAYER right after LOAD GAME.
void ensureMenuButton() {
    if (g_ui.menuBtn) return;
    MyGUI::Widget* root = 0;
    MyGUI::Widget* newGame = findLayoutWidget("NewGameButton", "ExitButton", &root);
    if (!newGame || !root) return;
    const std::string& rn = root->getName();
    const std::string pfx = rn.substr(0, rn.size() - 4);
    MyGUI::Widget* bg     = newGame->getParent();
    MyGUI::Widget* first  = root->findWidget(pfx + "ContinueButton");
    MyGUI::Widget* last   = root->findWidget(pfx + "ExitButton");
    if (!bg || !first || !last) return;
    static const char* const column[8] = {
        "ContinueButton", "NewGameButton", "LoadGameButton", 0,
        "ImportGameButton", "OptionsButton", "CreditsButton", "ExitButton" };
    const MyGUI::IntCoord a = first->getCoord(), z = last->getCoord();
    const float step = (float)(z.top - a.top) / 7.0f;
    MyGUI::Widget* btn = 0;
    for (int i = 0; i < 8; ++i) {
        const int top = a.top + (int)(step * (float)i + 0.5f);
        if (column[i] == 0) {
            MyGUI::Button* b = bg->createWidget<MyGUI::Button>(SKIN_BUTTON,
                MyGUI::IntCoord(a.left, top, a.width, a.height), MyGUI::Align::Default,
                pfx + "KenshiCoopMultiplayerButton");
            if (!b) return;
            b->setFontName(static_cast<MyGUI::TextBox*>(newGame)->getFontName());
            b->setCaption(MyGUI::UString("MULTIPLAYER"));
            b->eventMouseButtonClick += MyGUI::newDelegate(&onMenuClick);
            btn = b;
        } else {
            MyGUI::Widget* w = root->findWidget(pfx + column[i]);
            if (w) w->setPosition(a.left, top);
        }
    }
    g_ui.root = root; g_ui.bg = bg; g_ui.menuBtn = btn; g_ui.prefix = pfx;
    lobbyLog("MULTIPLAYER button added to the title menu");
}

void openWindow(const char* why) {
    if (g_ui.open) return;
    g_ui.open = true;
    g_ui.view = VIEW_MAIN;
    g_ui.page = 0;
    lobbyLog(std::string("window opened (") + why + ")");
}

void closeWindow() {
    g_ui.open = false;
    MyGUI::Gui* gui = myGui();
    if (gui && g_ui.win) gui->destroyWidget(g_ui.win); // unlinker forgets win/content/buttons
    g_ui.win = 0; g_ui.content = 0; g_buttons.clear();
    lobbyLog("window closed");
}

// Open Kenshi's own New Game window; the co-op start sized for the lobby is
// preselected on the next ticks (the host can still pick any start).
void openNewGame(int players) {
    MyGUI::Widget* ng = findLayoutWidget("NewGameButton", "ExitButton", 0);
    if (!ng) { lobbyLog("New Game: title menu button not found"); return; }
    if (players < 2) players = 2;
    if (players > (int)MAX_PLAYERS) players = (int)MAX_PLAYERS;
    char t[48];
    _snprintf(t, sizeof(t) - 1, "Multiplayer (Wanderer x%d)", players);
    t[sizeof(t) - 1] = '\0';
    g_ngTarget = t;
    g_ngWaitTicks = 0;
    fireClick(ng);
    lobbyLog(std::string("New Game window opened, preselecting '") + t + "'");
}

void preselectStart() {
    MyGUI::Widget* root = 0;
    MyGUI::Widget* title = findLayoutWidget("TitleTextBox", "BeginButton", &root);
    MyGUI::Widget* right = 0;
    if (root) {
        const std::string& rn = root->getName();
        right = root->findWidget(rn.substr(0, rn.size() - 4) + "RightButton");
    }
    if (!title || !right || !root->getVisible()) {
        if (++g_ngWaitTicks > 300) { lobbyLog("New Game window never appeared"); g_ngTarget.clear(); }
        return;
    }
    MyGUI::TextBox* tb = static_cast<MyGUI::TextBox*>(title);
    const std::string first = tb->getCaption().asUTF8();
    for (int i = 0; i < 64; ++i) {
        const std::string cur = tb->getCaption().asUTF8();
        if (cur == g_ngTarget) {
            lobbyLog("start preselected: " + cur);
            g_ngTarget.clear();
            return;
        }
        if (i > 0 && cur == first) break; // went all the way round
        fireClick(right);
    }
    lobbyLog("start '" + g_ngTarget + "' not installed - left Kenshi's default selected");
    g_ngTarget.clear();
}

void runAction(const std::string& a, const LobbyState* st, const LobbyActions* act) {
    lobbyLog("click " + a);
    if (a == "open")      { openWindow("menu button"); return; }
    if (a == "close")     { closeWindow(); return; }
    if (a == "host")      { if (act->host) act->host(); return; }
    if (a == "leave")     { if (act->leave) act->leave(); g_ui.view = VIEW_MAIN; g_ui.page = 0; return; }
    if (a == "newgame")   { openNewGame(st->memberCount); return; }
    if (a == "saves")     { g_ui.view = VIEW_SAVES; g_ui.page = 0; return; }
    if (a == "back")      { g_ui.view = VIEW_MAIN; g_ui.page = 0; return; }
    if (a == "prev")      { if (g_ui.page > 0) --g_ui.page; return; }
    if (a == "next")      { ++g_ui.page; return; }
    if (a.compare(0, 7, "invite:") == 0) { if (act->invite) act->invite(_strtoui64(a.c_str() + 7, 0, 10)); return; }
    if (a.compare(0, 5, "join:") == 0)   { if (act->join) act->join(_strtoui64(a.c_str() + 5, 0, 10)); return; }
    if (a.compare(0, 5, "load:") == 0) {
        if (act->loadSave && !act->loadSave(a.c_str() + 5)) lobbyLog("load did not start - try again");
        return;
    }
}

// ---- Test driver (KENSHICOOP_TEST_LOBBY) --------------------------------------------
// Synthetic input does not reach the game window reliably, so tests script the
// lobby through the SAME delegates a mouse click fires: "open" clicks the
// injected MULTIPLAYER button, "begin" Kenshi's New Game BEGIN button, "confirm"
// the host's own New Game character editor as it is, "vanilla=<name>" a widget of
// Kenshi's title menu by layout name (e.g. NewGameButton), any other verb (host,
// newgame, saves, close, leave, load=<save>) the lobby button with that action.
// Steps are ';'-separated, ~3 s apart, each retried up to 30 s until its target
// exists. Invite is deliberately not scriptable (it messages a real Steam friend).
struct TestStep { std::string verb, arg; };
std::vector<TestStep> g_test;
size_t g_testIdx = 0;
DWORD  g_testNext = 0, g_testStepSince = 0;
bool   g_testParsed = false;

void testParse() {
    g_testParsed = true;
    const char* e = getenv("KENSHICOOP_TEST_LOBBY");
    if (!e || !e[0]) return;
    std::string s(e);
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(';', p);
        if (q == std::string::npos) q = s.size();
        std::string item = s.substr(p, q - p);
        if (!item.empty()) {
            TestStep t;
            const size_t eq = item.find('=');
            t.verb = item.substr(0, eq);
            if (eq != std::string::npos) t.arg = item.substr(eq + 1);
            g_test.push_back(t);
        }
        p = q + 1;
    }
    lobbyLog("test script armed: " + s);
    g_testNext = GetTickCount() + 5000;
}

void testTick() {
    if (!g_testParsed) testParse();
    if (g_testIdx >= g_test.size()) return;
    const DWORD now = GetTickCount();
    if ((int)(now - g_testNext) < 0) return;
    if (g_testStepSince == 0) g_testStepSince = now ? now : 1;
    const TestStep& t = g_test[g_testIdx];
    MyGUI::Widget* target = 0;
    if (t.verb == "wait") {
        target = (MyGUI::Widget*)1; // nothing to click
    } else if (t.verb == "confirm") {
        // Kenshi's own New Game character editor (the host's squad): confirm as is.
        if (chargenEditorOpen() && chargenConfirmEditor()) target = (MyGUI::Widget*)1;
    } else if (t.verb == "open") {
        target = g_ui.menuBtn;
        fireClick(target);
    } else if (t.verb == "vanilla") {
        // A widget of Kenshi's own title menu by layout name (e.g. NewGameButton).
        target = findLayoutWidget(t.arg.c_str(), "ExitButton", 0);
        fireClick(target);
    } else if (t.verb == "begin") {
        target = findLayoutWidget("BeginButton", "TitleTextBox", 0);
        if (target && g_ngTarget.empty()) fireClick(target);
        else target = 0; // wait for the preselect to finish
    } else {
        const std::string action = t.verb == "load" ? "load:" + t.arg : t.verb;
        for (size_t i = 0; i < g_buttons.size(); ++i)
            if (g_buttons[i].action == action) { target = g_buttons[i].w; break; }
        if (target) fireClick(target);
    }
    char b[160];
    if (target) {
        _snprintf(b, sizeof(b) - 1, "[lobby-test] step %u '%s%s%s' fired", (unsigned)g_testIdx + 1,
                  t.verb.c_str(), t.arg.empty() ? "" : "=", t.arg.c_str());
    } else if (now - g_testStepSince > 30000) {
        _snprintf(b, sizeof(b) - 1, "[lobby-test] step %u '%s' FAILED (target never appeared)",
                  (unsigned)g_testIdx + 1, t.verb.c_str());
        g_testIdx = g_test.size();
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return;
    } else {
        return;
    }
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    ++g_testIdx;
    g_testStepSince = 0;
    g_testNext = now + 3000;
}

} // namespace

void lobbyTick(const LobbyState* st, const LobbyActions* act) {
    if (!st || !act) return;
    MyGUI::Gui* gui = myGui();
    if (!gui) return;
    if (!g_unlinkerOn) {
        MyGUI::WidgetManager* wm = myWidgets();
        if (!wm) return; // never run without the dangling-pointer guard
        wm->registerUnlinker(&g_unlinker);
        g_unlinkerOn = true;
    }
    ensureMenuButton();

    // Open by itself when we entered a friend's lobby or a join connection came
    // up at the menu: the player should see whose game they are waiting for.
    if (st->joinedSerial != g_ui.joinedSeen) {
        g_ui.joinedSeen = st->joinedSerial;
        openWindow("entered a friend's lobby");
    }
    const bool joinRunning = st->running && !st->isHost;
    if (joinRunning && !g_ui.joinRunning) openWindow("connecting to a host");
    g_ui.joinRunning = joinRunning;

    testTick();
    if (!g_pending.empty()) {
        const std::string a = g_pending;
        g_pending.clear();
        runAction(a, st, act);
    }
    if (!g_ngTarget.empty()) preselectStart();

    if (!g_ui.open || !g_ui.bg) return;
    if (!g_ui.win) {
        g_ui.win = g_ui.bg->createWidgetReal<MyGUI::Widget>(SKIN_PANEL,
            MyGUI::FloatCoord(0.47f, 0.07f, 0.50f, 0.86f), MyGUI::Align::Default,
            g_ui.prefix + "KenshiCoopLobby");
        g_ui.content = 0;
        g_ui.hash = 0;
        if (!g_ui.win) return;
    }
    Model m;
    buildModel(st, act, m);
    const MyGUI::IntCoord cc = g_ui.win->getClientCoord();
    const unsigned h = modelHash(m, cc.width, cc.height);
    if (!g_ui.content || h != g_ui.hash) {
        build(m);
        g_ui.hash = h;
    }
}

bool lobbyIsOpen() { return g_ui.open; }

void lobbyTestTick() { testTick(); }

// ASCII-uppercase copy (Kenshi shows squad tab captions in capitals).
static std::string upperAscii(const std::string& s) {
    std::string u(s);
    for (size_t i = 0; i < u.size(); ++i) if (u[i] >= 'a' && u[i] <= 'z') u[i] = (char)(u[i] - 32);
    return u;
}

bool renameSquadTabCaption(const std::string& oldName, const std::string& newName) {
    MyGUI::Widget* w = findLayoutWidget("SquadPanel", "ShortcutsPanel", 0);
    if (!w) return false;
    MyGUI::TabControl* tc = w->castType<MyGUI::TabControl>(false);
    if (!tc) return false;
    const std::string o = upperAscii(oldName);
    for (size_t i = 0; i < tc->getItemCount(); ++i) {
        const std::string cur = tc->getItemNameAt(i).asUTF8();
        if (cur != oldName && upperAscii(cur) != o) continue;
        const bool caps = cur != oldName && cur == o;
        tc->setItemNameAt(i, MyGUI::UString(caps ? upperAscii(newName) : newName));
        return true;
    }
    return false;
}

} // namespace engine
} // namespace coop
