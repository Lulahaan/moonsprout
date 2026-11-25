#pragma once
// Restarting the app to apply restart-only settings, or to return to the start menu
// (docs/launcher/SETTINGS.md 11). Declared here so every package can call it; implemented in
// port/launcher/restart.cpp by package W4.
//
// Pending changes: mppm::settings_restart_pending(gameBooted) in settings.hpp lists the
// Apply::Restart vars that differ from their value at startup.

namespace mppm::launcher {

enum class RestartTarget {
    StartMenu, // come back to the start menu (re-exec with --launcher)
    Play,      // come back straight into the game (re-exec with --play)
    SameAsNow, // the start menu if it is open now, else the game
};

// Flushes the config, runs the AURORA_EXIT teardown and re-executes the binary with the original
// arguments, `--launcher` or `--play` replacing any earlier one. A disc given on the command line
// stays there, unless a different, usable disc was chosen since (it is dropped, and MPPM_DISC with
// it); a --cvar whose var a choice in Settings released is dropped too. Does not return on success.
void request_restart(RestartTarget target);

// Whether the game has booted in this process (in game, or behind the live start menu). Before
// that, a changed disc applies on Play and is not a pending restart. Latched by note_game_booted;
// also true whenever the start menu is closed.
bool game_booted();

// Called by the boot flow when the game boots behind the start menu (show_prelaunch). Sets the
// game_booted() latch and tells live::on_game_booted.
void note_game_booted();

// True when a restart-only setting differs from its value at startup
// (settings_restart_pending(game_booted()) is not empty). The start menu checks it on Play and then
// restarts with RestartTarget::Play, so the game always starts with what the user chose. With the
// start menu open only a relaunch still scheduled by live_apply counts (and the Save Manager's).
bool restart_pending();

} // namespace mppm::launcher
