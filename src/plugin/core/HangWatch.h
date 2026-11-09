// HangWatch - say where the main thread is when it stops ticking.
//
// Why this exists: a join froze in the Holy Nation town in about 2 of 25 chase
// runs. Its log simply stopped after a third first-chance Ogre
// InvalidParametersException while the net thread kept logging, so nothing said
// where the main thread was stuck. The fault tracer (core/CrashDump.h) cannot
// help, because nothing faulted, and tools\_dumplive.ps1 only helps if somebody
// is watching when the game wedges - nobody is during an automated run.
//
// A background thread watches a timestamp the gameplay tick writes. After
// STALL_MS without a tick it suspends the main thread for the few microseconds
// it takes to read its context and walk its stack, resumes it, and logs the
// frames as module+RVA ("[hang]" lines, the form the fault tracer uses, which
// KenshiCoop.map and the engine RE notes resolve). A second sample 40 s later
// tells a spin (frames differ) from a wait (frames identical). If the main
// thread ticks again afterwards, one line says for how long it was gone, which
// is what separates a long hitch from a freeze.
//
// Loads: a mid-session load tears the world down and rebuilds it synchronously
// on the main thread, so silence right after a load edge gets a much longer
// threshold. Title-screen ticks disarm the watch, so loads from the menu are
// never sampled.
//
// On by default; KENSHICOOP_HANG_WATCH=0 turns it off.
#ifndef KENSHICOOP_HANGWATCH_H
#define KENSHICOOP_HANGWATCH_H

namespace coop {
namespace hangwatch {

// Start the watchdog thread. loadSerial is the engine's count of loads that
// proceeded (coop::engine::loadEdgeSerial); a change marks a load. A second
// call is ignored. Logs one "[hang]" line.
void install(unsigned int (*loadSerial)());

// Call from every main-thread safe point: gameplay=true from the gameplay tick
// (stamps and arms; the first call also picks the thread to watch), false from
// the title screen (stamps and disarms). Cheap: one GetTickCount per call.
void tick(bool gameplay);

} // namespace hangwatch
} // namespace coop

#endif
