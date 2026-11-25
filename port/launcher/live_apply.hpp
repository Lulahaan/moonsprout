#pragma once
// Start-menu settings that apply at once (live_options_design 3.5, 3.8): the applier that decides,
// per changed setting, whether it is already live, needs a short relaunch, or waits for the boot,
// and the relaunch-and-resume that brings the menu back on the same Settings row.
//
// Called from the boot flow (launcher.cpp), restart.cpp and the settings window. Everything here
// runs on the main thread.
//
// While the start menu is open, every frame (update):
// - each change logs `mppm: live: <key> -> live|relaunch|nothing`;
// - a restart-only var whose menu action (settings.hpp, menu_action) is not Relaunch is re-based
//   (it is live, or the boot reads it), as is a graphics backend that resolves to the running one;
//   a disc that does not inspect as Ready is left alone (the game keeps its disc);
// - whatever is left schedules one relaunch: 350 ms after the change, once the menu is idle, a
//   navy veil closes over the window (150 ms), and the process re-executes with
//   MPPM_RESUME="tab=<title>;key=<settings key>;scroll=<px>;x=<px>;y=<px>;max=<0|1>;t=<ms>" and
//   MPPM_SEAMLESS_RESTART=1. The new process puts the window and the Settings row back under the
//   veil, which fades out from its third frame, and logs `mppm: relaunch: resumed in N ms`.
// In game nothing here acts (no `mppm: live:` line): Settings keeps its banner and badges.

#include <string>
#include <string_view>

namespace borealis::config {
class VarBase;
}

namespace mppm::launcher::live {

// launcher::preinit, after the --cvar and environment overrides: reads MPPM_RESUME (and unsets it).
void preinit();
// launcher::init, after the runtime settings are bound: captures the boot backend, hooks the
// start-menu-only appliers (MSAA) and restores the window position of a relaunch.
void init();
// launcher::update, every frame after scene::update: applies or schedules what changed while the
// start menu is open.
void update();
// Before the start menu's first frame (both menu paths): the veil and the Settings window of a
// relaunch, put back with no entrance.
void restore_ui();
// From note_game_booted: settings read at boot (disc, save format, audio) now count as relaunch.
void on_game_booted();

// A relaunch is under way (the veil is up); the boot flow must not boot the game now.
bool relaunching();
// A relaunch is scheduled for a change that is still pending.
bool pending_relaunch();
// The row for this var shows "Applying...".
bool busy(const borealis::config::VarBase& var);

// The graphics backend id this process was started with (captured in init()).
const std::string& boot_backend_id();

} // namespace mppm::launcher::live
