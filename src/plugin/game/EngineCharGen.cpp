// EngineCharGen.cpp - join character creation (CharCreate, protocol 57).
//
// Opens Kenshi's own character editor (CharacterEditWindow, EDIT_NEWGAME so the
// race/subrace/gender buttons are live) on a join's owned characters, exports
// the result through the game's own .bod2 path (GameData::saveToFile, as the
// editor's exportCharacter does) and re-applies such bytes on another machine
// the way loadImportedCharacter does (createAppearanceData + loadFromFile +
// updateFrom, then Character::setAppearanceData which takes ownership and runs
// setRace on a race change - in place, so the body's hand stays valid).
//
// A marker key (idata "kenshicoop chargen") in the appearance GameData records
// "already created"; AppearanceBase::serialise copies every appearance key into
// the save, so the host's baked save carries it to reconnecting joins.
//
// Owner state: none (file-local SEH shims only). C2712 split as elsewhere: the
// std::string/map work lives in plain helpers, the __try frames are POD-only.

#include "EngineInternal.h"

#include <kenshi/gui/CharacterEditWindow.h>
#include <kenshi/AppearanceManager.h>
#include <kenshi/RaceData.h>
#include <windows.h>
#include <cstdio>

namespace coop {
namespace engine {

namespace {

const char* const CHARGEN_MARKER = "kenshicoop chargen";

// ---- plain (unwindable) helpers ------------------------------------------

bool markerRead(Character* c) {
    GameDataCopyStandalone* a = c->getAppearanceData();
    if (!a) return false;
    std::string k(CHARGEN_MARKER);
    return a->idata.find(k) != a->idata.end() && a->idata[k] != 0;
}

void markerWrite(Character* c) {
    GameDataCopyStandalone* a = c->getAppearanceData();
    if (a) a->idata[std::string(CHARGEN_MARKER)] = 1;
}

void raceSidCopy(Character* c, char* out, unsigned int cap) {
    RaceData* r = c->getRace();
    const char* s = (r && r->data) ? r->data->stringID.c_str() : "";
    strncpy(out, s, cap - 1);
    out[cap - 1] = '\0';
}

void showEditor(ForgottenGUI* g, Character** chars, unsigned int n) {
    lektor<Character*> lk;
    // The callee takes the lektor by value and frees 'stuff' with MSVCR100
    // free() (crash-measured: an Ogre-allocated buffer corrupts the heap), so
    // allocate from the same CRT heap our /MD v100 DLL shares.
    lk.stuff = (Character**)malloc(sizeof(Character*) * n);
    for (unsigned int i = 0; i < n; ++i) lk.stuff[i] = chars[i];
    lk.count = n;
    lk.maxSize = n;
    g->showCharacterEditor(lk, EDIT_NEWGAME, 0);
}

void tempBod2Path(char* out, unsigned int cap) {
    static unsigned int s_n = 0;
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) { dir[0] = '.'; dir[1] = '\\'; dir[2] = '\0'; }
    _snprintf(out, cap, "%skc_chargen_%lu_%u.bod2", dir,
              (unsigned long)GetCurrentProcessId(), ++s_n);
    out[cap - 1] = '\0';
}

bool exportTo(Character* c, const char* path) {
    GameDataCopyStandalone* a = c->getAppearanceData();
    if (!a) return false;
    std::string inEd("in editor");
    a->bdata.erase(inEd);
    return a->saveToFile(std::string(path));
}

bool applyFrom(Character* c, const char* path) {
    AppearanceManager* am = AppearanceManager::getInstance();
    if (!am) return false;
    GameDataCopyStandalone* tmp = am->createAppearanceData(0);
    if (!tmp) return false;
    tmp->clearEverything();
    if (!tmp->loadFromFile(std::string(path), (itemType)66)) { delete tmp; return false; }
    GameDataCopyStandalone* fin = am->createAppearanceData(0);
    if (!fin) { delete tmp; return false; }
    fin->updateFrom(tmp, false);
    delete tmp;
    c->setAppearanceData(fin); // takes ownership
    return true;
}

void setNameStr(Character* c, const char* name) {
    c->setName(std::string(name));
}

void autoConfirmBody(CharacterEditWindow* w, const char* name) {
    w->randomiseAll(0);
    w->nextRace(0);
    unsigned int n = w->characters.size();
    for (unsigned int i = 0; i < n; ++i) {
        Character* c = w->characters[i];
        if (c) c->setName(std::string(name));
    }
    w->multipleCharactersChangedCheck = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    w->confirmButton(0);
}

// ---- SEH frames (POD only) -----------------------------------------------

bool markerReadSeh(Character* c) {
    __try { return markerRead(c); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool markerWriteSeh(Character* c) {
    __try { markerWrite(c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool raceSidSeh(Character* c, char* out, unsigned int cap) {
    __try { raceSidCopy(c, out, cap); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; return false; }
}
bool showEditorSeh(ForgottenGUI* g, Character** chars, unsigned int n) {
    __try { showEditor(g, chars, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool exportSeh(Character* c, const char* path) {
    __try { return exportTo(c, path); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool applySeh(Character* c, const char* path) {
    __try { return applyFrom(c, path); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool setNameSeh(Character* c, const char* name) {
    __try { setNameStr(c, name); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool autoConfirmSeh(CharacterEditWindow* w, const char* name) {
    __try { autoConfirmBody(w, name); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
CharacterEditWindow* editorSeh() {
    __try { return ::gui ? ::gui->characterEditor : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool readFileBytes(const char* path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    out.clear();
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool writeFileBytes(const char* path, const std::vector<unsigned char>& data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t w = data.empty() ? 0 : fwrite(&data[0], 1, data.size(), f);
    fclose(f);
    return w == data.size();
}

} // namespace

// ---- public facade (declared in Engine.h, CharCreate block) ---------------

bool chargenHasMarker(Character* c) {
    return c ? markerReadSeh(c) : false;
}

bool chargenSetMarker(Character* c) {
    return c ? markerWriteSeh(c) : false;
}

bool chargenIsCoopStartChar(Character* c) {
    if (!c) return false;
    static const char kSuffix[] = "-KenshiCoop-MultiplayerStart.mod";
    __try {
        GameData* gd = c->getGameData();
        if (!gd) return false;
        const char* sid = gd->stringID.c_str();
        size_t n = sid ? strlen(sid) : 0, m = sizeof(kSuffix) - 1;
        return n > m && strcmp(sid + n - m, kSuffix) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool chargenRaceKnown(GameWorld* gw, const char* sid) {
    if (!sid || !sid[0]) return false;
    return findItemTemplateImpl(gw, sid, (unsigned int)RACE) != 0;
}

void chargenRaceSid(Character* c, char* out, unsigned int cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (c) raceSidSeh(c, out, cap);
}

bool chargenOpenEditor(Character** chars, unsigned int n) {
    if (!::gui || !chars || n == 0 || n > 32) return false;
    if (editorSeh()) return false;
    return showEditorSeh(::gui, chars, n);
}

bool chargenEditorOpen() {
    return editorSeh() != 0;
}

bool chargenAutoConfirm(const char* name) {
    CharacterEditWindow* w = editorSeh();
    if (!w) return false;
    return autoConfirmSeh(w, name ? name : "Coop Wanderer");
}

bool chargenExport(Character* c, std::vector<unsigned char>& out) {
    out.clear();
    if (!c) return false;
    char path[MAX_PATH + 64];
    tempBod2Path(path, sizeof(path));
    bool ok = exportSeh(c, path) && readFileBytes(path, out) && !out.empty();
    DeleteFileA(path);
    return ok;
}

bool chargenApply(Character* c, const std::vector<unsigned char>& bod2, const char* name) {
    if (!c || bod2.empty()) return false;
    char path[MAX_PATH + 64];
    tempBod2Path(path, sizeof(path));
    bool ok = writeFileBytes(path, bod2) && applySeh(c, path);
    DeleteFileA(path);
    if (ok && name && name[0]) setNameSeh(c, name);
    return ok;
}

} // namespace engine
} // namespace coop
