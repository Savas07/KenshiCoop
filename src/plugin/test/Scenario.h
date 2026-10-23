// Scenario - deterministic, log-emitting test scripts driven from the tick hook.
//
// A scenario runs on BOTH clients (it branches on ctx.isHost). The authoritative
// side emits "SCENARIO MEMBER hand=.. pos=.." lines; the observing side emits
// "SCENARIO RECV hand=.. pos=.." lines. The PowerShell runner cross-checks
// MEMBER vs RECV positions per hand within a tolerance, in addition to requiring
// "SCENARIO RESULT PASS" from each client. Keep these schema strings stable -
// run_test.ps1 parses them.

#ifndef KENSHICOOP_SCENARIO_H
#define KENSHICOOP_SCENARIO_H

#include <string>
#include "../../netproto/Wire.h"

class GameWorld;
class Character;

namespace coop {

// recruit_join: the replicator's per-body view of ONE local character - who
// streams it (owned = its local hand is in this tick's owned set), the
// ownership pins on its local hand, whether this tick drove / AI-suspended it,
// whether our existence judgment has it hidden (suppressed), and the peer key
// it is bound under (canonical drive stamp or proxy binding). Flags are 0/1.
// wire[] is zero when the body has no peer binding; local[] is the live
// readObjectHand. Same [t,c,cs,i,s] layout as every hand here.
struct BodyRoleView {
    int          owned, pinOwn, pinPeer, driven, aiSusp;
    int          canon, proxy, minted, squad, tabOwn;
    int          suppressed;
    unsigned int local[5];
    unsigned int wire[5];
};

struct ScenarioContext {
    GameWorld*    gw;
    bool          isHost;
    u32           localId;
    unsigned long elapsedMs; // since onStart
    unsigned int  tick;      // scenario tick counter
    // True once this client has received an owned-entity batch from a peer. On the
    // HOST this means the JOIN is loaded + streaming, so it is the correct gate for
    // any time-sensitive host action (e.g. a live spawn) that the join must witness.
    // Always false in HostOnly runs (no peer) - gate with a timeout fallback there.
    bool          peerReady;
    // assault_mint: describes the MINTED proxy nearest the body at refHand -
    // outLocal[5] is the hand that resolves on THIS client (what an order needs),
    // outCanon[5] the peer's key it is streamed by, outDist the planar distance.
    // Only the replicator's proxy table can answer this, and a scenario must not
    // reach into the replicator, so Plugin.cpp supplies the adapter (0 when
    // unavailable - always check).
    bool        (*pickMintedProxy)(const unsigned int refHand[5],
                                   unsigned int outLocal[5],
                                   unsigned int outCanon[5], float* outDist);
    // recruit_join: the replicator's view of body 'c' (see BodyRoleView). Same
    // adapter reason as pickMintedProxy. False when 'c' has no readable hand.
    bool        (*bodyRole)(Character* c, BodyRoleView* out);
    // host_pc_fight: Replicator::scenarioReplicaInfo through the same kind of
    // adapter - the canonical (wire) key of the body at a local hand plus its
    // copy's drive state (regime DevRegime or -1, newest streamed task/subject,
    // outstanding walk destination). 0 when unavailable - always check.
    bool        (*replicaInfo)(const unsigned int localHand[5], unsigned int outCanon[5],
                               int* outRegime, unsigned int* outTask,
                               unsigned int outSubj[5], bool* outDest);
};

class Scenario {
public:
    virtual ~Scenario() {}
    virtual const char* name() const = 0;
    // Called EVERY tick between local gameplay start and peer-ready arming.
    // Scenarios that must capture a subject while the freshly-loaded world is
    // still in its baked pose (the craft worker at the prop, the duelists at
    // their spawn) PIN it on the first call and HOLD it on subsequent calls -
    // the arming wait (a join-load, ~10-20 s) is long enough for faction AI to
    // walk an unpinned subject away from where the save baked it. ctx.elapsedMs
    // here is time since gameplay start (NOT the armed scenario clock).
    virtual void onGameplay(const ScenarioContext&) {}
    // Called ONCE when the scenario ARMS: at peer-ready (the first owned-entity
    // batch from the peer - on the host, "the join is loaded + streaming"), or
    // at the arm-timeout fallback. ctx.elapsedMs is measured from THIS moment,
    // so every scripted action happens with the peer watching.
    virtual void onStart(const ScenarioContext& ctx) = 0;
    // Returns true when the scenario is complete (caller logs RESULT then holds).
    virtual bool onTick(const ScenarioContext& ctx) = 0;
    virtual bool passed() const = 0;
};

// Build the scenario named 'name', or 0 if unknown.
Scenario* makeScenario(const std::string& name);

} // namespace coop

#endif // KENSHICOOP_SCENARIO_H
