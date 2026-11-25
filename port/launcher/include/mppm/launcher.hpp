#pragma once
// The launcher: the start menu shown before the game (disc selection, settings, update check) and
// the in-game menu. Ordinary host C++ built on RmlUi through Aurora; the game reaches it only
// through this header (port/main.cpp) and the C hooks in p2_host.h.
//
// Boot flow (docs/launcher/README.md 3), decided by boot_mode() once init() has run:
//
//   disc known, Skip Start Menu (MPPM_SKIP_LAUNCHER) or --play      -> Direct
//   no disc known                                                 -> StandaloneMenu
//   disc on the command line / MPPM_DISC                          -> LiveMenu
//   disc remembered, Live Start Menu on (and the scene available) -> LiveMenu
//   disc remembered, Live Start Menu off                          -> StandaloneMenu
//
// MPPM_FORCE_LAUNCHER=1 or --launcher show the start menu even when Skip Start Menu is on.

#include <aurora/aurora.h>

#include <string>

union SDL_Event;
struct AuroraEvent;

namespace mppm::launcher {

// Parses the command line (disc, --cvar key=value, --play, --launcher), loads the settings file,
// applies the --cvar and MPPM_* overrides, mirrors game-facing values into gP2Config and fills
// the AuroraConfig fields that must be known before aurora_initialize (window, backend, MSAA,
// vsync...). Call first.
void preinit(AuroraConfig& config, int argc, char** argv);

// After aurora_initialize: fonts, stylesheets, video state, runtime setting hooks.
bool init(const AuroraInfo& info);

// Every SDL event Aurora reports (AURORA_SDL_EVENT).
void handle_event(const SDL_Event& event);

// Aurora's own events other than AURORA_SDL_EVENT and AURORA_EXIT (window resized, ...).
void handle_aurora_event(const AuroraEvent& event);

// Once per presented frame, after aurora_begin_frame succeeded (main/game thread).
void update();

enum class BootMode {
    Direct,         // boot the game now; no start menu
    LiveMenu,       // boot behind the start menu (live scene); Play runs the take-off
    StandaloneMenu, // run the start menu on its own; the game boots when Play is pressed
};

// How this run starts (see the table above). Valid after init().
BootMode boot_mode();

// The disc to boot: a command-line/env path, or the remembered one if it still looks like a
// Pikmin 2 disc. Empty when the user has to pick one in the start menu.
std::string startup_disc();

// LiveMenu: shows the start menu over the booting game and arms the scene bridge (the scene runs
// parked while Live Start Menu is off). After run_standalone_menu returned keepMenu it reuses that
// menu. Call after the disc is open and before p2_game_main.
void show_prelaunch();

// StandaloneMenu: runs the start menu on its own until the player starts the game. `frame` must
// pump events and present one frame (it returns false when the app is quitting).
struct StandaloneResult {
    std::string disc;      // the disc to boot; empty when the user quit
    bool keepMenu = false; // a usable disc with Live Start Menu on: boot as LiveMenu behind this same menu
};
StandaloneResult run_standalone_menu(bool (*frame)());

// Whether the start menu is up (it forces the window-filling aspect and sets gP2Config.launcherOpen).
bool prelaunch_open();

// Pause While Menu Is Open (game.pauseInMenu, SETTINGS 6): true while a launcher window is up over
// the running game and the setting is on. p2_host_present then keeps presenting frames itself
// (with the last game picture under the UI) and does not return to the game until it is false.
// Also fades the game's sound out while held. Call after update().
bool hold_game();

// tools.logFps: print the frame rate once a second (read per frame by p2_host_present).
bool log_fps();

void shutdown();

} // namespace mppm::launcher
