// ReplicatorCore.cpp - Replicator construction + session lifecycle (monolith
// split from Replicator.cpp, 2026-07-12): the ctor defaults, the Phase 3
// unified entity-lifecycle audit (lifeName/lifeSet/lifeSweep + life_),
// resetSession (the ONE place every cross-tick hub resets on session swap),
// inbound ingest (ingest/ingestInv), tab latching/ranking, and the
// end-of-session smoothness summary.
//
// Shared hubs written here: ALL of them (resetSession clears every map);
// life_ is owned here (lifeSet is the only writer, called from every TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

Replicator::Replicator()
    : catchupK_(CATCHUP_K), snapDist_(SNAP_DIST), snapSeconds_(SNAP_SECONDS),
      combatSoftDist_(COMBAT_SOFT_DIST), combatSnapDist_(COMBAT_SNAP_DIST),
      combatBigSnapDist_(COMBAT_BIG_SNAP_DIST), combatSlideMax_(COMBAT_SLIDE_MAX),
      combatConvergeMs_(COMBAT_CONVERGE_MS),
      sendStamp_(true),
      starveHoldMs_(10000), starveHeldNow_(0),
      leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f), slewSkipFrames_(0),
      zpDown_(0), zpCarried_(0), zpFurn_(0), zpChain_(0), zpCrawl_(0),
      zpSneak_(0), zpSquadIdle_(0), zpAlias_(0), zpStall_(0),
      onsetReentries_(0),
      onActiveEarly_(0), onActiveMid_(0), onActiveSteady_(0),
      onZeroEarly_(0), onZeroMid_(0), onZeroSteady_(0),
      interpLerp_(0), interpSingle_(0), interpClampOld_(0),
      interpExtrap_(0), interpSegSnap_(0),
      hardSnapSquad_(0), hardSnapNpc_(0), hardSnapMid_(0),
      walkReissueSquad_(0), physHeal_(0), getUpHeal_(0), glideFrames_(0), glideStallEnters_(0), walkReissueNpc_(0), restFlipNpc_(0), restFlipMid_(0),
      combatSnapTotal_(0), combatSoftWalk_(0), combatSlide_(0), combatOrder_(0),
      combatWrongTgt_(0), combatLogTick_(0),
      interpLogTick_(0),
      trailN_(0), trailGapNewSum_(0.0f), trailGapNewMax_(0.0f), trailGapOutSum_(0.0f),
      trailBodySpdSum_(0.0f), trailSrcSpdSum_(0.0f), trailSpdN_(0),
      pauseHold_(0), sqSnapCool_(0), sqSettle_(0),
      trailBleedSum_(0.0f), trailBleedN_(0),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      marchHold_(0), marchSettle_(0), marchRelapse_(0),
      marchHoldDip_(0), marchHoldStop_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      aiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), worldSeeded_(false), nextNativeTakeId_(1),
      nextDropId_(1), nextPickupId_(1), nextXferId_(1),
      xferScanMs_(0), nextTreatId_(1),
      quietRelapse_(0), crawlPhysRestore_(0),
      sitOrders_(0), detachUses_(0), noDetach_(false),
      dmgGuard_(false), reportCombat_(false), nextHitId_(1),
      carrySync_(true), glide_(true), furnSync_(true), chainSync_(true),
      stealthSync_(true), proneSync_(true),
      gateAuthority_(false), trustLogTick_(0),
      trustGrants_(0), trustRevokes_(0),
      authSuppresses_(0), authRestores_(0), authReassertMs_(0), authPruned_(0),
      censusRadius_(0.0f), censusSendMs_(0), censusRecvMs_(0), censusCulls_(0),
      censusOffCell_(0), cellYields_(0), localId_(0xFFFFFFFFu), hostDriveRefusals_(0),
      censusPubTrunc_(false), censusFreshPrev_(false), censusFreshChkMs_(0),
      censusStaleMs_(0), censusStaleEdges_(0), proxyDriftLogMs_(0),
      camHintSendMs_(0),
      midCursor_(0), midSliceMs_(0), midFastPromoted_(0),
      censusParkDist_(0.0f), censusParks_(0),
      censusWalkDist_(0.0f), censusWalks_(0),
      censusFreezeAi_(true),
      attentionRadius_(0.0f),
      attnFlips_(0), attnWinMs_(0), attnBaseSupp_(0), attnBaseCull_(0),
      attnBaseProxy_(0), attnVetoMs_(0), attnVetoRawN_(0), attnVetoMask_(0),
      auditRows_(false), jailProbe_(false), jailObserve_(false),
      speedLastApplied_(-1.0f), speedMyReq_(-1.0f), speedPeerReq_(-1.0f),
      speedCombatCap_(true),
      speedMyCombat_(false), speedPeerCombat_(false), speedLastSet_(-1.0f),
      speedSeqOut_(1), speedSeqSeen_(0),
      speedLastSendMs_(0), speedCombatSampleMs_(0), speedCombatHoldMs_(0),
      spawnSync_(false), spawnPosLogMs_(0),
      spawnMintRadius_(0.0f), adoptRadius_(0.0f), censusAdopts_(0),
      censusScanMs_(0),
      poolSeen_(-1), poolSent_(-1), poolSentMs_(0), poolTotal_(-1),
      poolSeq_(0), poolAcked_(0),
      moneySync_(true), recruitSync_(true),
      squadSync_(true), tabsSeeded_(0), hostRole_(true),
      cellAuth_(false), cellCollapse_(false), collapsed_(false),
      claimSendMs_(0), claimAssertMs_(0), claimMapMs_(0),
      facSeqOut_(1), facSampleMs_(0), factionSync_(true),
      doorSeqOut_(1), doorSampleMs_(0), doorSync_(true),
      buildSeqOut_(1), buildSampleMs_(0), buildSync_(true),
      bdoorSeqOut_(1), bdoorSampleMs_(0), bdoorSync_(true),
      hungerSync_(true),
      prodSeqOut_(1), prodSampleMs_(0), prodSync_(true),
      researchSeqOut_(1), researchSampleMs_(0), researchSync_(true),
      jsSeqOut_(1), jsSampleMs_(0), joinStateSync_(true),
      deedSeqOut_(1), deedSampleMs_(0), deedAuditMs_(0), deedSync_(true),
      fixtureSeqOut_(1), fixtureSampleMs_(0), fixtureSync_(true),
      storeSync_(false), contCensusMs_(0), corpseSync_(false),
      vendorSync_(false), vendorScanMs_(0), nextVendorDeltaId_(1), vendorCashMs_(0),
      timeSync_(true), timeBrake_(true),
      timeSlew_(1.0f), timeSeqOut_(1), timeSeqSeen_(0),
      timeLastSendMs_(0), timeLastLogMs_(0), timeSlewApplied_(-1.0f),
      platoonT0_(0),
      lifeSweepMs_(0) {
}

// ---- Phase 3: unified entity lifecycle ---------------------------------------
// The AUDIT layer over the authority/mint/drive machinery: every decision
// point reports the state it just put a hand into, lifeSet logs the edge, and
// the lifecycle oracle judges the journeys. Mechanics live where they were
// validated; this is the one place their OUTCOMES meet.

const char* Replicator::lifeName(int s) {
    switch (s) {
    case LIFE_DISCOVERED: return "DISCOVERED";
    case LIFE_RESOLVED:   return "RESOLVED";
    case LIFE_HI:         return "HI";
    case LIFE_MID:        return "MID";
    case LIFE_PARKED:     return "PARKED";
    case LIFE_CULLED:     return "CULLED";
    default:              return "UNKNOWN";
    }
}

int Replicator::lifeSet(const Key& k, int to, const char* reason) {
    unsigned long now = nowMs();
    Lifecycle& lc = life_[k];
    int from = lc.state;
    lc.touchMs = now;
    if (from == to) return from;
    lc.state = (u8)to;
    lc.sinceMs = now;
    lc.stuckLogMs = 0;
    // Silent seed: every census-band wilderness NPC is born PARKED, and
    // logging hundreds of those at session start buries the real journeys.
    if (from == LIFE_UNKNOWN && to == LIFE_PARKED) return from;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[life] hand=%u,%u,%u,%u,%u from=%s to=%s reason=%s",
              k.t, k.c, k.cs, k.i, k.s,
              lifeName(from), lifeName(to), reason ? reason : "-");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return from;
}

void Replicator::lifeSweep(GameWorld* gw, unsigned long now) {
    if ((now - lifeSweepMs_) < 5000) return;
    lifeSweepMs_ = now;
    // A hand not confirmed for a while left interest (or its body despawned):
    // drop the record so the map tracks the live world, and a re-appearance
    // starts a fresh journey from UNKNOWN.
    const unsigned long PRUNE_MS = 30000;
    // Self-audit: DISCOVERED means "the host swears this exists and we have
    // no body" - the mint pipeline should resolve that within its dwell +
    // round-trip budget WHEN the hand's census position sits in a locally
    // loaded zone (an unloaded far zone defers legitimately, forever if need
    // be). Older than STUCK_MS is the invisible-raid failure (the join
    // fights nothing); one line per hand per STUCK_LOG_MS so the lifecycle
    // oracle sees it without the log flooding.
    const unsigned long STUCK_MS     = 30000;
    const unsigned long STUCK_LOG_MS = 15000;
    for (std::map<Key, Lifecycle>::iterator it = life_.begin();
         it != life_.end(); ) {
        Lifecycle& lc = it->second;
        if ((now - lc.touchMs) > PRUNE_MS) { life_.erase(it++); continue; }
        if (lc.state == LIFE_DISCOVERED && (now - lc.sinceMs) > STUCK_MS &&
            (lc.stuckLogMs == 0 || (now - lc.stuckLogMs) > STUCK_LOG_MS)) {
            std::map<Key, CensusPos>::iterator cp = censusPos_.find(it->first);
            bool mintable = cp != censusPos_.end() &&
                            engine::isZoneLoadedAt(gw, cp->second.x,
                                                   cp->second.y, cp->second.z);
            if (mintable) {
                lc.stuckLogMs = now;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "[life] STUCK hand=%u,%u,%u,%u,%u state=DISCOVERED age=%lus (mintable)",
                          it->first.t, it->first.c, it->first.cs, it->first.i,
                          it->first.s, (now - lc.sinceMs) / 1000);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                lc.stuckLogMs = now; // re-check the zone on the next cadence
            }
        }
        ++it;
    }
}

void Replicator::resetSession() {
    // Pointer caches (dangling after the swap) + everything keyed off them.
    targets_.clear();          // interp buffers + per-body drive state
    drivenChars_.clear();
    drivenSeen_.clear();       // recently-driven grace (pointers dangle after swap)
    canonicalOf_.clear();      // capture-translation reverse map (same pointers)
    rekeyLogged_.clear();      // and the change-detector for its [rekey] line
    ownSkipLogMs_.clear();     // [drive] SKIP-OWNED throttle
    pinNpcSkipLogged_.clear(); // [recruit] NPC-SKIP once-per-hand log
    jailObs_.clear();          // jail-observe spike per-captive last sample
    proxyByKey_.clear();
    suppressed_.clear();
    midBand_.clear();          // host mid-band round-robin (rebuilt by next census)
    midCursor_ = 0; midSliceMs_ = 0;
    life_.clear();             // Phase 3 lifecycle: the OLD world's journeys
    lifeSweepMs_ = 0;
    // Debug markers hold raw Character* from the OLD world plus GUI label
    // objects we own - destroy the labels and drop the map before either
    // dangles into the new session.
    for (std::map<Character*, DebugMarker>::iterator mi = debugMarkers_.begin();
         mi != debugMarkers_.end(); ++mi)
        engine::markerDestroy(mi->second.label);
    debugMarkers_.clear();
    hostBody_.clear();
    attackerOf_.clear();
    combatCapMs_.clear();
    authCount_.clear();
    ownHands_.clear();
    // Protocol 36: the existence censuses describe the OLD world's hands; their
    // authors re-publish within a second of the new world going live.
    census_.clear();
    censusHands_.clear();
    censusPos_.clear();
    censusOwnerOf_.clear();
    censusFix_.clear();
    parkMs_.clear();
    censusRecvMs_ = 0;
    censusSendMs_ = 0;
    // Protocol 43: the camera hints describe the OLD world's coordinates.
    camHintSendMs_ = 0;
    peerCams_.clear();
    // Attention latches are per-hand state about the OLD world's geometry.
    attnObs_.clear();
    attnObsPeer_.clear();
    // Claims describe where bodies stand in the OLD world.
    claimSlots_.clear();
    claimedCells_.clear();
    cellLastOwner_.clear();
    collapseTo_.clear();
    claimDwell_.clear();
    cellYield_.clear();
    claimSendMs_   = 0;
    claimAssertMs_ = 0;
    claimMapMs_    = 0;
    attnFlips_ = 0;
    attnWinMs_ = 0;
    // The zone verdict describes the OLD world's streaming state.
    attnVetoMs_ = 0;
    attnVetoRawN_ = 0;
    attnVetoMask_ = 0;
    furnPeerPend_.clear();
    ownFurnExit_.clear();
    // Session maps + change-gate baselines (they describe the OLD world; the
    // reloaded save re-seeds them on first sample).
    ownBuilds_.clear();
    peerBuilds_.clear();
    mintByLocal_.clear();
    bdoorRows_.clear();
    doorRows_.clear();
    prodRows_.clear();
    researchRows_.clear(); // protocol 38: re-baselined from the new world's store
    jsSquads_.clear();     // protocol 64: re-baselined from the new world's names/jobs
    jsFaction_ = JsRow();
    jsJobs_.clear();
    deedRows_.clear();     // protocol 54: re-baselined from the new world's owned set
    facRows_.clear();
    invPub_.clear();
    invRecv_.clear();
    ownedContainers_.clear();
    censusContainers_.clear(); // protocol 34: re-censused in the new world
    censusCorpses_.clear();    // protocol 62: likewise
    // Protocol 60: every counter hand + baseline belongs to the old world.
    censusShops_.clear();
    vendorBase_.clear();
    vendorPend_.clear();
    vendorOwners_.clear();
    appliedVendorDeltas_.clear();
    vendorCashSent_.clear();
    vendorCashBase_.clear();
    vendorCashPend_.clear();
    vendorCashIn_.clear();
    worldTrack_.clear();
    worldProxies_.clear();
    worldSeeded_ = false; // re-log the reloaded world's save-native census
    pendingDrops_.clear(); // after a reload every ground item is a save-native
    dormantDrops_.clear();
    weaponCensus_.clear();
    appliedDrops_.clear();
    appliedPickups_.clear();
    groundedWeapons_.clear();
    pendingPickups_.clear(); // the objects those intents named belong to the old world
    // Protocol 37: every container hand and Item* baseline is stale in the new world.
    xferBase_.clear();
    xferSeeded_.clear();
    xferPend_.clear();
    xferLatch_.clear();
    xferDefer_.clear();
    xferOut_.clear();
    appliedXfers_.clear();
    wdSuppress_.clear();
    xferScanMs_ = 0;
    medPub_.clear();
    medRecv_.clear();
    medNpc_.clear();
    statsPub_.clear();
    // Protocol 52: the pool baseline describes the OLD world's wallet, so a
    // reload must re-seed it - otherwise the newly loaded save's cats read as
    // one giant local purchase. Pending deltas die with the session they were
    // measured in.
    poolSeen_ = -1; poolSent_ = -1; poolSentMs_ = 0; poolTotal_ = -1;
    poolSeq_ = 0; poolAcked_ = 0;
    poolPending_.clear();
    stealthPub_.clear();
    pinOwned_.clear();
    pinPeer_.clear();
    moveEcho_.clear();
    exitedOwn_.clear();
    // Protocol 35: the rank latch + the engine's pointer->hand baseline both
    // describe the OLD world (containers and Character* dangle after a swap);
    // the reloaded save re-seeds them at first census/poll.
    tabRank_.clear();
    lastCtnrs_.clear();
    tabOwned_.clear();
    tabsSeeded_ = 0;
    rekeyedOld_.clear();
    engine::clearSquadRoster();
    probed_.clear();
    spawnReq_.clear();
    unresolvedHands_.clear();
    forceReqHands_.clear();
    spawnLogged_.clear();
    spawnReplyMs_.clear();
    censusScanMs_ = 0;
    // Speed/time consensus: re-seed from the fresh world's live state (the
    // save's speed becomes the new baseline; the join's slew re-measures).
    speedLastApplied_ = -1.0f;
    speedMyReq_       = -1.0f;
    speedPeerReq_     = -1.0f;
    speedMyCombat_    = false;
    speedPeerCombat_  = false;
    speedLastSet_     = -1.0f;
    speedSeqSeen_     = 0;
    speedLastSendMs_  = 0;
    speedCombatSampleMs_ = 0;
    speedCombatHoldMs_ = 0;
    timeSlew_         = 1.0f;
    timeSeqSeen_      = 0;
    timeLastSendMs_   = 0;
    timeSlewApplied_  = -1.0f;
    // Sample-cadence clocks restart.
    facSampleMs_ = doorSampleMs_ = buildSampleMs_ = bdoorSampleMs_ = 0;
    prodSampleMs_ = 0;
    researchSampleMs_ = 0;
    jsSampleMs_ = 0;
    deedSampleMs_ = 0;
    deedAuditMs_ = 0;
    // A coordinated load re-instantiates every runtime fixture, so both halves
    // of the protocol-55 pairing are stale: re-announce and re-match from
    // scratch rather than translate a hand into a destroyed object.
    fixtureSampleMs_ = 0;
    fixtureOut_.clear();
    fixtureMap_.clear();
    contCensusMs_ = 0;
    authReassertMs_ = 0;
    // Every remote player's channel state (speed votes, time seqs, money acks,
    // per-sender row seqs) describes the old session too.
    forgetPeerChannels(OWNER_ID_ALL);
    eventSeen_.clear();
    // Config gates, ownRanks_ and every OUTBOUND seq counter are deliberately
    // preserved (see the header comment).
    coop::logLine("[load] session reset: pointer caches, session maps, change gates cleared");
}

void Replicator::clearPeerReplicationState(GameWorld* gw, u32 leaverId) {
    // Destroy MINTED proxy bodies before the maps that own the pointers drop
    // them. The engine owns the bodies; a proxy left standing after the player
    // that authored it is gone is a permanent ghost, and any map still pointing
    // at it (targets_, drivenChars_) would drive a body with no fresh authority -
    // or a freed pointer once the engine reaps it. SEH-guarded via
    // despawnProxyNpc so a single bad pointer can't take down the leave path.
    //
    // ADOPTED entries are released, not destroyed (destroyIfMinted): a body we
    // adopted is one this client generated itself, so its author leaving makes
    // it ours again rather than making it garbage. Destroying them here would
    // empty a whole town on disconnect, and bake that emptiness into the next
    // save.
    const bool all = (leaverId == OWNER_ID_ALL);
    // Who a proxy key belongs to: the author of its stream if one is on record,
    // else the player its spawn REQ was about, else the census that listed it.
    // A key nobody can attribute is kept - only the leaver's bodies go.
    std::set<Key> gone;   // stream keys the leaver authored (targets_ + proxies)
    if (!all) {
        for (std::map<Key, Driven>::const_iterator ti = targets_.begin();
             ti != targets_.end(); ++ti)
            if (ti->second.author == leaverId) gone.insert(ti->first);
        for (std::map<Key, Character*>::const_iterator pi = proxyByKey_.begin();
             pi != proxyByKey_.end(); ++pi) {
            u32 who = AUTHOR_NONE;
            std::map<Key, Driven>::const_iterator ti = targets_.find(pi->first);
            if (ti != targets_.end()) who = ti->second.author;
            if (who == AUTHOR_NONE) {
                std::map<Key, SpawnReqState>::const_iterator sq = spawnReq_.find(pi->first);
                if (sq != spawnReq_.end()) who = sq->second.target;
            }
            if (who == AUTHOR_NONE) {
                std::map<Key, u32>::const_iterator co = censusOwnerOf_.find(pi->first);
                if (co != censusOwnerOf_.end()) who = co->second;
            }
            if (who == leaverId) gone.insert(pi->first);
        }
    }
    unsigned int cleared = 0, released = 0;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ) {
        if (!all && gone.find(it->first) == gone.end()) { ++it; continue; }
        if (gw && it->second) {
            if (destroyIfMinted(gw, it->second)) ++cleared; else ++released;
        }
        if (all) { ++it; continue; }   // resetSession drops the whole map below
        mintedBodies_.erase(it->second);
        spawnReq_.erase(it->first);
        life_.erase(it->first);
        proxyByKey_.erase(it++);
    }
    if (all) mintedBodies_.clear();
    char b[128];
    _snprintf(b, sizeof(b) - 1, "[leave] cleared proxies=%u released=%u",
              cleared, released);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // World-item proxies (Phase 3): the world stays LIVE across a peer leave /
    // reconnect (no engine world swap), so the proxy RootObjects we minted for the
    // departed player's ground items must be destroyed here - otherwise they
    // linger as duplicate items and, worse, get baked into the save on the next
    // write (becoming natives that re-stream on reload). The maps alone do not
    // own the bodies, so despawn first. The world surviving does NOT mean the
    // proxies did: the player may have left after a long trek, and anything whose
    // block unloaded on the way is already destroyed, so each one is re-resolved.
    // Keyed (author, netId), so a single leaver's are exactly first == leaverId.
    unsigned int wcleared = 0, wstale = 0;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator wi = worldProxies_.begin();
         wi != worldProxies_.end(); ) {
        if (!all && wi->first.first != leaverId) { ++wi; continue; }
        RootObject* live = gw ? liveWorldProxy(wi->second) : 0;
        if (!live) ++wstale;
        else if (engine::removeWorldItemProxy(gw, live)) ++wcleared;
        if (all) ++wi;
        else worldProxies_.erase(wi++);
    }
    _snprintf(b, sizeof(b) - 1, "[leave] cleared worldProxies=%u stale=%u",
              wcleared, wstale);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    if (all) {
        // Every remote player is gone: drop every map (proxyByKey_ included)
        // back to freshly-launched state.
        resetSession();
        return;
    }

    // ---- One player left; everyone else's state stays exactly as it was. ----
    // Its drive targets: the stream that wrote them has ended. The pointer
    // caches keyed by those streams go with them.
    unsigned int nTargets = 0;
    for (std::set<Key>::const_iterator gi = gone.begin(); gi != gone.end(); ++gi) {
        if (targets_.erase(*gi)) ++nTargets;
        unresolvedHands_.erase(*gi);
        spawnReq_.erase(*gi);
    }
    for (std::map<Character*, Key>::iterator ci = canonicalOf_.begin();
         ci != canonicalOf_.end(); ) {
        if (gone.find(ci->second) == gone.end()) { ++ci; continue; }
        drivenSeen_.erase(ci->first);
        drivenChars_.erase(ci->first);
        canonicalOf_.erase(ci++);
    }
    // REQs asked of the leaver, and recruit edges it proved, can never be
    // answered now.
    for (std::map<Key, SpawnReqState>::iterator si = spawnReq_.begin();
         si != spawnReq_.end(); ) {
        if (si->second.target == leaverId) spawnReq_.erase(si++); else ++si;
    }
    for (std::map<Key, u32>::iterator fi = forceReqHands_.begin();
         fi != forceReqHands_.end(); ) {
        if (fi->second == leaverId) forceReqHands_.erase(fi++); else ++fi;
    }
    // Its pins: hands it recruited/moved fall back to the tab rule, as they did
    // when the whole session reset on a leave.
    unsigned int nPins = 0;
    for (std::map<Key, u32>::iterator pi = pinPeer_.begin(); pi != pinPeer_.end(); ) {
        if (pi->second == leaverId) { pinPeer_.erase(pi++); ++nPins; }
        else ++pi;
    }
    // Its census and camera: existence and attention claims nobody is making.
    census_.erase(leaverId);
    rebuildCensusUnion();
    peerCams_.erase(leaverId);
    // Its cell claims, and the cells it last held: those fall back to the host
    // rather than staying credited to an id that will never census them.
    unsigned int nClaims = 0;
    for (std::map<std::pair<u32, u32>, CellClaim>::iterator ci = claimSlots_.begin();
         ci != claimSlots_.end(); ) {
        if (ci->first.first == leaverId) { claimSlots_.erase(ci++); ++nClaims; }
        else ++ci;
    }
    for (std::map<std::pair<int, int>, u32>::iterator li = cellLastOwner_.begin();
         li != cellLastOwner_.end(); ++li)
        if (li->second == leaverId) li->second = (u32)CELL_OWNER_HOST;
    rebuildClaimedCells();
    // Its clock and reliable-event window: a reconnect starts both afresh.
    peerClock_.erase(leaverId);
    eventSeen_.erase(leaverId);
    forgetPeerChannels(leaverId);
    _snprintf(b, sizeof(b) - 1,
              "[leave] owner=%u targets=%u pins=%u claims=%u (others kept)",
              leaverId, nTargets, nPins, nClaims);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        // Wire v35: index the interp ring on the SENDER's capture time mapped
        // into the local clock, not the arrival time - path jitter (Steam
        // relay) otherwise smears straight into the snapshot spacing and the
        // buffer starves into extrapolation/snap cycles (the jumpy remote-
        // player movement). Mapping = sendMs + min-tracked offset (see
        // PeerClock); clamped to 'now' so a stamp can never land in the future.
        unsigned long t = now;
        if (sendStamp_) {
            PeerClock& pc = peerClock_[it->ownerId];
            long off = (long)(now - (unsigned long)it->sendMs);
            if (!pc.have) {
                pc.offsetMs = off; pc.have = true; pc.lastCreepMs = now;
            } else {
                unsigned long dt = now - pc.lastCreepMs;
                if (dt >= 500) { // creep ~2 ms/s toward slower routes
                    pc.offsetMs += (long)(dt / 500);
                    pc.lastCreepMs = now;
                }
                if (off < pc.offsetMs) pc.offsetMs = off;
            }
            t = (unsigned long)((long)it->sendMs + pc.offsetMs);
            if ((long)(t - now) > 0) t = now;
        }
        const Key k = keyOf(it->e);
        Driven& d = targets_[k];
        // N-player: record WHO streams the hand. Two authors writing one hand
        // (a contested cell, a handover window) must not merge into one interp
        // ring - the two simulations disagree, and interleaving them renders as
        // jitter and teleports. Same total order as the yield/refusal rules:
        // while the incumbent is still streaming, a LOWER-priority author
        // (higher id) is ignored; a higher-priority one takes over. A takeover,
        // or a new author after the incumbent fell silent, starts a fresh ring.
        // With two players there is only ever one remote author, so none of
        // this fires.
        if (d.author != it->ownerId) {
            bool incumbentLive = d.author != AUTHOR_NONE && d.lastSeenMs != 0 &&
                (now - d.lastSeenMs) <= (unsigned long)PEER_STREAM_FRESH_MS;
            if (incumbentLive && it->ownerId > d.author) continue;
            if (d.author != AUTHOR_NONE) {
                d.interp.restartRing();
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "[drive] AUTHOR hand=%u,%u,%u,%u,%u from=%u to=%u (%s)",
                          k.t, k.c, k.cs, k.i, k.s, d.author, it->ownerId,
                          incumbentLive ? "takeover" : "handover");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            d.author = it->ownerId;
            d.authorSinceMs = now;
        }
        d.interp.push(it->e, t, now);
        d.lastSeenMs = now;
    }
}

// Who streams this hand to us (AUTHOR_NONE if nobody ever has).
u32 Replicator::ownerOfDriven(const Key& k) const {
    std::map<Key, Driven>::const_iterator it = targets_.find(k);
    return it == targets_.end() ? AUTHOR_NONE : it->second.author;
}

void Replicator::setOwnedContainerHand(const unsigned int hand[5]) {
    ownedContainers_.clear();
    Key k; k.t = hand[0]; k.c = hand[1]; k.cs = hand[2]; k.i = hand[3]; k.s = hand[4];
    ownedContainers_.insert(k);
}

void Replicator::ingestInv(Inbound& in) {
    std::deque<InboundInv> got;
    in.drainInv(got);
    for (std::deque<InboundInv>::iterator it = got.begin(); it != got.end(); ++it) {
        Key k; k.t = it->cKey[0]; k.c = it->cKey[1]; k.cs = it->cKey[2];
        k.i = it->cKey[3]; k.s = it->cKey[4];
        // Protocol 34: a placer-key row resolves through OUR build maps to
        // the LOCAL building hand (own placement = own hand; the host's
        // placement = our minted proxy). An unresolvable key (mint not
        // landed yet / refused / tombstoned) is dropped - the sender's 5 s
        // safety resend re-delivers once the mint exists.
        if (it->keyKind == 1) {
            unsigned int lh[5];
            if (!localHandForBuildKey(k, lh, it->ownerId)) continue;
            k.t = lh[0]; k.c = lh[1]; k.cs = lh[2]; k.i = lh[3]; k.s = lh[4];
        }
        bool fresh = invRecv_.count(k) == 0;
        InvRecv& r = invRecv_[k];
        if (fresh) r.bench = BENCH_UNKNOWN; // explicit: never trust value-init here
        r.ownerId   = it->ownerId;
        r.items     = it->items; // latest snapshot supersedes
        r.dirty     = true;
        r.truncated = (it->flags & INV_FLAG_TRUNCATED) != 0;
        r.shop      = (it->flags & INV_FLAG_SHOP) != 0;
        r.corpse    = (it->flags & INV_FLAG_CORPSE) != 0;
        r.recvMs    = nowMs();
        if (r.bench == BENCH_UNKNOWN) classifyBench(k, r);
    }
}

void Replicator::latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return; // legacy per-tick ranking needs no state
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabRank_.find(ctnrs[i]) != tabRank_.end()) continue;
        unsigned int next = (unsigned int)tabRank_.size();
        tabRank_[ctnrs[i]] = next;
        if (next >= 2) { // session-start seeding of the standard 2-tab save is silent
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[squad] LATCH cont=%u,%u rank=%u",
                      ctnrs[i].first, ctnrs[i].second, next);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::decideTabs(const EntityState* raw, unsigned int nSquad,
                            const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return;   // legacy per-tick ranking owns nothing durably
    bool seeding = (tabsSeeded_ == 0);
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabOwned_.find(ctnrs[i]) != tabOwned_.end()) continue;
        unsigned int rank = tabRankFor(ctnrs[i], ctnrs);
        TabOwner tv;
        const char* why;
        // The seeding pass, or a save tab whose verdict setOwnRanks dropped
        // (inside the shared-rank prefix, so the rank means the same tab on
        // every client).
        if (seeding || rankIsShared(rank)) {
            // The save's own tabs, ranked identically on every client: the
            // historical rule, so nothing about a normal session changes. A tab
            // that is not ours belongs to the player whose id is its rank
            // (player k drives squad tab k).
            tv.mine  = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
            tv.owner = tv.mine ? localId_ : (u32)rank;
            tv.seeded = true;
            why = "seed";
        } else {
            // A tab that did not exist at session start. Its rank is a local
            // number the others do not share, so the rank rule cannot answer -
            // ask who authored the bodies standing in it. The pins are still
            // present at this moment (the move/recruit that created the tab is
            // what put them there); capturing the answer HERE is what makes it
            // survive the later EXIT that clears them.
            int verdict = 0;   // +1 ours, -1 another player's, 0 nobody claims it
            u32 pinAuthor = (u32)CELL_OWNER_HOST;
            for (unsigned int m = 0; m < nSquad; ++m) {
                if (raw[m].hContainer != ctnrs[i].first ||
                    raw[m].hContainerSerial != ctnrs[i].second) continue;
                Key k = keyOf(raw[m]);
                if (pinOwned_.count(k)) { verdict = 1; break; }
                std::map<Key, u32>::const_iterator pp = pinPeer_.find(k);
                if (pp != pinPeer_.end()) { verdict = -1; pinAuthor = pp->second; }
            }
            if (verdict == 1)       { tv.mine = true;  tv.owner = localId_; }
            else if (verdict == -1) { tv.mine = false; tv.owner = pinAuthor; }
            else { tv.mine = isHostRole(); tv.owner = (u32)CELL_OWNER_HOST; }
            why = (verdict == 1) ? "pin-own"
                                 : ((verdict == -1) ? "pin-peer" : "host-fallback");
        }
        tabOwned_[ctnrs[i]] = tv;
        if (!seeding) {
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[squad] TABOWN cont=%u,%u rank=%u own=%d via=%s",
                      ctnrs[i].first, ctnrs[i].second, rank, tv.mine ? 1 : 0, why);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Freeze the shared-rank prefix at the end of the seeding pass.
    if (seeding && !ctnrs.empty()) tabsSeeded_ = (unsigned int)tabRank_.size();
}

bool Replicator::ownsTab(const std::pair<u32, u32>& key, unsigned int rank) const {
    std::map<std::pair<u32, u32>, TabOwner>::const_iterator it = tabOwned_.find(key);
    if (it != tabOwned_.end()) return it->second.mine;
    // Not yet decided (squadSync_ off, or the first tick of a fresh container):
    // the historical rule, so this is never MORE permissive than before.
    return ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
}

void Replicator::sharedTabs(std::vector<std::pair<std::pair<u32, u32>, u32> >& out) const {
    out.clear();
    for (unsigned int i = 0; i < lastCtnrs_.size(); ++i) {
        const unsigned int rank = tabRankFor(lastCtnrs_[i], lastCtnrs_);
        if (squadSync_ && !rankIsShared(rank)) continue;
        out.push_back(std::make_pair(lastCtnrs_[i], (u32)rank));
    }
}

u32 Replicator::tabOwnerOf(const std::pair<u32, u32>& key, unsigned int rank) const {
    std::map<std::pair<u32, u32>, TabOwner>::const_iterator it = tabOwned_.find(key);
    if (it != tabOwned_.end()) return it->second.mine ? localId_ : it->second.owner;
    return ownsTab(key, rank) ? localId_ : (u32)rank;
}

unsigned int Replicator::tabRankFor(const std::pair<u32, u32>& key,
                                    const std::vector<std::pair<u32, u32> >& ctnrs) const {
    if (squadSync_) {
        std::map<std::pair<u32, u32>, unsigned int>::const_iterator it =
            tabRank_.find(key);
        return it == tabRank_.end() ? 0xFFFFFFFFu : it->second;
    }
    return (unsigned int)(std::lower_bound(ctnrs.begin(), ctnrs.end(), key)
                          - ctnrs.begin());
}

void Replicator::logSmoothSummary() {
    float zeroFrac = (activeFrames_ > 0)
                         ? (float)zeroWhileActive_ / (float)activeFrames_
                         : 0.0f;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO SMOOTH active=%lu zeroWhileActive=%lu zeroFrac=%.3f maxStep=%.3f slewSkip=%lu",
              activeFrames_, zeroWhileActive_, zeroFrac, maxStep_, slewSkipFrames_);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // zeroFrac population audit (see the zp* declarations): what the frozen
    // frames actually WERE. Everything but `free` is a body that could not have
    // walked that frame, so it bounds how much of zeroFrac is even addressable.
    char zp[208];
    _snprintf(zp, sizeof(zp) - 1,
              "SCENARIO ZEROPOP zero=%lu down=%lu carried=%lu furn=%lu chain=%lu "
              "crawl=%lu sneak=%lu squadIdle=%lu alias=%lu stall=%lu",
              zeroWhileActive_, zpDown_, zpCarried_, zpFurn_, zpChain_,
              zpCrawl_, zpSneak_, zpSquadIdle_, zpAlias_, zpStall_);
    zp[sizeof(zp) - 1] = '\0';
    coop::logLine(zp);

    // Motion-onset audit (see the onset* declarations): is zeroFrac reporting
    // stutter, or is it reporting how often the body re-entered scoring? Each
    // bucket's fraction is what to read - if early runs far above steady, the
    // metric is charging render catch-up at motion onset, and the low-sample
    // runs fail because bursty motion pays that toll repeatedly.
    float eF = (onActiveEarly_  > 0) ? (float)onZeroEarly_  / (float)onActiveEarly_  : 0.0f;
    float mF = (onActiveMid_    > 0) ? (float)onZeroMid_    / (float)onActiveMid_    : 0.0f;
    float sF = (onActiveSteady_ > 0) ? (float)onZeroSteady_ / (float)onActiveSteady_ : 0.0f;
    char on[240];
    _snprintf(on, sizeof(on) - 1,
              "SCENARIO ONSET reentries=%lu earlyN=%lu earlyZero=%lu earlyFrac=%.3f "
              "midN=%lu midZero=%lu midFrac=%.3f steadyN=%lu steadyZero=%lu steadyFrac=%.3f",
              onsetReentries_, onActiveEarly_, onZeroEarly_, eF,
              onActiveMid_, onZeroMid_, mF,
              onActiveSteady_, onZeroSteady_, sF);
    on[sizeof(on) - 1] = '\0';
    coop::logLine(on);

    // Anim-truth oracle: fraction of translating frames that did NOT report a
    // real walk state. Low == engine is walking the body (Stage 3 goal); high ==
    // the body slides a static pose (the float bug).
    unsigned long floatFrames = (translateFrames_ > walkTruthFrames_)
                                    ? (translateFrames_ - walkTruthFrames_) : 0;
    float floatFrac = (translateFrames_ > 0)
                          ? (float)floatFrames / (float)translateFrames_
                          : 0.0f;
    char a[160];
    _snprintf(a, sizeof(a) - 1,
              "SCENARIO ANIM translate=%lu walkTruth=%lu floatFrac=%.3f",
              translateFrames_, walkTruthFrames_, floatFrac);
    a[sizeof(a) - 1] = '\0';
    coop::logLine(a);

    // March-in-place oracle: of the at-rest frames, fraction where the body played
    // a walk clip while NOT moving. High == "walking on the spot" (the failure the
    // float oracle cannot see, e.g. a host-seated NPC stuck walking on the join).
    float marchFrac = (restSampleFrames_ > 0)
                          ? (float)marchFrames_ / (float)restSampleFrames_
                          : 0.0f;
    // hold/settle/rlps attribute those march frames (see the counter decls). They
    // are appended AFTER marchFrac so Test-MarchInPlace's regex still matches.
    char m[224];
    _snprintf(m, sizeof(m) - 1,
              "SCENARIO MARCH restSamples=%lu march=%lu marchFrac=%.3f "
              "hold=%lu settle=%lu rlps=%lu holdDip=%lu holdStop=%lu",
              restSampleFrames_, marchFrames_, marchFrac,
              marchHold_, marchSettle_, marchRelapse_,
              marchHoldDip_, marchHoldStop_);
    m[sizeof(m) - 1] = '\0';
    coop::logLine(m);

    // Step-2 pruning evidence: how often the legacy quieting patchwork actually
    // fired this run. Sustained relapse=0 across regressions = the I11 re-quiet is
    // dead code under default AI-suspend and can be deleted; detach counts feed the
    // KENSHICOOP_NO_DETACH A/B decision.
    char q[160];
    _snprintf(q, sizeof(q) - 1,
              "SCENARIO QUIET relapse=%lu sitOrders=%lu detach=%lu noDetach=%d "
              "crawlPhys=%lu",
              quietRelapse_, sitOrders_, detachUses_, noDetach_ ? 1 : 0,
              crawlPhysRestore_);
    q[sizeof(q) - 1] = '\0';
    coop::logLine(q);

    // Step-4 evidence: how much of the driven set the divergence gate handed back
    // to local AI. grants>0 = the mechanism engages; the npc_track oracle proves
    // trusted bodies still track.
    if (gateAuthority_) {
        char t[128];
        _snprintf(t, sizeof(t) - 1,
                  "SCENARIO TRUST grants=%lu revokes=%lu", trustGrants_, trustRevokes_);
        t[sizeof(t) - 1] = '\0';
        coop::logLine(t);
    }

    // Step-5 evidence: suppression churn under the hysteresis band (split_interest
    // metric; boundary flip-flops show up as high counts).
    char au[112];
    _snprintf(au, sizeof(au) - 1,
              "SCENARIO AUTH suppresses=%lu restores=%lu", authSuppresses_, authRestores_);
    au[sizeof(au) - 1] = '\0';
    coop::logLine(au);

    // Protocol 36 jumpiness evidence: what regime the interp buffer ran in
    // (extrapFrac = starvation share of all samples) and how often the drive
    // layer had to hard-snap / re-path. Under WAN jitter these are the numbers
    // the interp fixes must move.
    {
        unsigned long total = interpLerp_ + interpSingle_ + interpClampOld_ +
                              interpExtrap_ + interpSegSnap_;
        float extrapFrac = (total > 0)
                               ? (float)(interpExtrap_ + interpClampOld_) / (float)total
                               : 0.0f;
        char ip[240];
        _snprintf(ip, sizeof(ip) - 1,
                  "SCENARIO INTERP samples=%lu lerp=%lu extrap=%lu clamp=%lu seg=%lu "
                  "extrapFrac=%.3f snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu "
                  "restFlip=%lu pruned=%lu",
                  total, interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
                  extrapFrac, hardSnapSquad_, hardSnapNpc_,
                  walkReissueSquad_, walkReissueNpc_, restFlipNpc_, authPruned_);
        ip[sizeof(ip) - 1] = '\0';
        coop::logLine(ip);
    }

    // Step-3 evidence: how many locally-simulated melee hits the damage guard
    // intercepted vs passed through. guarded>0 in a combat scenario proves the
    // hook engaged (complements the blood-flat vitals check).
    if (dmgGuard_) {
        unsigned long guarded = 0, passed = 0;
        engine::damageGuardStats(&guarded, &passed);
        char dg[112];
        _snprintf(dg, sizeof(dg) - 1,
                  "SCENARIO DMGGUARD guarded=%lu passed=%lu", guarded, passed);
        dg[sizeof(dg) - 1] = '\0';
        coop::logLine(dg);
    }
}


} // namespace coop
