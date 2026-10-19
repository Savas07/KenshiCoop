// DevTools.h - developer tooling for co-op sync debugging (docs/DEVTOOLS.md).
//
// Everything here is OFF unless Config::devtools is set (coop_config.json
// "devtools": 1 or KENSHICOOP_DEVTOOLS=1). Off means: no sockets opened, no
// per-tick engine probes, no files written, no labels created.
//
// Pieces (one TU each):
//   DevTools.cpp    telemetry (UDP JSON to the dashboard), command channel
//                   (UDP text commands executed on the main thread), desync
//                   watchdog (rules + per-hand ring buffer + dump files)
//   DevOverlay.cpp  in-world markers: nameplate / model (pelvis bone) / physics
//                   body positions + a status caption per squad character
//   PacketRecorder  (PacketRecorder.h) every ENet packet in/out to a .kcrec file
//
// Threading: everything in this header is MAIN (game) thread only.
// Toolchain: C++03 / VC++2010; SEH frames must hold no destructible objects.

#ifndef KENSHICOOP_DEVTOOLS_H
#define KENSHICOOP_DEVTOOLS_H

#include <string>
#include <vector>

class GameWorld;

namespace coop {

class Replicator;

// What the replicator is doing with a driven body this tick.
enum DevRegime {
    DR_NONE = 0,   // not driven this tick (no fresh sample / skipped)
    DR_OWN,        // locally owned: we stream it, nothing drives it
    DR_PAUSED,     // consensus sim paused: hold
    DR_DOWN,       // down/KO/dead enforcement
    DR_CARRIED,    // on someone's shoulder (local attach owns the transform)
    DR_CARRIER,    // carrying someone
    DR_FURN,       // bed / cage / chained
    DR_CRAWL,      // crippled crawl drive
    DR_COMBAT,     // combat intent reproduction
    DR_GLIDE,      // squad glide (placement + gait mirror)
    DR_SNAP,       // hard snap this tick
    DR_WALK,       // walk drive (move orders)
    DR_REST,       // rest / park
    DR_REFUSED,    // cell authority refused the stream
    DR_COUNT
};
const char* devRegimeName(int r);

// One player-squad character as THIS client sees it. POD.
struct DevBody {
    unsigned int hand[5];      // type, container, containerSerial, index, serial
    char  name[40];
    bool  own;                 // we stream it (locally controlled)
    unsigned int author;       // player id streaming it (driven); our id when own
    // Local engine truth
    bool  havePos;  float np[3];          // Character::getPosition (nameplate, oracles)
    bool  haveMesh; float mesh[3];        // rendered skeleton pelvis bone
    float foot;                           // left-right foot distance (-1 unknown); swings when a gait clip plays
    bool  haveHk;   float hk[3];          // Havok character position, WORLD units (0 = none: model cannot follow placement)
    unsigned short bs;                    // local readBodyState
    int   task;                           // local task key (65535 none)
    bool  carrying, carried;              // local carry relationship
    bool  localMoving; float localSpeed;  // CharMovement currentlyMoving/currentSpeed (NOTE: written by our gait mirror on driven copies)
    // Drive state (driven bodies; zeros for own)
    int   regime;                         // DevRegime
    bool  fresh;                          // a sample arrived recently
    float gap;                            // |newest stream sample - our position|
    float vlen;                           // stream velocity (wall units/s)
    float cSpeed;                         // owner's own currentSpeed (game units/s)
    bool  streamMoving;                   // owner's currentlyMoving
    unsigned short streamBs;
    unsigned short streamTask;
    bool  glide;                          // in the glide regime
    bool  haveDest;                       // a walk order is outstanding
    float newest[3];                      // newest stream sample position
};

struct DevGlobal {
    bool  isHost;
    unsigned int localId;     // our network player id (0 host)
    unsigned int peers;       // connected peers
    bool  netRunning;
    bool  worldLive;
    float timeSlew;           // join clock catch-up rate (1 = none)
    float speed;              // consensus game speed
    bool  paused;
};

namespace dev {

struct DevConfig {
    bool enabled;
    bool overlay;             // overlay on at start
    bool record;              // record every network session
    bool watchdog;
    int  telemetryPort;       // UDP 127.0.0.1:<port> telemetry target
    bool isHost;
    std::string baseDir;      // folder for kc_records\ and kc_dumps\ (game working dir)
    std::string version;      // KENSHICOOP_VERSION
};

void init(const DevConfig& cfg);   // once, after config load (title screen)
void shutdown();
bool enabled();

// Every main-loop tick while the world is live, AFTER Replicator::applyTargets.
// bodies = Replicator::devSnapshot() of this tick (only built when enabled()).
void tick(GameWorld* gw, Replicator& repl, const std::vector<DevBody>& bodies, const DevGlobal& g);

// Every main-loop tick while NO world is live (title / loading): keeps the
// command channel answering and drops overlay labels.
void idleTick();

// Role flips (in-game panel reconnect as host/join).
void setRole(bool isHost);

} // namespace dev

namespace overlay {
// Main thread. on=false removes every label it created. Labels exist only for
// bodies in 'bodies'; a body that disappears loses its labels the same tick.
void tick(GameWorld* gw, const std::vector<DevBody>& bodies, bool on);
void clear();
} // namespace overlay

} // namespace coop

#endif // KENSHICOOP_DEVTOOLS_H
