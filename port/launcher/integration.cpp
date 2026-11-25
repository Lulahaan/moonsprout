#include "integration.hpp"

#include "menu_registry.hpp"
#include "prelaunch.hpp"

#include "controller_config.hpp"
#include "menu_bar.hpp"
#include "mods_window.hpp"
#include "online_status_window.hpp"
#include "online_ui.hpp"
#include "online_window.hpp"
#include "restart.hpp"
#include "disc.hpp"
#include "saves_window.hpp"
#include "settings.hpp"
#include "update_check.hpp"
#include "verify_window.hpp"
#include "mppm/input_defaults.hpp"
#include "mppm/launcher.hpp"
#include "ui/modal.hpp"
#include "ui/settings_window.hpp"
#include "ui/sound.hpp"
#include "ui/ui.hpp"

#include <borealis/config.hpp>
#include <dolphin/pad.h>
#include <fmt/format.h>
#include <p2_host.h>
#include <p2_online.hpp>

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace mppm::launcher::integration {
namespace {

std::vector<borealis::config::Connection> sConnections;

void focus_top() {
    if (auto* doc = ui::top_document()) {
        doc->focus();
    }
}

// ---- Settings window buttons (SettingsAction) ----

void reset_all_controls() {
    using namespace ui;
    const auto dismiss = [](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Reset All Controls",
        .bodyText = "Put every player's buttons, sticks and keys back to their defaults? Dead zones and "
                    "rumble strength stay as they are.",
        .actions =
            {
                {.label = "Cancel", .onPressed = dismiss},
                {.label = "Reset",
                    .onPressed =
                        [](Modal& modal) {
                            PADInit();
                            for (u32 port = 0; port < PAD_CHANMAX; port++) {
                                PADRestoreDefaultMapping(port);
                                // Port 1 always gets its keys back; others only if their keyboard is on.
                                u32 count = 0;
                                if (port == 0 || PADGetKeyButtonBindings(port, &count) != nullptr) {
                                    apply_default_keyboard(port, true);
                                }
                            }
                            PADSerializeMappings();
                            play_sound(kSoundBindingChanged);
                            modal.pop();
                        }},
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "warning",
    }));
    focus_top();
}

void register_settings_actions() {
    using ui::SettingsAction;
    ui::set_settings_action(
        SettingsAction::VerifyDisc, [] { ui::open_verify_disc(); },
        [] {
            const std::string& path = settings().backend.discPath.get();
            return std::string{disc::verification_label(disc::remembered_verification(path))};
        });
    ui::set_settings_action(SettingsAction::SaveManager, [] {
        if (!ui::open_saves_window()) {
            ui::play_sound(ui::kSoundWarning);
        }
    });
    ui::set_settings_action(SettingsAction::ConfigureControllers, [] { ui::open_controller_config(0); });
    ui::set_settings_action(SettingsAction::ResetAllControls, reset_all_controls);
    ui::set_settings_action(
        SettingsAction::CheckForUpdates, [] { update_check::begin(true); },
        [] {
            update_check::poll();
            return update_check::summary();
        });
    // A remembered verification belongs to one file (SETTINGS 2, Verify Disc).
    sConnections.push_back(settings().backend.discPath.on_change([] { disc::forget_verification(); }));
}

// ---- Player Colour Light (SETTINGS 4) ----

struct Rgb {
    u8 r, g, b;
};
constexpr Rgb kPlayerColours[PAD_CHANMAX] = {
    {0xE8, 0x32, 0x2B}, // red
    {0x2F, 0x7B, 0xEA}, // blue
    {0xFF, 0xD2, 0x3F}, // yellow
    {0x8A, 0x4F, 0xC8}, // purple
};
int sLedDevice[PAD_CHANMAX] = {-2, -2, -2, -2}; // controller index each port's light was set for
bool sLedOn = false;

// Sets (or clears) the light of every assigned controller that has one. Only touches a port when
// its device or the setting changed, so a hot-plug or a reassignment in the controller window
// recolours it on the next frame.
void apply_player_leds(bool force) {
    const bool on = settings().input.playerLed.get();
    if (on != sLedOn) {
        force = true;
    }
    for (u32 port = 0; port < PAD_CHANMAX; port++) {
        const int device = PADGetIndexForPort(port);
        if (!force && device == sLedDevice[port]) {
            continue;
        }
        sLedDevice[port] = device;
        if (device < 0 || !PADHasLED(port)) {
            continue;
        }
        const Rgb c = on ? kPlayerColours[port] : Rgb{0, 0, 0};
        PADSetColor(port, c.r, c.g, c.b);
    }
    sLedOn = on;
}

// ---- Mute in Background (SETTINGS 5) ----

bool sFocused = true;
bool sAuroraPaused = false;
bool sGameHeld = false; // Pause While Menu Is Open (SETTINGS 6): the game's sound fades out too

void apply_duck() {
    const bool mute = sAuroraPaused || (!sFocused && settings().audio.muteInBackground.get());
    p2_audio_set_duck(mute || sGameHeld ? 0.0f : 1.0f);
    // The menu's own cues still play while it holds the game (they are the game's system sound
    // effects, port/game/ui_sound.cpp); only a real mute silences them too.
    p2_audio_set_menu_hold(sGameHeld ? 1 : 0, mute ? 0.0f : 1.0f);
}

// ---- UI sounds (README 6) ----

// The game's own sound system plays the cue. Before a disc is booted (and while the game is still
// starting) the game side plays nothing, so the launcher is silent there. Called from
// launcher::update, which runs on the game thread once the game exists.
void game_ui_sound(ui::UiSound sound) {
    p2_game_play_ui_sound(static_cast<int>(sound));
}

// ---- Reset Game (SETTINGS 13) ----

void confirm_reset_game() {
    // The same danger modal as Start Menu and Quit (SETTINGS 13), skipped on the title screen and
    // file select where nothing can be lost.
    ui::confirm_progress_loss("Return to the title screen?", "Reset",
        [] {
            // Close the menu so the game runs again, then start its own reset sequence (fade,
            // sound stop, card idle, OSResetSystem).
            ui::close_all_documents();
            p2_game_request_reset();
        },
        ui::MenuBar::current());
}

void register_menu_entries() {
    // Mods (SETTINGS 17): editable on the start menu, read-only over a running game. The bar entry
    // takes the id of the hidden slot menu_bar.cpp reserves, so it replaces it.
    register_menu_entry(Menu::StartMenu,
        {
            .id = "mods",
            .label = "Mods",
            .order = ui::start_order::kMods,
            .open = [] { ui::open_mods_window(false); },
        });
    register_menu_entry(Menu::InGameBar,
        {
            .id = "mods",
            .label = "Mods",
            .order = bar_order::kMods,
            .visible = [] { return launcher::game_booted() && !launcher::prelaunch_open(); },
            .open = [] { ui::open_mods_window(true); },
        });
    // Multiplayer (SETTINGS 18, M5.3b): host or join an online lobby from the start menu. The bar
    // entry takes the id of the hidden "online" slot menu_bar.cpp reserves, and shows while a lobby
    // or a match exists.
    register_menu_entry(Menu::StartMenu,
        {
            .id = "multiplayer",
            .label = "Multiplayer",
            .order = ui::start_order::kMultiplayer,
            .open = [] { ui::open_online_window(); },
        });
    register_menu_entry(Menu::InGameBar,
        {
            .id = "online",
            .label = "Online",
            .order = bar_order::kOnline,
            .visible = [] { return mppm::online::active() && !launcher::prelaunch_open(); },
            .open = [] { ui::open_online_status_window(); },
        });
    register_menu_entry(Menu::InGameBar,
        {
            .id = "reset",
            .label = "Reset Game",
            .order = bar_order::kResetGame,
            // Only over a running game; the start menu has its own flow. Never online: every machine
            // would have to reset together.
            .visible = [] {
                return launcher::game_booted() && !launcher::prelaunch_open() && !ui::is_prelaunch_open() &&
                       !mppm::online::in_game();
            },
            .open = confirm_reset_game,
        });
}

} // namespace

void init() {
    // Aurora's ImGui is only a debug overlay here; every user setting lives in config.json, so it
    // must not leave an imgui.ini in the data folder (Tools > Open Data Folder shows it). Clearing
    // IniFilename before the first frame stops both the load and the save; a file an older build
    // left behind is removed.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.IniFilename != nullptr) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(io.IniFilename), ec);
            io.IniFilename = nullptr;
        }
    }
    register_settings_actions();
    register_menu_entries();
    ui::online_ui::init();
    ui::set_sound_handler(game_ui_sound);
    PADInit(); // idempotent; the light can be set before the game's own PADInit
    apply_player_leds(true);
    sConnections.push_back(settings().input.playerLed.on_change([] { apply_player_leds(true); }));
    sConnections.push_back(settings().audio.muteInBackground.on_change([] { apply_duck(); }));
}

void set_game_held(bool held) {
    if (held != sGameHeld) {
        sGameHeld = held;
        apply_duck();
    }
}

void update() {
    apply_player_leds(false);
    ui::online_ui::update();
}

void handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        sFocused = false;
        apply_duck();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        sFocused = true;
        apply_duck();
        break;
    default:
        break;
    }
}

void handle_aurora_event(const AuroraEvent& event) {
    switch (event.type) {
    case AURORA_PAUSED:
        sAuroraPaused = true; // always silent while Aurora holds the game
        apply_duck();
        break;
    case AURORA_UNPAUSED:
        sAuroraPaused = false;
        apply_duck();
        break;
    case AURORA_CONTROLLER_ADDED:
        apply_player_leds(true);
        break;
    default:
        break;
    }
}

} // namespace mppm::launcher::integration

extern "C" void p2_host_reset_system(int toMenu) {
    using namespace mppm::launcher;
    std::fprintf(stderr, "mppm: soft reset (%s)\n", toMenu ? "start menu" : "title");
    request_restart(toMenu ? RestartTarget::StartMenu : RestartTarget::Play);
}
