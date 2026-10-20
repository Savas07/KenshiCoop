// ReplicatorDev.cpp - Replicator::devSnapshot for the developer tools
// (devtools/DevTools.h, docs/DEVTOOLS.md). Read-only: probes the local engine
// truth of every player-squad character and pairs it with the drive record
// applyTargets left on its Driven entry this tick. Only called while the
// devtools are on, so its per-tick bone/physics reads cost nothing otherwise.

#include "ReplicatorUtil.h"
#include "../devtools/DevTools.h"

#include <cstring>

namespace coop {

namespace {

void fillLocal(Character* c, DevBody& b) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    b.havePos = engine::readPos(c, &x, &y, &z);
    b.np[0] = x; b.np[1] = y; b.np[2] = z;
    engine::MeshProbe mp;
    b.haveMesh = engine::readMeshProbe(c, &mp) && mp.valid;
    if (b.haveMesh) { b.mesh[0] = mp.px; b.mesh[1] = mp.py; b.mesh[2] = mp.pz; b.foot = mp.footSep; }
    else            { b.foot = -1.0f; }
    engine::DriveProbe dp;
    b.haveHk = engine::readDriveProbe(c, &dp) && dp.haveHk;
    if (b.haveHk) {
        // Havok space is world / 10 (measured on the crawl probe).
        b.hk[0] = dp.hkX * 10.0f; b.hk[1] = dp.hkY * 10.0f; b.hk[2] = dp.hkZ * 10.0f;
    }
    b.bs   = engine::readBodyState(c);
    b.task = engine::readTaskKey(c);
    engine::CarryRead cr;
    if (engine::readCarry(c, &cr) && cr.valid) { b.carrying = cr.carrying; b.carried = cr.beingCarried; }
    bool mv = false; float sp = 0.0f;
    if (engine::readMotion(c, &mv, &sp)) { b.localMoving = mv; b.localSpeed = sp; }
    engine::charName(c, b.name, sizeof(b.name));
}

} // namespace

void Replicator::devSnapshot(GameWorld* gw, std::vector<DevBody>& out) {
    out.clear();
    if (!gw) return;
    // Every player-squad character in the local world: our own tab(s) and the
    // copies of everyone else's (they share the player faction).
    static EntityState sq[64]; // main thread only
    unsigned int n = engine::captureSquad(gw, false, sq, 64);
    const unsigned long now = nowMs();
    for (unsigned int i = 0; i < n && out.size() < 32; ++i) {
        const Key k = keyOf(sq[i]);
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        DevBody b;
        memset(&b, 0, sizeof(b));
        b.hand[0] = k.t; b.hand[1] = k.c; b.hand[2] = k.cs; b.hand[3] = k.i; b.hand[4] = k.s;
        fillLocal(c, b);
        std::map<Key, Driven>::iterator dit = targets_.find(k);
        b.own = ownHands_.find(k) != ownHands_.end() || dit == targets_.end();
        if (b.own) {
            b.author = localId_;
            b.regime = DR_OWN;
        } else {
            const Driven& d = dit->second;
            b.author = d.author;
            // A record applyTargets did not touch this tick is stale.
            b.regime = (d.dbgMs != 0 && now - d.dbgMs <= 250) ? d.dbgRegime : DR_NONE;
            b.fresh  = d.fresh;
            b.gap    = d.dbgGap;
            b.vlen   = d.dbgVlen;
            b.glide  = d.glideActive;
            b.haveDest = d.haveDest;
            if (d.dbgHaveNewest) { b.newest[0] = d.dbgNx; b.newest[1] = d.dbgNy; b.newest[2] = d.dbgNz; }
            EntityState e; float vx, vy, vz;
            if (d.interp.latest(&e, &vx, &vy, &vz)) {
                b.cSpeed = e.cSpeed;
                b.streamMoving = e.cMoving != 0;
                b.streamBs = e.bodyState;
                b.streamTask = e.task;
            }
        }
        out.push_back(b);
    }
}

bool Replicator::scenarioReplicaInfo(const unsigned int localHand[5],
                                     unsigned int outCanon[5], int* outRegime,
                                     unsigned int* outTask, unsigned int outSubj[5],
                                     bool* outDest) const {
    if (outRegime) *outRegime = -1;
    if (outTask) *outTask = 0;
    if (outDest) *outDest = false;
    for (int j = 0; j < 5; ++j) {
        if (outCanon) outCanon[j] = localHand ? localHand[j] : 0;
        if (outSubj) outSubj[j] = 0;
    }
    if (!localHand) return false;
    Character* c = engine::resolveCharByHand(localHand[3], localHand[4], localHand[0],
                                             localHand[1], localHand[2]);
    if (!c) return false;
    Key k; k.t = localHand[0]; k.c = localHand[1]; k.cs = localHand[2];
    k.i = localHand[3]; k.s = localHand[4];
    std::map<Character*, Key>::const_iterator ci = canonicalOf_.find(c);
    if (ci != canonicalOf_.end()) k = ci->second;
    if (outCanon) {
        outCanon[0] = k.t; outCanon[1] = k.c; outCanon[2] = k.cs;
        outCanon[3] = k.i; outCanon[4] = k.s;
    }
    if (ownHands_.find(k) != ownHands_.end()) return true;
    std::map<Key, Driven>::const_iterator dit = targets_.find(k);
    if (dit == targets_.end()) return true;
    const Driven& d = dit->second;
    const unsigned long now = nowMs();
    if (outRegime)
        *outRegime = (d.dbgMs != 0 && now - d.dbgMs <= 250) ? d.dbgRegime : DR_NONE;
    if (outDest) *outDest = d.haveDest;
    EntityState e;
    if (d.interp.latest(&e, 0, 0, 0)) {
        if (outTask) *outTask = e.task;
        if (outSubj) {
            outSubj[0] = e.sType; outSubj[1] = e.sContainer; outSubj[2] = e.sContainerSerial;
            outSubj[3] = e.sIndex; outSubj[4] = e.sSerial;
        }
    }
    return true;
}

} // namespace coop
