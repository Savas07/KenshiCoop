// FocusGuard - keep an automated test instance off the user's desktop.
//
// Test runs launch two to six games while the machine's owner keeps using it.
// Kenshi takes the foreground when its window comes up (and again around the
// load), which pulls the owner out of whatever they were doing - including a
// fullscreen game - once per launch. Armed by KENSHICOOP_NO_FOCUS=1, which only
// the automated runner (scripts\run_test.ps1) sets; real sessions never see it.
//
// Two layers, both main-thread only:
//   * install(): a thread WH_CBT hook that refuses every activation of this
//     thread's windows and creates top-level windows at the bottom of the
//     z-order. It only covers windows created on the installing thread, so
//     tick() also hooks the thread it runs on if that is a different one.
//   * tick(): if this process holds the foreground anyway (the window existed
//     before the hook, or a path the hook does not see), hand it back to the
//     last foreground window outside every Kenshi process, and push this
//     process's windows below that window whenever they rise above it.
#ifndef KENSHICOOP_FOCUSGUARD_H
#define KENSHICOOP_FOCUSGUARD_H

namespace coop {
namespace focusguard {

// Arm the guard on the calling thread. No-op unless enabled; a second call is
// ignored. Logs one "[focus]" line.
void install(bool enabled);

// Call from every main-thread safe point (title screen and gameplay). Cheap:
// one GetForegroundWindow per call, the rest at most every 200 ms.
void tick();

} // namespace focusguard
} // namespace coop

#endif
