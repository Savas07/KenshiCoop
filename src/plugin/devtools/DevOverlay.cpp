// DevOverlay.cpp - in-world debug labels for co-op sync debugging (DevTools.h).
//
// Per player-squad body (DevBody, max kMaxBodies):
//   caption  tracks the character (engine projection, like markerCreate):
//            "<name> <regime> hk=<0|1> t=<task> gap=<gap>[ DETACHED <d>]"
//            green normal / yellow glide, snap, walk with gap>30 /
//            red detached or "upright with no Havok body" (frozen model).
//   [M]      fixed label at the rendered pelvis bone (green)
//   [H]      fixed label at the Havok character position (red)
//            [M]/[H] only show while that point is off the nameplate position
//            (getPosition) - otherwise they would just stack on every body.
//
// Lifetime: ForgottenGUI owns every ScreenLabel and destroys them on its own
// schedule (tracked labels die the frame their character stops resolving; a
// world teardown drops them all), so a held handle can dangle or even be
// recycled for someone else's label. Every handle is therefore re-validated
// each tick with markerCaptionIs (registry membership AND the exact caption we
// last set) before it is touched, and a handle that fails is forgotten - never
// destroyed, since it may now belong to a stranger. Slots are keyed by the
// body's hand (index,serial); no Character* is ever held across ticks.
//
// Main thread only. All engine access goes through coop::engine (SEH-guarded).

#include "DevTools.h"
#include "../game/Engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace coop {
namespace overlay {

namespace {

// ---- Tuning ------------------------------------------------------------------

const unsigned int kMaxBodies = 24;
// DETACHED: model (pelvis) this far from the nameplate horizontally.
const float kDetachDist = 15.0f;
// A walk drive this far behind the stream is worth a yellow caption.
const float kWalkGapWarn = 30.0f;
// [M]/[H] show past kShowDist and hide again under kHideDist. The gap keeps a
// point hovering at the threshold from blinking. Horizontal distance only: the
// pelvis bone and the Havok capsule sit at a pose-dependent HEIGHT above the
// ground position, which is not a desync.
const float kShowDist = 2.0f;
const float kHideDist = 1.5f;
// Caption height above the tracked position, in multiples of the measured
// upright pelvis height. At the FEET, not the head: Kenshi's own nameplate sits
// at head height and a head-high caption printed on top of it (seen in-game).
const float kCaptionPelvisMul = 0.25f;
// Before any pelvis has been measured.
const float kCaptionFallbackOff = 0.3f;

const int COL_GREEN = 0;   // markerColour ids (EngineUi.cpp)
const int COL_RED = 1;
const int COL_YELLOW = 2;

const char* const kMeshCaption = "[M]";
const char* const kHkCaption = "[H]";

// ---- Per-body label slot -------------------------------------------------------

// One fixed-position label ([M] or [H]).
struct FixedLabel {
    void* label;
    bool  shown;
    float last[3];          // where it was last placed (to hide it in place)
};

struct Slot {
    bool used;
    bool seen;              // present in this tick's bodies
    unsigned int hand[5];   // full hand of the body (key = hand[3], hand[4])
    void* caption;
    char  capText[160];     // exact caption last set (identity check)
    int   capColor;
    FixedLabel mesh;
    FixedLabel hk;
};

Slot g_slots[kMaxBodies];
// Running estimate of an upright pelvis height above the ground position
// (world units); 0 until a body with a readable skeleton has been seen.
float g_pelvisH = 0.0f;

float hdist(const float* a, const float* b) {
    float dx = a[0] - b[0];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

// Forget a handle that no longer validates. Never destroy it: a failed check
// means it is either already gone or now a label somebody else owns.
void validate(void** label, const char* caption) {
    if (*label && !engine::markerCaptionIs(*label, caption)) *label = 0;
}

// Destroy a label only if it is still provably ours.
void destroyOwned(void** label, const char* caption) {
    if (*label && engine::markerCaptionIs(*label, caption))
        engine::markerDestroy(*label);
    *label = 0;
}

void dropSlot(Slot* s) {
    destroyOwned(&s->caption, s->capText);
    destroyOwned(&s->mesh.label, kMeshCaption);
    destroyOwned(&s->hk.label, kHkCaption);
    memset(s, 0, sizeof(*s));
}

Slot* findSlot(const unsigned int* hand) {
    for (unsigned int i = 0; i < kMaxBodies; ++i)
        if (g_slots[i].used && g_slots[i].hand[3] == hand[3] &&
            g_slots[i].hand[4] == hand[4])
            return &g_slots[i];
    return 0;
}

Slot* allocSlot(const unsigned int* hand) {
    for (unsigned int i = 0; i < kMaxBodies; ++i) {
        if (g_slots[i].used) continue;
        Slot* s = &g_slots[i];
        memset(s, 0, sizeof(*s));
        s->used = true;
        memcpy(s->hand, hand, sizeof(s->hand));
        s->capColor = -1;
        return s;
    }
    return 0;
}

bool upright(const DevBody& b) {
    return !coop::bodyIsDown(b.bs);
}

// ---- Caption -------------------------------------------------------------------

void buildCaption(const DevBody& b, char* out, unsigned int cap, int* color) {
    bool detached = false;
    float d = 0.0f;
    if (b.haveMesh && b.havePos) {
        d = hdist(b.mesh, b.np);
        detached = d > kDetachDist;
    }
    char name[sizeof(b.name) + 1];
    memcpy(name, b.name, sizeof(b.name));
    name[sizeof(b.name)] = 0;

    int n = _snprintf(out, cap - 1, "%s %s hk=%d t=%d gap=%.1f",
                      name[0] ? name : "?", devRegimeName(b.regime),
                      b.haveHk ? 1 : 0, b.task, b.gap);
    if (n < 0) n = (int)cap - 1;
    if (detached && (unsigned int)n < cap - 1)
        _snprintf(out + n, cap - 1 - n, " DETACHED %.1f", d);
    out[cap - 1] = 0;

    // Red: the model cannot follow placement - left behind (detached), or an
    // upright, uncarried body with no Havok character (the frozen-model case).
    if (detached || (!b.haveHk && upright(b) && !b.carried))
        *color = COL_RED;
    else if (b.regime == DR_GLIDE || b.regime == DR_SNAP ||
             (b.regime == DR_WALK && b.gap > kWalkGapWarn))
        *color = COL_YELLOW;
    else
        *color = COL_GREEN;
}

void updateCaption(Slot* s, const DevBody& b) {
    char text[sizeof(s->capText)];
    int color = COL_GREEN;
    buildCaption(b, text, sizeof(text), &color);

    validate(&s->caption, s->capText);
    if (!s->caption) {
        // Resolve fresh every time we mint: the label tracks by HAND, so the
        // Character* is only needed for this one call and is not kept.
        Character* c = engine::resolveCharByHand(b.hand[3], b.hand[4], b.hand[0],
                                                 b.hand[1], b.hand[2]);
        if (!c) return;   // not loaded here: no caption until it resolves
        float off = g_pelvisH > 0.0f ? g_pelvisH * kCaptionPelvisMul
                                     : kCaptionFallbackOff;
        s->caption = engine::markerCreateTracked(c, text, color, off);
        if (s->caption) {
            memcpy(s->capText, text, sizeof(s->capText));
            s->capColor = color;
        }
        return;
    }
    if (color == s->capColor && strcmp(text, s->capText) == 0) return;
    if (engine::markerUpdate(s->caption, text, color)) {
        memcpy(s->capText, text, sizeof(s->capText));
        s->capColor = color;
    } else {
        s->caption = 0;   // died between the check and the update; re-mint next tick
    }
}

// ---- Fixed [M]/[H] labels ------------------------------------------------------

void updateFixed(FixedLabel* f, const char* caption, int color,
                 bool have, const float* p, const float* np) {
    validate(&f->label, caption);
    if (!f->label) f->shown = false;

    bool want = false;
    if (have) {
        float d = hdist(p, np);
        want = d > (f->shown ? kHideDist : kShowDist);
    }

    if (want) {
        if (!f->label) {
            f->label = engine::markerCreateFixed(caption, color, p[0], p[1], p[2]);
            f->shown = f->label != 0;
        } else if (engine::markerPlace(f->label, p[0], p[1], p[2], true)) {
            f->shown = true;
        } else {
            f->label = 0; f->shown = false;
        }
        if (f->label) memcpy(f->last, p, sizeof(f->last));
    } else if (f->label && f->shown) {
        // Hide in place rather than destroy: a point that keeps crossing the
        // threshold would otherwise churn the GUI's add/remove queues.
        if (engine::markerPlace(f->label, f->last[0], f->last[1], f->last[2], false))
            f->shown = false;
        else
            f->label = 0;
    }
}

} // namespace

// ---- Public --------------------------------------------------------------------

void tick(GameWorld* gw, const std::vector<DevBody>& bodies, bool on) {
    (void)gw;
    if (!on) { clear(); return; }

    unsigned int n = (unsigned int)bodies.size();
    if (n > kMaxBodies) n = kMaxBodies;

    // Calibrate the caption height from upright skeletons (see kCaptionPelvisMul).
    for (unsigned int i = 0; i < n; ++i) {
        const DevBody& b = bodies[i];
        if (!b.haveMesh || !b.havePos || !upright(b) || b.carried) continue;
        float h = b.mesh[1] - b.np[1];
        if (h < 0.2f || h > 40.0f) continue;   // implausible: crouch/seat/garbage
        g_pelvisH = g_pelvisH > 0.0f ? g_pelvisH * 0.95f + h * 0.05f : h;
    }

    for (unsigned int i = 0; i < kMaxBodies; ++i) g_slots[i].seen = false;

    for (unsigned int i = 0; i < n; ++i) {
        const DevBody& b = bodies[i];
        Slot* s = findSlot(b.hand);
        // Same (index,serial) but a different container/type: another object.
        if (s && memcmp(s->hand, b.hand, sizeof(s->hand)) != 0) {
            dropSlot(s);
            s = 0;
        }
        if (!s) s = allocSlot(b.hand);
        if (!s) continue;
        s->seen = true;

        updateCaption(s, b);
        updateFixed(&s->mesh, kMeshCaption, COL_GREEN, b.haveMesh && b.havePos,
                    b.mesh, b.np);
        updateFixed(&s->hk, kHkCaption, COL_RED, b.haveHk && b.havePos,
                    b.hk, b.np);
    }

    // Bodies gone from the snapshot lose their labels this tick.
    for (unsigned int i = 0; i < kMaxBodies; ++i)
        if (g_slots[i].used && !g_slots[i].seen) dropSlot(&g_slots[i]);
}

void clear() {
    for (unsigned int i = 0; i < kMaxBodies; ++i)
        if (g_slots[i].used) dropSlot(&g_slots[i]);
}

} // namespace overlay
} // namespace coop
