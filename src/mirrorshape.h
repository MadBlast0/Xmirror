#pragma once

// Holds the mirror window to the mirrored picture's shape.
//
// The window belongs to the GStreamer video sink and runs its message loop on a
// GStreamer thread. Its window procedure is replaced -- allowed within one
// process -- so an interactive resize can be constrained, and a "Lock aspect
// ratio" item is added to its title-bar (system) menu. Nothing here repaints,
// reparents or otherwise takes the window over from the sink.
//
// Window handles are passed as void* so this header does not pull in windows.h.
namespace MirrorShape {

// Called on the window's own thread when the user toggles the lock from the
// title-bar menu. Post to the GUI thread before touching any UI.
using LockToggled = void (*)(bool locked);

// Hooks `window` and adds the menu item. Safe to call again for the same window.
void install(void *window, bool locked, LockToggled onToggled);

// Restores the window's own procedure, for a window that outlives its session.
void release(void *window);

// The picture's width / height. 0 means unknown, which disables the constraint.
void setAspect(double aspect);

// Updates the constraint and the menu check mark.
void setLocked(void *window, bool locked);

// Resizes `window` so its client area has the picture's shape: the current
// height is kept and the width derived, as the client does on rotation, then it
// is shrunk to fit its monitor and kept centred. Does nothing to a minimised,
// maximised or fullscreen window. Returns true if it asked for a new size.
bool fit(void *window, int pictureWidth, int pictureHeight);

}  // namespace MirrorShape
