// Default keyboard layout for port 1 (docs/launcher/SETTINGS.md 8). Moved out of port/main.cpp so
// the controller window's "Restore Defaults" can apply the same table.
#include "mppm/input_defaults.hpp"
#include "p2_data_dir.hpp"

#include "branding.hpp"

#include <borealis/io.hpp>
#include <dolphin/pad.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_stdinc.h>

#include <array>
#include <filesystem>
#include <system_error>

namespace mppm::launcher {
namespace {

// WASD stick, IJKL C-stick, Space A, Left Shift B, E X, Q Y, Enter Start, Tab Z, F L, R R, arrows
// D-pad, 1/2 analog L/R.
constexpr PADKeyButtonBinding kButtons[] = {
    {SDL_SCANCODE_SPACE, PAD_BUTTON_A},     {SDL_SCANCODE_LSHIFT, PAD_BUTTON_B},
    {SDL_SCANCODE_E, PAD_BUTTON_X},         {SDL_SCANCODE_Q, PAD_BUTTON_Y},
    {SDL_SCANCODE_RETURN, PAD_BUTTON_START}, {SDL_SCANCODE_TAB, PAD_TRIGGER_Z},
    {SDL_SCANCODE_F, PAD_TRIGGER_L},        {SDL_SCANCODE_R, PAD_TRIGGER_R},
    {SDL_SCANCODE_UP, PAD_BUTTON_UP},       {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
    {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},   {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
};
constexpr PADKeyAxisBinding kAxes[] = {
    {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 0},  {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 0},
    {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 0},  {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 0},
    {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 0}, {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 0},
    {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 0}, {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 0},
    {SDL_SCANCODE_1, PAD_AXIS_TRIGGER_L, 0},   {SDL_SCANCODE_2, PAD_AXIS_TRIGGER_R, 0},
};

constexpr unsigned kDefaultPort = 0; // only Player 1 has a default layout

std::array<bool, PAD_MAX_CONTROLLERS> sStartupDone{};

// Aurora writes keyboard_bindings.dat (in the data folder, which is AuroraConfig.userPath) the
// first time anything is saved from the controller window. Its presence means the player's own
// choices are on disk, including a keyboard they switched off.
bool bindings_saved() {
    const std::string& dir = mppm::data_dir_utf8();
    if (dir.empty()) {
        return false;
    }
    const std::filesystem::path file = borealis::io::fs_path_from_utf8(dir) / "keyboard_bindings.dat";
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec);
}

void bind_defaults(unsigned port) {
    if (port != kDefaultPort) {
        return;
    }
    for (const PADKeyButtonBinding& b : kButtons) {
        PADSetKeyButtonBinding(port, b);
    }
    for (const PADKeyAxisBinding& a : kAxes) {
        PADSetKeyAxisBinding(port, a);
    }
}

} // namespace

bool apply_default_keyboard(unsigned port, bool force) {
    if (port >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    PADInit(); // loads the saved bindings the first time; does nothing afterwards
    if (!force) {
        if (sStartupDone[port]) {
            return false;
        }
        sStartupDone[port] = true;
        u32 count = 0;
        if (PADGetKeyButtonBindings(port, &count) != nullptr || bindings_saved()) {
            return false; // saved bindings are active, or the player chose none
        }
        if (port != kDefaultPort) {
            return false;
        }
        bind_defaults(port);
        PADSetKeyboardActive(port, TRUE);
        return true;
    }
    sStartupDone[port] = true;
    u32 count = 0;
    const bool wasActive = PADGetKeyButtonBindings(port, &count) != nullptr;
    PADClearKeyBindings(port); // every key unbound, keyboard off
    bind_defaults(port);
    PADSetKeyboardActive(port, (wasActive || port == kDefaultPort) ? TRUE : FALSE);
    return true;
}

int default_key(unsigned port, bool axis, unsigned short input) {
    if (port != kDefaultPort) {
        return PAD_KEY_INVALID;
    }
    if (axis) {
        for (const PADKeyAxisBinding& a : kAxes) {
            if (a.padAxis == input) {
                return a.scancode;
            }
        }
    } else {
        for (const PADKeyButtonBinding& b : kButtons) {
            if (b.padButton == input) {
                return b.scancode;
            }
        }
    }
    return PAD_KEY_INVALID;
}

} // namespace mppm::launcher
