#pragma once
// Window and picture state: the one place that turns VIDEO settings into Aurora calls
// (docs/launcher/ASPECT.md 5, SETTINGS.md 3).
//
// apply() is the only caller of AuroraSetViewportPolicy, VILockAspectRatio / VIUnlockAspectRatio
// and VISetFrameBufferScale, and the only reader of video.aspectRatio, interface.hudLayout and
// interface.hudMaxAspect (it pushes them into port/sdk/wide.cpp with p2_wide_configure).
// Settings needed before the window exists go into AuroraConfig instead (launcher::preinit).

#include <aurora/aurora.h>

struct SDL_Window;

namespace mppm::video {

// After aurora_initialize: applies the current settings and re-applies on every change.
// `window` is AuroraInfo::window, used to read the display's refresh rate (may be nullptr).
void init(SDL_Window* window);

// Refresh rate of the display the window is on, in Hz; 0 when unknown (headless, no mode).
// Re-read on refresh_display() (window moved to another display, display mode changed).
float display_refresh_hz();

// SDL_EVENT_WINDOW_DISPLAY_CHANGED / SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: re-reads the refresh.
void refresh_display();

// Bumped whenever display_refresh_hz() changes, so cached derivations (frame_policy) can follow.
unsigned display_generation();

// Brings Aurora in line with the settings. Idempotent; cheap enough to call on any change.
void apply();

// Switches the frame buffers to `samples` (1 or 4; anything else is clamped) at the next surface
// refresh. Only the start menu calls it (live_apply): in game video.msaa still needs a restart.
void set_msaa(int samples);

// While the start menu, or any launcher window in game, is open the aspect behaves as `auto`
// whatever the setting says, so the UI fills the window (ASPECT.md 5; the UI is drawn inside the
// content rectangle). The launcher sets this when they open and close; the held game picture is
// redrawn at the new shape (port/main.cpp).
void set_start_menu_override(bool on);

// F11 / Alt+Enter and the settings row: flips video.fullscreen (apply() follows).
void toggle_fullscreen();

// "Reset Window Size": windowed 1280x960 in the middle of the screen; forgets the saved size.
void reset_window_size();

// AURORA_WINDOW_RESIZED: remembers the windowed size in video.windowWidth / windowHeight.
void on_window_resized(const AuroraWindowSize& size);

} // namespace mppm::video
