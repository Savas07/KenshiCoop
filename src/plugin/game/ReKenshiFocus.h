// ReKenshiFocus - keeps RE_Kenshi's focus handler from faulting while the game
// has no player.
//
// RE_Kenshi registers an Ogre::WindowEventListener (class WindowFocusListener)
// on the render window and never removes it. Its windowFocusChange(rw) returns
// when the window is active; when it is not, it reads ou->player->camera
// without a null check to drop free-camera mode. Any WM_ACTIVATE(WA_INACTIVE)
// while ou->player is null therefore faults reading 0x30 at
// RE_Kenshi.dll+0x381e9. Game exit does exactly that: the world is freed
// first, then ~D3D11RenderWindowHwnd -> destroy() -> DestroyWindow, and
// Windows sends the focused window WM_ACTIVATE(WA_INACTIVE) from inside
// DestroyWindow (a player's 2026-10-23 host log: the game faulted while
// closing from the title, its stack exactly that chain).
//
// install() swaps that vtable slot for a wrapper that skips the handler when
// there is no player - there is no free camera to leave then - and calls it
// unchanged otherwise.
#ifndef KENSHICOOP_REKENSHIFOCUS_H
#define KENSHICOOP_REKENSHIFOCUS_H

namespace coop {
namespace rekfocus {

// Find RE_Kenshi's WindowFocusListener vtable through its RTTI and swap the
// windowFocusChange slot. Logs the outcome; returns false, changing nothing,
// when RE_Kenshi or the class isn't found.
bool install();

// Test-only (KENSHICOOP_TEST_FOCUS_NOPLAYER): recreate the exit fault's state
// at the title - clear ou->player, send our own render window
// WM_ACTIVATE(WA_INACTIVE) (same thread, so Ogre's WndProc and every listener
// run inside the call), then restore the player and reactivate. Unguarded it
// faults at RE_Kenshi.dll+0x381e9 like the exit crash. Deliberately not
// SEH-guarded. Main thread only.
void testFocusLossWithoutPlayer();

} // namespace rekfocus
} // namespace coop

#endif
