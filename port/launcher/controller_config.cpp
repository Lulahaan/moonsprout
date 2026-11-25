// Controller and keyboard configuration window (docs/launcher/SETTINGS.md 8). Based on Dusklight's
// src/dusk/ui/controller_config.cpp (CC0), without its Twilight Princess action bindings, with
// swap-on-conflict, a timed capture, per-player keyboards alongside controllers and an info column.
#include "controller_config.hpp"

#include "mppm/input_defaults.hpp"
#include "ui/bool_button.hpp"
#include "ui/modal.hpp"
#include "ui/number_button.hpp"
#include "ui/pane.hpp"
#include "ui/select_button.hpp"
#include "ui/sound.hpp"
#include "ui/ui.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_timer.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>

namespace mppm::ui {

// A physical input bound to a GameCube input. Exactly one of the three is set for a bound input;
// none for an unbound one. Keyboard bindings only use `key`; controller button rows only `button`.
struct BindingSource {
    s32 key = PAD_KEY_INVALID;           // SDL scancode, or PAD_KEY_MOUSE_*
    s32 axis = -1;                       // SDL_GamepadAxis
    PADAxisSign sign = AXIS_SIGN_POSITIVE;
    s32 button = -1;                     // SDL_GamepadButton

    bool bound() const { return key != PAD_KEY_INVALID || axis >= 0 || button >= 0; }
    bool operator==(const BindingSource&) const = default;
};

namespace {

using Input  = ControllerConfigWindow::Input;
using Device = ControllerConfigWindow::Device;

constexpr double kCaptureSeconds = 5.0; // press-to-bind gives up after this long
constexpr double kNoticeSeconds  = 4.0; // "Swapped with ..." stays this long in the info column
constexpr double kRumbleSeconds  = 1.0; // length of the rumble test
constexpr double kSaveDelay      = 0.6; // sliders save once they rest this long
constexpr Sint16 kAxisPulled     = 16384; // half travel: same threshold as PADGetNativeAxisPulled

double now_seconds() {
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

// ---- GameCube inputs ---------------------------------------------------------------------------

// One GameCube input as the window presents it: names, what it does in Pikmin 2 (checked against
// the game code: naviState.cpp for the captain, playCamera.cpp for the camera), the glyph drawn in
// its row and the part of the info-column diagram it lights.
struct InputInfo {
    Input input;
    const char* name;      // row label, e.g. "A" or "Up" (under a "Control Stick" heading)
    const char* longName;  // stand-alone name, e.g. "Control Stick Up"
    const char* hint;      // in-game action
    const char* glyph;     // pad-glyph class
    const char* glyphRml;  // glyph content: a letter, or an icon codepoint for .icon-glyph
    bool glyphIcon;
    const char* part;      // diagram part
    const char* direction; // stick lean for direction rows
};

constexpr const char* kArrowUp    = "&#xe5d8;";
constexpr const char* kArrowDown  = "&#xe5db;";
constexpr const char* kArrowLeft  = "&#xe5c4;";
constexpr const char* kArrowRight = "&#xe5c8;";

// clang-format off
constexpr InputInfo kInputs[] = {
    {{false, PAD_BUTTON_A},     "A",     "A",     "Throw, pluck, punch",                 "glyph-a",     "A",     false, "a",          ""},
    {{false, PAD_BUTTON_B},     "B",     "B",     "Whistle",                             "glyph-b",     "B",     false, "b",          ""},
    {{false, PAD_BUTTON_X},     "X",     "X",     "Dismiss your group",                  "glyph-x",     "X",     false, "x",          ""},
    {{false, PAD_BUTTON_Y},     "Y",     "Y",     "Switch captain",                      "glyph-y",     "Y",     false, "y",          ""},
    {{false, PAD_TRIGGER_Z},    "Z",     "Z",     "Change the camera angle",             "glyph-z",     "Z",     false, "z",          ""},
    {{false, PAD_BUTTON_START}, "Start", "Start", "Pause menu",                          "glyph-start", "",      false, "start",      ""},
    {{false, PAD_TRIGGER_L},    "L",     "L",     "Put the camera behind your captain",  "glyph-l",     "L",     false, "l",          ""},
    {{false, PAD_TRIGGER_R},    "R",     "R",     "Zoom the camera",                     "glyph-r",     "R",     false, "r",          ""},
    {{false, PAD_BUTTON_UP},    "Up",    "D-Pad Up",    "Ultra-bitter spray",                  "glyph-dpad", kArrowUp,    true, "dpad-up",    ""},
    {{false, PAD_BUTTON_DOWN},  "Down",  "D-Pad Down",  "Ultra-spicy spray",                   "glyph-dpad", kArrowDown,  true, "dpad-down",  ""},
    {{false, PAD_BUTTON_LEFT},  "Left",  "D-Pad Left",  "Choose which Pikmin to throw",        "glyph-dpad", kArrowLeft,  true, "dpad-left",  ""},
    {{false, PAD_BUTTON_RIGHT}, "Right", "D-Pad Right", "Choose which Pikmin to throw",        "glyph-dpad", kArrowRight, true, "dpad-right", ""},
    {{true, PAD_AXIS_LEFT_Y_POS},  "Up",    "Control Stick Up",    "Move your captain",       "glyph-stick",  kArrowUp,    true, "stick",  "up"},
    {{true, PAD_AXIS_LEFT_Y_NEG},  "Down",  "Control Stick Down",  "Move your captain",       "glyph-stick",  kArrowDown,  true, "stick",  "down"},
    {{true, PAD_AXIS_LEFT_X_NEG},  "Left",  "Control Stick Left",  "Move your captain",       "glyph-stick",  kArrowLeft,  true, "stick",  "left"},
    {{true, PAD_AXIS_LEFT_X_POS},  "Right", "Control Stick Right", "Move your captain",       "glyph-stick",  kArrowRight, true, "stick",  "right"},
    {{true, PAD_AXIS_RIGHT_Y_POS}, "Up",    "C-Stick Up",          "Steer your Pikmin group", "glyph-cstick", kArrowUp,    true, "cstick", "up"},
    {{true, PAD_AXIS_RIGHT_Y_NEG}, "Down",  "C-Stick Down",        "Steer your Pikmin group", "glyph-cstick", kArrowDown,  true, "cstick", "down"},
    {{true, PAD_AXIS_RIGHT_X_NEG}, "Left",  "C-Stick Left",        "Steer your Pikmin group", "glyph-cstick", kArrowLeft,  true, "cstick", "left"},
    {{true, PAD_AXIS_RIGHT_X_POS}, "Right", "C-Stick Right",       "Steer your Pikmin group", "glyph-cstick", kArrowRight, true, "cstick", "right"},
    {{true, PAD_AXIS_TRIGGER_L},   "L Pressure", "L Pressure", "A light press makes the camera follow", "glyph-l", "L", false, "l", ""},
    {{true, PAD_AXIS_TRIGGER_R},   "R Pressure", "R Pressure", "How far R is pressed",                  "glyph-r", "R", false, "r", ""},
};
// clang-format on

const InputInfo& input_info(Input input) {
    for (const InputInfo& info : kInputs) {
        if (info.input == input) {
            return info;
        }
    }
    return kInputs[0];
}

constexpr Input kFaceButtons[] = {{false, PAD_BUTTON_A}, {false, PAD_BUTTON_B}, {false, PAD_BUTTON_X},
    {false, PAD_BUTTON_Y}, {false, PAD_TRIGGER_Z}, {false, PAD_BUTTON_START}};
constexpr Input kShoulders[] = {{false, PAD_TRIGGER_L}, {false, PAD_TRIGGER_R}};
constexpr Input kDpad[] = {{false, PAD_BUTTON_UP}, {false, PAD_BUTTON_DOWN}, {false, PAD_BUTTON_LEFT},
    {false, PAD_BUTTON_RIGHT}};
constexpr Input kControlStick[] = {{true, PAD_AXIS_LEFT_Y_POS}, {true, PAD_AXIS_LEFT_Y_NEG},
    {true, PAD_AXIS_LEFT_X_NEG}, {true, PAD_AXIS_LEFT_X_POS}};
constexpr Input kCStick[] = {{true, PAD_AXIS_RIGHT_Y_POS}, {true, PAD_AXIS_RIGHT_Y_NEG},
    {true, PAD_AXIS_RIGHT_X_NEG}, {true, PAD_AXIS_RIGHT_X_POS}};
constexpr Input kAnalogTriggers[] = {{true, PAD_AXIS_TRIGGER_L}, {true, PAD_AXIS_TRIGGER_R}};

// ---- devices -----------------------------------------------------------------------------------

bool keyboard_active(int port) {
    u32 count = 0;
    return PADGetKeyButtonBindings(static_cast<u32>(port), &count) != nullptr;
}

bool has_controller(int port) {
    return PADGetIndexForPort(static_cast<u32>(port)) >= 0;
}

SDL_Gamepad* gamepad_for_port(int port) {
    const s32 index = PADGetIndexForPort(static_cast<u32>(port));
    return index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
}

Rml::String controller_name(int port) {
    const char* name = PADGetName(static_cast<u32>(port));
    return name != nullptr ? Rml::String(name) : Rml::String("None");
}

Rml::String controller_index_name(u32 index) {
    const char* name = PADGetNameForControllerIndex(index);
    return name != nullptr ? Rml::String(name) : fmt::format("Controller {}", index + 1);
}

// The port a controller (by index) is assigned to, or -1.
int port_of_index(u32 index) {
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        if (PADGetIndexForPort(static_cast<u32>(port)) == static_cast<s32>(index)) {
            return port;
        }
    }
    return -1;
}

// ---- physical input names ----------------------------------------------------------------------

struct SpecificButtonName {
    SDL_GamepadType type;
    const char* name;
};

struct ButtonNames {
    SDL_GamepadButton button;
    std::array<SpecificButtonName, 6> names;
};

// clang-format off
const ButtonNames kGamepadButtonNames[] = {
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, {{
        {SDL_GAMEPAD_TYPE_PS3, "L3"}, {SDL_GAMEPAD_TYPE_PS4, "L3"}, {SDL_GAMEPAD_TYPE_PS5, "L3"},
        {SDL_GAMEPAD_TYPE_XBOX360, "Left Stick"}, {SDL_GAMEPAD_TYPE_XBOXONE, "Left Stick"},
        {SDL_GAMEPAD_TYPE_GAMECUBE, "Control Stick"}}}},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, {{
        {SDL_GAMEPAD_TYPE_PS3, "R3"}, {SDL_GAMEPAD_TYPE_PS4, "R3"}, {SDL_GAMEPAD_TYPE_PS5, "R3"},
        {SDL_GAMEPAD_TYPE_XBOX360, "Right Stick"}, {SDL_GAMEPAD_TYPE_XBOXONE, "Right Stick"},
        {SDL_GAMEPAD_TYPE_GAMECUBE, "C-Stick"}}}},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, {{
        {SDL_GAMEPAD_TYPE_PS3, "L1"}, {SDL_GAMEPAD_TYPE_PS4, "L1"}, {SDL_GAMEPAD_TYPE_PS5, "L1"},
        {SDL_GAMEPAD_TYPE_XBOX360, "LB"}, {SDL_GAMEPAD_TYPE_XBOXONE, "LB"},
        {SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, "L"}}}},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, {{
        {SDL_GAMEPAD_TYPE_PS3, "R1"}, {SDL_GAMEPAD_TYPE_PS4, "R1"}, {SDL_GAMEPAD_TYPE_PS5, "R1"},
        {SDL_GAMEPAD_TYPE_XBOX360, "RB"}, {SDL_GAMEPAD_TYPE_XBOXONE, "RB"},
        {SDL_GAMEPAD_TYPE_GAMECUBE, "Z"}}}},
    {SDL_GAMEPAD_BUTTON_BACK, {{
        {SDL_GAMEPAD_TYPE_PS3, "Select"}, {SDL_GAMEPAD_TYPE_PS4, "Share"}, {SDL_GAMEPAD_TYPE_PS5, "Create"},
        {SDL_GAMEPAD_TYPE_XBOX360, "Back"}, {SDL_GAMEPAD_TYPE_XBOXONE, "View"},
        {SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, "Minus"}}}},
    {SDL_GAMEPAD_BUTTON_START, {{
        {SDL_GAMEPAD_TYPE_PS3, "Start"}, {SDL_GAMEPAD_TYPE_PS4, "Options"}, {SDL_GAMEPAD_TYPE_PS5, "Options"},
        {SDL_GAMEPAD_TYPE_XBOX360, "Start"}, {SDL_GAMEPAD_TYPE_XBOXONE, "Menu"},
        {SDL_GAMEPAD_TYPE_GAMECUBE, "Start/Pause"}}}},
};
// clang-format on

Rml::String native_button_name(SDL_Gamepad* gamepad, s32 buttonIndex) {
    if (buttonIndex < 0) {
        return "Not bound";
    }
    const auto button = static_cast<SDL_GamepadButton>(buttonIndex);
    if (gamepad != nullptr) {
        switch (SDL_GetGamepadButtonLabel(gamepad, button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:
            return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B:
            return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X:
            return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y:
            return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
            return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
            return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
            return "Triangle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
            return "Square";
        default:
            break;
        }
    }
    const SDL_GamepadType type = gamepad != nullptr ? SDL_GetGamepadType(gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    for (const auto& entry : kGamepadButtonNames) {
        if (entry.button != button) {
            continue;
        }
        for (const auto& name : entry.names) {
            if (name.name != nullptr && name.type == type) {
                return name.name;
            }
        }
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return "D-Pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return "D-Pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return "D-Pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return "D-Pad Right";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return "Left shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return "Right shoulder";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return "Left stick click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return "Right stick click";
    default:
        break;
    }
    if (const char* name = PADGetNativeButtonName(static_cast<u32>(buttonIndex)); name != nullptr && name[0] != '\0') {
        Rml::String text = name;
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }
    return "Unknown";
}

bool is_playstation(SDL_GamepadType type) {
    return type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
}

bool is_xbox(SDL_GamepadType type) {
    return type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE;
}

bool is_nintendo(SDL_GamepadType type) {
    return type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
           type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
           type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
}

// "Left Stick up", "C-Stick Left", "LT", "R2"... for an axis direction of the given controller.
Rml::String native_axis_name(SDL_Gamepad* gamepad, s32 axis, PADAxisSign sign) {
    const SDL_GamepadType type = gamepad != nullptr ? SDL_GetGamepadType(gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const bool gamecube = type == SDL_GAMEPAD_TYPE_GAMECUBE;
    const bool positive = sign == AXIS_SIGN_POSITIVE;
    switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return fmt::format("{} {}", gamecube ? "Control Stick" : "Left Stick", positive ? "right" : "left");
    case SDL_GAMEPAD_AXIS_LEFTY: // SDL's y axis points down
        return fmt::format("{} {}", gamecube ? "Control Stick" : "Left Stick", positive ? "down" : "up");
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return fmt::format("{} {}", gamecube ? "C-Stick" : "Right Stick", positive ? "right" : "left");
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return fmt::format("{} {}", gamecube ? "C-Stick" : "Right Stick", positive ? "down" : "up");
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        return is_playstation(type) ? "L2" : is_xbox(type) ? "LT" : is_nintendo(type) ? "ZL" : gamecube ? "L" : "Left trigger";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        return is_playstation(type) ? "R2" : is_xbox(type) ? "RT" : is_nintendo(type) ? "ZR" : gamecube ? "R" : "Right trigger";
    default:
        break;
    }
    if (const char* name = PADGetNativeAxisName({axis, sign}); name != nullptr) {
        return fmt::format("{}{}", name, positive ? "+" : "-");
    }
    return "Unknown";
}

Rml::String key_name(s32 scancode) {
    switch (scancode) {
    case PAD_KEY_INVALID:
        return "Not bound";
    case PAD_KEY_MOUSE_LEFT:
        return "Left click";
    case PAD_KEY_MOUSE_MIDDLE:
        return "Middle click";
    case PAD_KEY_MOUSE_RIGHT:
        return "Right click";
    case PAD_KEY_MOUSE_X1:
        return "Mouse back";
    case PAD_KEY_MOUSE_X2:
        return "Mouse forward";
    case SDL_SCANCODE_RETURN:
        return "Enter";
    default:
        break;
    }
    if (scancode < 0) {
        return "Unknown";
    }
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name != nullptr && name[0] != '\0' ? Rml::String(name) : Rml::String("Unknown");
}

// ---- reading and writing bindings --------------------------------------------------------------

PADButtonMapping* button_mapping(int port, u16 button) {
    u32 count = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<u32>(port), &count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].padButton == button) {
            return &mappings[i];
        }
    }
    return nullptr;
}

PADAxisMapping* axis_mapping(int port, u16 axis) {
    u32 count = 0;
    PADAxisMapping* mappings = PADGetAxisMappings(static_cast<u32>(port), &count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].padAxis == axis) {
            return &mappings[i];
        }
    }
    return nullptr;
}

s32 key_for(int port, Input input) {
    u32 count = 0;
    if (input.axis) {
        const PADKeyAxisBinding* keys = PADGetKeyAxisBindings(static_cast<u32>(port), &count);
        for (u32 i = 0; keys != nullptr && i < count; ++i) {
            if (keys[i].padAxis == input.id) {
                return keys[i].scancode;
            }
        }
    } else {
        const PADKeyButtonBinding* keys = PADGetKeyButtonBindings(static_cast<u32>(port), &count);
        for (u32 i = 0; keys != nullptr && i < count; ++i) {
            if (keys[i].padButton == input.id) {
                return keys[i].scancode;
            }
        }
    }
    return PAD_KEY_INVALID;
}

BindingSource source_of(int port, Device device, Input input) {
    BindingSource source;
    if (device == Device::Keyboard) {
        source.key = key_for(port, input);
    } else if (input.axis) {
        if (const PADAxisMapping* m = axis_mapping(port, input.id)) {
            if (m->nativeAxis.nativeAxis >= 0) {
                source.axis = m->nativeAxis.nativeAxis;
                source.sign = m->nativeAxis.sign;
            } else if (m->nativeButton >= 0) {
                source.button = m->nativeButton;
            }
        }
    } else if (const PADButtonMapping* m = button_mapping(port, input.id)) {
        if (m->nativeButton != PAD_NATIVE_BUTTON_INVALID) {
            source.button = static_cast<s32>(m->nativeButton);
        }
    }
    return source;
}

// Returns false when the row cannot hold that kind of input (a controller button row is offered a
// stick direction); the binding is then left unchanged.
bool set_source(int port, Device device, Input input, const BindingSource& source) {
    if (device == Device::Keyboard) {
        if (input.axis) {
            return PADSetKeyAxisBinding(static_cast<u32>(port), {source.key, input.id, 0}) != FALSE;
        }
        return PADSetKeyButtonBinding(static_cast<u32>(port), {source.key, input.id}) != FALSE;
    }
    if (input.axis) {
        PADAxisMapping* m = axis_mapping(port, input.id);
        if (m == nullptr) {
            return false;
        }
        m->nativeAxis   = {source.axis >= 0 ? source.axis : -1, source.axis >= 0 ? source.sign : AXIS_SIGN_POSITIVE};
        m->nativeButton = source.axis >= 0 ? -1 : source.button;
        return true;
    }
    PADButtonMapping* m = button_mapping(port, input.id);
    if (m == nullptr || source.axis >= 0) {
        return false;
    }
    m->nativeButton = source.button >= 0 ? static_cast<u32>(source.button) : PAD_NATIVE_BUTTON_INVALID;
    return true;
}

bool same_input(const BindingSource& a, const BindingSource& b) {
    return (a.key != PAD_KEY_INVALID && a.key == b.key) || (a.axis >= 0 && a.axis == b.axis && a.sign == b.sign) ||
           (a.button >= 0 && a.button == b.button);
}

// Another row of the same player and device already using `source`.
std::optional<Input> find_conflict(int port, Device device, Input target, const BindingSource& source) {
    for (const InputInfo& info : kInputs) {
        if (info.input != target && same_input(source, source_of(port, device, info.input))) {
            return info.input;
        }
    }
    return std::nullopt;
}

Rml::String binding_label(int port, Device device, Input input) {
    const BindingSource source = source_of(port, device, input);
    if (device == Device::Keyboard) {
        return key_name(source.key);
    }
    SDL_Gamepad* gamepad = gamepad_for_port(port);
    if (source.axis >= 0) {
        return native_axis_name(gamepad, source.axis, source.sign);
    }
    if (source.button >= 0) {
        return native_button_name(gamepad, source.button);
    }
    // Digital L and R are normally pressed by pushing the analog trigger past its press point.
    if (!input.axis && (input.id == PAD_TRIGGER_L || input.id == PAD_TRIGGER_R)) {
        const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
        const BindingSource analog =
            source_of(port, device, {true, static_cast<u16>(input.id == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R)});
        if (zones != nullptr && zones->emulateTriggers && analog.axis >= 0) {
            return fmt::format("{} press", native_axis_name(gamepad, analog.axis, analog.sign));
        }
    }
    return "Not bound";
}

bool binding_unbound(int port, Device device, Input input) {
    return binding_label(port, device, input) == "Not bound";
}

// ---- polling physical inputs -------------------------------------------------------------------

// Keys that keep their launcher meaning and can never be bound: menu, fullscreen, cancel, clear.
bool reserved_key(int scancode) {
    return scancode == SDL_SCANCODE_F1 || scancode == SDL_SCANCODE_F11 || scancode == SDL_SCANCODE_ESCAPE ||
           scancode == SDL_SCANCODE_DELETE || scancode == SDL_SCANCODE_BACKSPACE;
}

// Back opens the menu (ui/input.cpp) and Guide belongs to the system.
bool reserved_button(int button) {
    return button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_GUIDE;
}

bool key_down(SDL_Scancode scancode) {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys != nullptr && scancode < count && keys[scancode];
}

bool keyboard_neutral() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i]) {
            return false;
        }
    }
    float x = 0.0f;
    float y = 0.0f;
    return SDL_GetMouseState(&x, &y) == 0;
}

// The first key held, or a mouse button other than the left one (left clicks drive the UI).
s32 pressed_key() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 1; keys != nullptr && i < count; ++i) {
        if (keys[i] && !reserved_key(i)) {
            return i;
        }
    }
    float x = 0.0f;
    float y = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    for (int button = SDL_BUTTON_MIDDLE; button <= SDL_BUTTON_X2; ++button) {
        if ((buttons & SDL_BUTTON_MASK(button)) != 0) {
            return -(button + 1); // PAD_KEY_MOUSE_MIDDLE (-3) and on
        }
    }
    return PAD_KEY_INVALID;
}

s32 pressed_button(SDL_Gamepad* gamepad) {
    for (int i = 0; gamepad != nullptr && i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
        if (!reserved_button(i) && SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(i))) {
            return i;
        }
    }
    return -1;
}

// The axis direction pushed furthest past half travel. Triggers rest at their minimum, so only
// their positive direction counts.
std::optional<std::pair<s32, PADAxisSign>> pulled_axis(SDL_Gamepad* gamepad) {
    std::optional<std::pair<s32, PADAxisSign>> best;
    int bestMagnitude = kAxisPulled - 1;
    for (int i = 0; gamepad != nullptr && i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
        const int value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(i));
        const bool trigger = i == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || i == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if ((trigger && value < 0) || std::abs(value) <= bestMagnitude) {
            continue;
        }
        bestMagnitude = std::abs(value);
        best = std::make_pair(i, value < 0 ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE);
    }
    return best;
}

bool controller_neutral(int port) {
    SDL_Gamepad* gamepad = gamepad_for_port(port);
    return pressed_button(gamepad) < 0 && !pulled_axis(gamepad);
}

u16 percent_to_raw(int percent) {
    return static_cast<u16>(std::clamp(percent, 0, 100) * 32767 / 100);
}

int raw_to_percent(u16 raw) {
    return static_cast<int>(std::lround(static_cast<double>(raw) * 100.0 / 32767.0));
}

// ---- widgets -----------------------------------------------------------------------------------

// A binding row: [glyph] name / action hint ........ bound input [countdown ring]
class BindingRow final : public SelectButton {
public:
    struct Props {
        const InputInfo* info = nullptr;
        std::function<Rml::String()> value;
        std::function<bool()> unbound;
        std::function<bool()> modified;
        std::function<double()> capture; // remaining share of the countdown while capturing, else < 0
        std::function<Rml::String()> prompt;
    };

    BindingRow(Rml::Element* parent, Props props)
        : SelectButton(parent, {.key = props.info->name}), mProps(std::move(props)) {
        mRoot->SetClass("binding-row", true);
        auto* doc = mRoot->GetOwnerDocument();

        auto glyph = doc->CreateElement("pad-glyph");
        glyph->SetClass(mProps.info->glyph, true);
        glyph->SetClass("icon-glyph", mProps.info->glyphIcon);
        glyph->SetInnerRML(mProps.info->glyphRml);
        mRoot->InsertBefore(std::move(glyph), mKeyElem);

        // key + hint stacked in one column
        Rml::Element* label = mRoot->InsertBefore(doc->CreateElement("binding-label"), mKeyElem);
        label->AppendChild(mRoot->RemoveChild(mKeyElem));
        append_text_element(label, "binding-hint", mProps.info->hint);

        mRing = append(mRoot, "capture-ring");
    }

    void update() override {
        const double remaining = mProps.capture ? mProps.capture() : -1.0;
        const bool capturing = remaining >= 0.0;
        mRoot->SetClass("capturing", capturing);
        set_value_label(capturing ? mProps.prompt() : mProps.value());
        mValueElem->SetClass("unbound", !capturing && mProps.unbound && mProps.unbound());
        set_modified(!capturing && mProps.modified && mProps.modified());
        if (capturing) {
            // A pie that empties clockwise over the capture time.
            const int degrees = static_cast<int>(std::lround(std::clamp(remaining, 0.0, 1.0) * 360.0));
            if (degrees != mRingDegrees) {
                mRingDegrees = degrees;
                mRing->SetProperty("decorator",
                    fmt::format("conic-gradient(#8CE04A 0deg, #8CE04A {0}deg, rgba(255,255,255,16%) {0}deg)", degrees));
            }
        } else {
            mRingDegrees = -1;
        }
        SelectButton::update();
    }

private:
    Props mProps;
    Rml::Element* mRing = nullptr;
    int mRingDegrees = -1;
};

// The controller diagram: an abstract pad drawn with boxes (no artwork), each part named so the
// info column can light it.
constexpr const char* kDiagramRml = R"RML(
<pad-diagram id="pad-diagram">
    <pad-part class="part-z" part="z">Z</pad-part>
    <pad-part class="part-l shoulder" part="l">L</pad-part>
    <pad-part class="part-r shoulder" part="r">R</pad-part>
    <img class="pad-shell" src="res/img/pad.png" />
    <pad-part class="part-stick" part="stick"><pad-knob /></pad-part>
    <pad-part class="part-start" part="start" />
    <pad-part class="part-y" part="y">Y</pad-part>
    <pad-part class="part-x" part="x">X</pad-part>
    <pad-part class="part-b" part="b">B</pad-part>
    <pad-part class="part-a" part="a">A</pad-part>
    <pad-part class="part-dpad dpad-center" part="dpad-center" />
    <pad-part class="part-dpad dpad-up" part="dpad-up" />
    <pad-part class="part-dpad dpad-down" part="dpad-down" />
    <pad-part class="part-dpad dpad-left" part="dpad-left" />
    <pad-part class="part-dpad dpad-right" part="dpad-right" />
    <pad-part class="part-cstick" part="cstick"><pad-knob /></pad-part>
</pad-diagram>
)RML";

constexpr const char* kAllButtonParts = "a b x y z start l r dpad-up dpad-down dpad-left dpad-right";

} // namespace

// ---- window ------------------------------------------------------------------------------------

ControllerConfigWindow::ControllerConfigWindow(int port)
    : Window(Props{.styleSheets = {"res/rml/controller.rcss"}}) {
    // The keyboard defaults and saved bindings must be loaded before anything here reads or saves
    // them; the start menu can open this window before the game's own PADInit.
    launcher::apply_default_keyboard(0, false);
    mRoot->SetClass("controller-config", true);

    // While a binding is being captured, and until the input that finished it is released, keys
    // and gamepad buttons (which arrive as keys) must not navigate. Clicks cancel the capture.
    listen(
        Rml::EventId::Keydown,
        [this](Rml::Event& event) {
            if (mCapture || mSuppressUntilNeutral) {
                event.StopPropagation();
            }
        },
        true);
    const auto cancelOnPointer = [this](Rml::Event& event) {
        if (mCapture) {
            if (event.GetId() == Rml::EventId::Click) {
                cancel_capture();
            }
            event.StopPropagation();
        }
    };
    listen(Rml::EventId::Click, cancelOnPointer, true);
    listen(Rml::EventId::Mouseover, cancelOnPointer, true);

    for (int p = 0; p < PAD_MAX_CONTROLLERS; ++p) {
        add_tab(fmt::format("Player {}", p + 1), [this, p](Rml::Element* content) {
            if (mCapture && mCapture->port != p) {
                cancel_capture();
            }
            if (p != mActivePort) {
                mPage = Page::Device;
            }
            build_port_tab(content, p);
        });
    }
    if (port > 0 && port < PAD_MAX_CONTROLLERS) {
        set_active_tab(port);
    }
}

ControllerConfigWindow::~ControllerConfigWindow() {
    stop_rumble_test();
    if (mSaveAt != 0.0) {
        PADSerializeMappings();
    }
}

void ControllerConfigWindow::hide(bool close) {
    stop_rumble_test();
    if (mCapture) {
        mCapture.reset();
    }
    save_now();
    Window::hide(close);
}

bool ControllerConfigWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (mCapture || mSuppressUntilNeutral) {
        return true;
    }
    // Left and Right inside the panes move between the columns and stop at the outer ones: past
    // them Window would hand the press to the tab bar, so a stray Right on a binding row (nothing
    // focusable to its right) switched to Player 2's page. The tabs stay on Up and on L / R.
    if ((cmd == NavCommand::Left || cmd == NavCommand::Right) && event.GetTargetElement() != nullptr &&
        event.GetTargetElement()->Closest("content") != nullptr) {
        handle_content_nav(event, cmd);
        return true;
    }
    return Window::handle_nav_command(event, cmd);
}

bool ControllerConfigWindow::consume_close_request() {
    if (mCapture) {
        cancel_capture();
        return true;
    }
    return false;
}

void ControllerConfigWindow::update() {
    const double now = now_seconds();
    poll_capture();
    if (mRumblePort >= 0 && now >= mRumbleUntil) {
        stop_rumble_test();
    }
    if (mSaveAt != 0.0 && now >= mSaveAt) {
        save_now();
    }
    // Hot-plug, or a device change made on this page: rebuild so the page list matches.
    if (!mCapture && device_signature(mActivePort) != mSignature) {
        rebuild_tab();
    }
    if (mRefocusRestore && visible()) {
        mRefocusRestore = false;
        if (!mLeftItems.empty()) {
            mLeftItems.back()->focus(); // Restore Defaults is always the last row
        }
    }
    if (mNotice != nullptr && mNoticeUntil != 0.0 && now >= mNoticeUntil) {
        clear_notice();
    }
    Window::update();
}

// Everything the page list and the device page depend on: which controller each player has, the
// keyboard state of this player and how many controllers are connected.
u64 ControllerConfigWindow::device_signature(int port) const {
    u64 signature = PADCount();
    for (int p = 0; p < PAD_MAX_CONTROLLERS; ++p) {
        signature = signature * 31 + static_cast<u64>(PADGetIndexForPort(static_cast<u32>(p)) + 1);
    }
    return signature * 2 + (keyboard_active(port) ? 1 : 0);
}

// Rebuilds the current tab and puts the focus back where it was (same row index in the same pane).
void ControllerConfigWindow::rebuild_tab() {
    auto* context = mDocument->GetContext();
    Rml::Element* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    const auto index_of = [focused](const std::vector<Component*>& items) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i]->contains(focused)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int leftIndex = index_of(mLeftItems);
    const int middleIndex = index_of(mMiddleItems);
    // The left row whose page is open (it re-renders the middle pane when focused).
    int pageRow = 0;
    for (size_t i = 0; i < mLeftPages.size(); ++i) {
        if (mLeftPages[i] == mPage) {
            pageRow = static_cast<int>(i);
            break;
        }
    }
    refresh_active_tab();
    const auto focus_at = [](const std::vector<Component*>& items, int index) {
        for (int i = std::min(index, static_cast<int>(items.size()) - 1); i >= 0; --i) {
            if (items[i]->focus()) {
                return true;
            }
        }
        return false;
    };
    if (leftIndex >= 0) {
        focus_at(mLeftItems, leftIndex);
    } else if (middleIndex >= 0 && pageRow < static_cast<int>(mLeftItems.size())) {
        mLeftItems[pageRow]->focus(); // renders its page into the middle pane
        mLeftPane->set_selected_item(pageRow);
        if (!focus_at(mMiddleItems, middleIndex)) {
            mLeftItems[pageRow]->focus();
        }
    }
}

void ControllerConfigWindow::build_port_tab(Rml::Element* content, int port) {
    stop_rumble_test();
    mActivePort = port;
    mSignature = device_signature(port);
    mLeftItems.clear();
    mLeftPages.clear();
    mMiddleItems.clear();

    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& middle = add_child<Pane>(content, Pane::Type::Uncontrolled);
    left.root()->SetClass("pages", true);
    middle.root()->SetClass("page", true);
    mLeftPane = &left;
    mMiddlePane = &middle;
    build_info_pane(content);

    const auto page_row = [this, &left, &middle, port](Component& row, Page page, Info info) -> Component& {
        mLeftItems.push_back(&row);
        mLeftPages.push_back(page);
        left.register_control(row, middle, [this, port, page, info](Pane& pane) {
            if (page != mNoticePage) {
                clear_notice(); // it was about the page being left
            }
            mPage = page;
            set_info(info);
            render_page(pane, port, page);
        });
        return row;
    };

    if (port >= 2) {
        left.add_text("Pikmin 2 uses two controllers.")->SetClass("port-note", true);
    }

    const bool controller = has_controller(port);
    const bool keyboard = keyboard_active(port);
    page_row(left.add_select_button({
                 .key = "Controller",
                 .getValue = [port] { return controller_name(port); },
             }),
        Page::Device,
        {.title = "Controller",
         .text = fmt::format("Pick the controller Player {} uses. A controller belongs to one player at a time.", port + 1)});
    page_row(left.add_child<BoolButton>(BoolButton::Props{
                 .key = "Keyboard",
                 .getValue = [port] { return keyboard_active(port); },
                 .setValue =
                     [this, port](bool on) {
                         cancel_capture();
                         PADSetKeyboardActive(static_cast<u32>(port), on ? TRUE : FALSE);
                         if (on) {
                             // A keyboard with no keys at all gets the defaults (Player 1 only has some).
                             bool anyKey = false;
                             for (const InputInfo& info : kInputs) {
                                 anyKey = anyKey || key_for(port, info.input) != PAD_KEY_INVALID;
                             }
                             if (!anyKey) {
                                 launcher::apply_default_keyboard(static_cast<unsigned>(port), true);
                             }
                         }
                         save_now();
                     },
             }),
        Page::KeyboardToggle,
        {.title = "Keyboard",
         .text = fmt::format("Let the keyboard play for Player {}. It works alongside a controller.", port + 1)});

    if (controller) {
        left.add_section("Controller");
        page_row(left.add_group_button({.text = "Buttons"}), Page::Buttons,
            {.title = "Buttons", .text = "Choose the controller button for each GameCube button.", .parts = kAllButtonParts});
        page_row(left.add_group_button({.text = "Sticks"}), Page::Sticks,
            {.title = "Sticks", .text = "Choose what moves your captain and what steers your Pikmin.", .parts = "stick cstick"});
        page_row(left.add_group_button({.text = "Triggers"}), Page::Triggers,
            {.title = "Triggers", .text = "The analog L and R triggers, and how far they go before they click.", .parts = "l r"});
        page_row(left.add_group_button({.text = "Dead Zones"}), Page::DeadZones,
            {.title = "Dead Zones", .text = "Ignore small stick movements, so a worn stick doesn't drift.", .parts = "stick cstick"});
        page_row(left.add_group_button({.text = "Rumble"}), Page::Rumble,
            {.title = "Rumble", .text = "How strongly this controller vibrates, with a test."});
    }
    if (keyboard) {
        left.add_section("Keyboard");
        page_row(left.add_group_button({.text = "Keys"}), Page::Keyboard,
            {.title = "Keys", .text = "Choose the key for each GameCube button and stick direction.", .parts = kAllButtonParts});
    }

    auto& restore = left.add_button(Rml::String("Restore Defaults"));
    restore.root()->SetClass("restore-defaults", true);
    restore.on_pressed([this, port] {
        play_sound(kSoundClick);
        push(std::make_unique<Modal>(Modal::Props{
            .title = "Restore default controls?",
            .bodyText = fmt::format(
                "Player {}'s buttons, sticks and keys go back to their defaults. Dead zones and rumble strength stay as they are.",
                port + 1),
            .actions =
                {
                    {.label = "Cancel",
                     .onPressed =
                         [this](Modal& modal) {
                             modal.pop();
                             mRefocusRestore = true;
                         }},
                    {.label = "Restore",
                     .onPressed =
                         [this, port](Modal& modal) {
                             modal.pop();
                             restore_defaults(port);
                             mRefocusRestore = true;
                         }},
                },
            .onDismiss =
                [this](Modal& modal) {
                    modal.pop();
                    mRefocusRestore = true;
                },
        }));
    });
    page_row(restore, Page::Restore,
        {.title = "Restore Defaults",
         .text = fmt::format("Put every binding of Player {} back the way it came, for the controller and the keyboard.", port + 1)});

    // Keep the page that was open before a rebuild, unless its device went away.
    const bool controllerPage = mPage == Page::Buttons || mPage == Page::Sticks || mPage == Page::Triggers ||
                                mPage == Page::DeadZones || mPage == Page::Rumble;
    if ((controllerPage && !controller) || (mPage == Page::Keyboard && !keyboard)) {
        mPage = Page::Device;
    }
    set_info({.title = fmt::format("Player {}", port + 1),
              .text = controller && keyboard ? fmt::format("Playing with {} and the keyboard.", controller_name(port))
                      : controller           ? fmt::format("Playing with {}.", controller_name(port))
                      : keyboard             ? Rml::String("Playing with the keyboard.")
                                             : Rml::String("No controller or keyboard. Pick one on the left.")});
    render_page(middle, port, mPage);
}

void ControllerConfigWindow::build_info_pane(Rml::Element* content) {
    auto& info = add_child<Pane>(content, Pane::Type::Uncontrolled);
    mInfoRoot = info.root();
    mInfoRoot->SetClass("info", true);
    mInfoRoot->SetInnerRML(fmt::format(R"RML(
{}
<info-title id="info-title" />
<info-text id="info-text" />
<info-note id="info-note" />
<info-notice id="info-notice" />
)RML",
        kDiagramRml));
    mDiagram = mInfoRoot->GetElementById("pad-diagram");
    mInfoTitle = mInfoRoot->GetElementById("info-title");
    mInfoText = mInfoRoot->GetElementById("info-text");
    mInfoHint = mInfoRoot->GetElementById("info-note");
    mNotice = mInfoRoot->GetElementById("info-notice");
    // A notice outlives a rebuild of its own player tab (e.g. after Restore Defaults), never a
    // switch to another player: it would describe someone else's controls there.
    if (mNoticeUntil != 0.0 && mNoticePort == mActivePort) {
        set_text_content(mNotice, mNoticeText);
        mNotice->SetClass("shown", true);
    } else {
        mNoticeUntil = 0.0;
    }
    refresh_info();
}

void ControllerConfigWindow::set_info(Info info) {
    mInfo = std::move(info);
    refresh_info();
}

void ControllerConfigWindow::refresh_info() {
    if (mInfoRoot == nullptr) {
        return;
    }
    Info info = mInfo;
    if (mCapture) {
        const InputInfo& in = input_info(mCapture->input);
        info.title = in.longName;
        info.text = in.hint;
        info.parts = in.part;
        info.direction = in.direction;
        info.note = mCapture->device == Device::Keyboard
                        ? "Press a key. Esc cancels; Delete or Backspace clears the key."
                        : fmt::format("Press a button{} on {}. Waiting 5 seconds cancels; Delete clears it.",
                              mCapture->input.axis ? " or move a stick" : "", controller_name(mCapture->port));
    }
    set_text_content(mInfoTitle, info.title);
    set_text_content(mInfoText, info.text);
    set_text_content(mInfoHint, info.note);
    // Light the named parts; the rest dim when something is lit.
    Rml::ElementList parts;
    mDiagram->GetElementsByTagName(parts, "pad-part");
    const Rml::String lit = " " + info.parts + " ";
    for (auto* part : parts) {
        const Rml::String name = part->GetAttribute<Rml::String>("part", "");
        part->SetClass("lit", !info.parts.empty() && lit.find(" " + name + " ") != Rml::String::npos);
        for (const char* dir : {"up", "down", "left", "right"}) {
            part->SetClass(Rml::String("lean-") + dir, info.direction == dir && lit.find(" " + name + " ") != Rml::String::npos);
        }
    }
    mDiagram->SetClass("dimmed", !info.parts.empty());
    mDiagram->SetClass("capturing", mCapture.has_value());
}

void ControllerConfigWindow::show_notice(const Rml::String& text) {
    if (mNotice == nullptr) {
        return;
    }
    mNoticeText = text;
    set_text_content(mNotice, text);
    mNotice->SetClass("shown", true);
    mNoticeUntil = now_seconds() + kNoticeSeconds;
    mNoticePort = mActivePort;
    mNoticePage = mPage;
}

void ControllerConfigWindow::clear_notice() {
    mNoticeUntil = 0.0;
    if (mNotice != nullptr) {
        mNotice->SetClass("shown", false);
    }
}

void ControllerConfigWindow::render_page(Pane& pane, int port, Page page) {
    pane.clear();
    mMiddleItems.clear();
    switch (page) {
    case Page::Device:
        render_device_page(pane, port);
        break;
    case Page::Keyboard:
        if (keyboard_active(port)) {
            render_bindings(pane, port, Device::Keyboard, page);
        } else {
            pane.add_text(fmt::format("The keyboard doesn't play for Player {}. Turn it on to choose keys.", port + 1))
                ->SetClass("page-note", true);
        }
        break;
    case Page::Buttons:
    case Page::Sticks:
    case Page::Triggers:
        render_bindings(pane, port, Device::Controller, page);
        break;
    case Page::DeadZones:
        render_dead_zones(pane, port);
        break;
    case Page::Rumble:
        render_rumble(pane, port);
        break;
    case Page::KeyboardToggle:
        pane.add_text(keyboard_active(port)
                          ? fmt::format("The keyboard plays for Player {}. Choose its keys under Keys, in the list on the left.", port + 1)
                          : fmt::format("The keyboard doesn't play for Player {}.", port + 1))
            ->SetClass("page-note", true);
        break;
    case Page::Restore:
        pane.add_text(port == 0 ? "Buttons, sticks and triggers return to the standard layout for this kind of "
                                  "controller. The keyboard returns to WASD, Space, Shift and the arrow keys."
                                : "Buttons, sticks and triggers return to the standard layout for this kind of "
                                  "controller. Keyboard keys are cleared.")
            ->SetClass("page-note", true);
        break;
    }
}

void ControllerConfigWindow::render_device_page(Pane& pane, int port) {
    pane.add_section(fmt::format("Player {}'s Controller", port + 1));
    auto& none = pane.add_button({
        .text = "",
        .isSelected = [port] { return !has_controller(port); },
    });
    none.root()->SetClass("device-option", true);
    none.root()->SetInnerRML("<device-name>None</device-name>");
    none.on_pressed([this, port] {
        play_sound(kSoundClick);
        cancel_capture();
        stop_rumble_test();
        PADClearPort(static_cast<u32>(port));
    });
    none.listen(Rml::EventId::Focus, [this, port](Rml::Event&) {
        // Says what this player is left with, from the Keyboard toggle beside the list.
        set_info({.title = "No Controller",
                  .text = keyboard_active(port)
                              ? fmt::format("Player {} plays with the keyboard.", port + 1)
                              : fmt::format("Player {} has no controller, and the keyboard is off.", port + 1)});
    });
    mMiddleItems.push_back(&none);

    const u32 count = PADCount();
    for (u32 i = 0; i < count; ++i) {
        auto& button = pane.add_button({
            .text = "",
            .isSelected = [port, i] { return PADGetIndexForPort(static_cast<u32>(port)) == static_cast<s32>(i); },
        });
        button.root()->SetClass("device-option", true);
        // Name, then small tags: battery, adapter, the player using it now.
        Rml::String tags;
        if (SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(i)) {
            int level = -1;
            const SDL_PowerState power = SDL_GetGamepadPowerInfo(gamepad, &level);
            if (power != SDL_POWERSTATE_UNKNOWN && power != SDL_POWERSTATE_ERROR && power != SDL_POWERSTATE_NO_BATTERY) {
                tags += fmt::format("<device-tag><icon>&#x{};</icon>{}</device-tag>", battery_icon(power, level),
                    level >= 0 ? fmt::format("{}%", level) : Rml::String(""));
            }
            if (PADGetControllerTypeForIndex(i) == PAD_TYPE_GAMECUBE) {
                tags += "<device-tag>GameCube adapter</device-tag>";
            }
        }
        const int owner = port_of_index(i);
        if (owner >= 0 && owner != port) {
            tags += fmt::format("<device-tag class=\"owner\">Player {}</device-tag>", owner + 1);
        }
        button.root()->SetInnerRML(fmt::format("<device-name>{}</device-name><device-tags>{}</device-tags>",
            escape(controller_index_name(i)), tags));
        button.on_pressed([this, port, i] {
            play_sound(kSoundClick);
            cancel_capture();
            stop_rumble_test();
            PADSetPortForIndex(i, static_cast<u32>(port));
        });
        button.listen(Rml::EventId::Focus, [this, port, i, owner](Rml::Event&) {
            set_info({.title = controller_index_name(i),
                      .text = owner >= 0 && owner != port
                                  ? fmt::format("Player {} uses it now. Picking it moves it to Player {}.", owner + 1, port + 1)
                                  : fmt::format("Use this controller for Player {}.", port + 1)});
        });
        mMiddleItems.push_back(&button);
    }
    if (count == 0) {
        pane.add_text("No controllers connected. Plug one in and it appears here.")->SetClass("page-note", true);
    }
}

void ControllerConfigWindow::add_binding_row(Pane& pane, int port, Device device, Input input) {
    const InputInfo& info = input_info(input);
    auto& row = pane.add_child<BindingRow>(BindingRow::Props{
        .info = &info,
        .value = [port, device, input] { return binding_label(port, device, input); },
        .unbound = [port, device, input] { return binding_unbound(port, device, input); },
        .modified =
            [port, device, input] {
                if (device != Device::Keyboard) {
                    return false; // controller defaults depend on the controller type; Aurora owns them
                }
                return key_for(port, input) != launcher::default_key(static_cast<unsigned>(port), input.axis, input.id);
            },
        .capture =
            [this, port, device, input] {
                if (!capturing(port, device, input)) {
                    return -1.0;
                }
                return 1.0 - (now_seconds() - mCapture->startedAt) / kCaptureSeconds;
            },
        .prompt =
            [this, device, input] {
                if (mCapture && !mCapture->armed) {
                    return Rml::String("Release to start\xE2\x80\xA6");
                }
                if (device == Device::Keyboard) {
                    return Rml::String("Press a key\xE2\x80\xA6");
                }
                return Rml::String(input.axis ? "Move a stick or press\xE2\x80\xA6" : "Press a button\xE2\x80\xA6");
            },
    });
    row.on_pressed([this, port, device, input] { begin_capture(port, device, input); });
    const auto show = [this, &info, device, port](Rml::Event&) {
        const int def = device == Device::Keyboard
                            ? launcher::default_key(static_cast<unsigned>(port), info.input.axis, info.input.id)
                            : PAD_KEY_INVALID;
        set_info({.title = info.longName,
                  .text = info.hint,
                  .parts = info.part,
                  .direction = info.direction,
                  .note = def != PAD_KEY_INVALID ? fmt::format("Default: {}. Confirm, then press the new input.", key_name(def))
                                                 : Rml::String("Confirm, then press the new input.")});
    };
    row.listen(Rml::EventId::Focus, show);
    row.listen(Rml::EventId::Mouseover, show);
    mMiddleItems.push_back(&row);
}

void ControllerConfigWindow::render_bindings(Pane& pane, int port, Device device, Page page) {
    const auto rows = [&](const char* heading, std::span<const Input> inputs) {
        pane.add_section(heading);
        for (const Input input : inputs) {
            add_binding_row(pane, port, device, input);
        }
    };
    if (device == Device::Controller && !has_controller(port)) {
        pane.add_text("No controller selected.")->SetClass("page-note", true);
        return;
    }
    if (device == Device::Keyboard) {
        rows("Buttons", kFaceButtons);
        rows("L and R", kShoulders);
        rows("Analog L and R", kAnalogTriggers);
        rows("D-Pad", kDpad);
        rows("Control Stick", kControlStick);
        rows("C-Stick", kCStick);
        pane.add_text("F1, F11, Esc, Delete and Backspace keep their own jobs and can't be bound.")
            ->SetClass("page-note", true);
        return;
    }
    switch (page) {
    case Page::Buttons:
        rows("Buttons", kFaceButtons);
        rows("L and R", kShoulders);
        rows("D-Pad", kDpad);
        break;
    case Page::Sticks:
        rows("Control Stick", kControlStick);
        rows("C-Stick", kCStick);
        break;
    case Page::Triggers: {
        rows("Analog", kAnalogTriggers);
        pane.add_section("Click");
        auto& emulate = pane.add_child<BoolButton>(BoolButton::Props{
            .key = "Press L and R from the Triggers",
            .getValue =
                [port] {
                    const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                    return zones != nullptr && zones->emulateTriggers;
                },
            .setValue =
                [this, port](bool on) {
                    if (PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port))) {
                        zones->emulateTriggers = on;
                        mark_dirty();
                    }
                },
            .isDisabled = [port] { return PADGetDeadZones(static_cast<u32>(port)) == nullptr; },
        });
        emulate.listen(Rml::EventId::Focus, [this](Rml::Event&) {
            set_info({.title = "Trigger Clicks",
                      .text = "A GameCube trigger clicks at the end of its travel. Most controllers have no click, so "
                              "pushing the trigger past the press point below presses L or R.",
                      .parts = "l r"});
        });
        mMiddleItems.push_back(&emulate);
        for (const bool left : {true, false}) {
            auto& threshold = pane.add_child<NumberButton>(NumberButton::Props{
                .key = left ? "L Press Point" : "R Press Point",
                .getValue =
                    [port, left] {
                        const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                        return zones == nullptr ? 0
                                                : raw_to_percent(left ? zones->leftTriggerActivationZone
                                                                      : zones->rightTriggerActivationZone);
                    },
                .setValue =
                    [this, port, left](int value) {
                        if (PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port))) {
                            (left ? zones->leftTriggerActivationZone : zones->rightTriggerActivationZone) =
                                percent_to_raw(value);
                            mark_dirty();
                        }
                    },
                .isDisabled =
                    [port] {
                        const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                        return zones == nullptr || !zones->emulateTriggers;
                    },
                .min = 5,
                .max = 100,
                .step = 5,
                .suffix = "%",
            });
            threshold.listen(Rml::EventId::Focus, [this, left](Rml::Event&) {
                set_info({.title = left ? "L Press Point" : "R Press Point",
                          .text = "How far the trigger travels before it counts as a click.",
                          .parts = left ? "l" : "r"});
            });
            mMiddleItems.push_back(&threshold);
        }
        break;
    }
    default:
        break;
    }
}

void ControllerConfigWindow::render_dead_zones(Pane& pane, int port) {
    if (PADGetDeadZones(static_cast<u32>(port)) == nullptr) {
        pane.add_text("No controller selected.")->SetClass("page-note", true);
        return;
    }
    auto& use = pane.add_child<BoolButton>(BoolButton::Props{
        .key = "Use Dead Zones",
        .getValue =
            [port] {
                const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                return zones != nullptr && zones->useDeadzones;
            },
        .setValue =
            [this, port](bool on) {
                if (PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port))) {
                    zones->useDeadzones = on;
                    mark_dirty();
                }
            },
    });
    use.listen(Rml::EventId::Focus, [this](Rml::Event&) {
        set_info({.title = "Dead Zones",
                  .text = "Ignore stick movement close to the centre. Turn off only for a perfectly centred stick.",
                  .parts = "stick cstick"});
    });
    mMiddleItems.push_back(&use);
    for (const bool main : {true, false}) {
        auto& zone = pane.add_child<NumberButton>(NumberButton::Props{
            .key = main ? "Control Stick" : "C-Stick",
            .getValue =
                [port, main] {
                    const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                    return zones == nullptr ? 0 : raw_to_percent(main ? zones->stickDeadZone : zones->substickDeadZone);
                },
            .setValue =
                [this, port, main](int value) {
                    if (PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port))) {
                        (main ? zones->stickDeadZone : zones->substickDeadZone) = percent_to_raw(value);
                        mark_dirty();
                    }
                },
            .isDisabled =
                [port] {
                    const PADDeadZones* zones = PADGetDeadZones(static_cast<u32>(port));
                    return zones == nullptr || !zones->useDeadzones;
                },
            .min = 0,
            .max = 100,
            .step = 1,
            .suffix = "%",
        });
        zone.listen(Rml::EventId::Focus, [this, main](Rml::Event&) {
            set_info({.title = main ? "Control Stick Dead Zone" : "C-Stick Dead Zone",
                      .text = main ? "Raise it if your captain walks on his own; lower it if small moves are ignored."
                                   : "Raise it if your Pikmin drift on their own.",
                      .parts = main ? "stick" : "cstick"});
        });
        mMiddleItems.push_back(&zone);
    }
}

void ControllerConfigWindow::render_rumble(Pane& pane, int port) {
    const u32 p = static_cast<u32>(port);
    auto& test = pane.add_select_button({
        .key = "Test Rumble",
        .getValue = [this, port] { return mRumblePort == port ? Rml::String("Rumbling\xE2\x80\xA6") : Rml::String("Start"); },
    });
    test.on_pressed([this, port] {
        play_sound(kSoundItemChange);
        start_rumble_test(port);
    });
    test.listen(Rml::EventId::Focus, [this](Rml::Event&) {
        set_info({.title = "Test Rumble", .text = "Vibrates the controller for a second at the strengths below."});
    });
    mMiddleItems.push_back(&test);

    if (PADSupportsRumbleIntensity(p)) {
        for (const bool low : {true, false}) {
            auto& strength = pane.add_child<NumberButton>(NumberButton::Props{
                .key = low ? "Low-Frequency Strength" : "High-Frequency Strength",
                .getValue =
                    [p, low] {
                        u16 lo = 0;
                        u16 hi = 0;
                        PADGetRumbleIntensity(p, &lo, &hi);
                        return raw_to_percent(low ? lo : hi);
                    },
                .setValue =
                    [this, p, low](int value) {
                        u16 lo = 0;
                        u16 hi = 0;
                        PADGetRumbleIntensity(p, &lo, &hi);
                        (low ? lo : hi) = percent_to_raw(value);
                        PADSetRumbleIntensity(p, lo, hi);
                        mark_dirty();
                    },
                .min = 0,
                .max = 100,
                .step = 5,
                .suffix = "%",
            });
            strength.listen(Rml::EventId::Focus, [this, low](Rml::Event&) {
                set_info({.title = low ? "Low-Frequency Strength" : "High-Frequency Strength",
                          .text = low ? "The heavy motor: deep rumbles." : "The light motor: buzzes and taps."});
            });
            mMiddleItems.push_back(&strength);
        }
    } else if (PADIsGCAdapter(p)) {
        pane.add_text("GameCube controllers rumble at one fixed strength.")->SetClass("page-note", true);
    } else {
        pane.add_text("This controller reports no adjustable rumble.")->SetClass("page-note", true);
    }
    if (PADCanForceDeviceRumble(p)) {
        auto& device = pane.add_child<BoolButton>(BoolButton::Props{
            .key = "Use the Device's Own Rumble",
            .getValue = [p] { return PADGetForceDeviceRumble(p) != FALSE; },
            .setValue =
                [this, p](bool on) {
                    PADSetForceDeviceRumble(p, on ? TRUE : FALSE);
                    mark_dirty();
                },
        });
        device.listen(Rml::EventId::Focus, [this](Rml::Event&) {
            set_info({.title = "Device Rumble",
                      .text = "Vibrate the device this controller is built into (a handheld PC, for example) "
                              "instead of the controller."});
        });
        mMiddleItems.push_back(&device);
    }
    pane.add_text("Rumble in the game itself is switched in Settings, under Input.")->SetClass("page-note", true);
}

// ---- press-to-bind -----------------------------------------------------------------------------

bool ControllerConfigWindow::capturing(int port, Device device, Input input) const {
    return mCapture && mCapture->port == port && mCapture->device == device && mCapture->input == input;
}

void ControllerConfigWindow::begin_capture(int port, Device device, Input input) {
    if (capturing(port, device, input)) {
        return;
    }
    play_sound(kSoundClick);
    mCapture = Capture{.port = port, .device = device, .input = input, .startedAt = now_seconds()};
    refresh_info();
}

void ControllerConfigWindow::poll_capture() {
    if (mSuppressUntilNeutral && keyboard_neutral() && (mSuppressPort < 0 || controller_neutral(mSuppressPort))) {
        mSuppressUntilNeutral = false;
        mSuppressPort = -1;
    }
    if (!mCapture) {
        return;
    }
    Capture& capture = *mCapture;
    const bool expired = now_seconds() - capture.startedAt >= kCaptureSeconds;
    // The device the capture listens to must still be there.
    if (expired || (capture.device == Device::Controller && !has_controller(capture.port)) ||
        (capture.device == Device::Keyboard && !keyboard_active(capture.port)))
    {
        cancel_capture();
        return;
    }
    if (!capture.armed) {
        // Wait for the confirm press (key, button or click) that opened the capture to end.
        if (keyboard_neutral() && (capture.device == Device::Keyboard || controller_neutral(capture.port))) {
            capture.armed = true;
            refresh_info();
        }
        return;
    }
    if (key_down(SDL_SCANCODE_ESCAPE)) {
        cancel_capture();
        return;
    }
    if (key_down(SDL_SCANCODE_DELETE) || key_down(SDL_SCANCODE_BACKSPACE)) {
        clear_capture_binding();
        return;
    }
    if (capture.device == Device::Keyboard) {
        if (const s32 key = pressed_key(); key != PAD_KEY_INVALID) {
            commit_capture(BindingSource{.key = key});
        }
        return;
    }
    SDL_Gamepad* gamepad = gamepad_for_port(capture.port);
    if (capture.input.axis) {
        if (const auto axis = pulled_axis(gamepad)) {
            commit_capture(BindingSource{.axis = axis->first, .sign = axis->second});
            return;
        }
    }
    if (const s32 button = pressed_button(gamepad); button >= 0) {
        commit_capture(BindingSource{.button = button});
    }
}

void ControllerConfigWindow::commit_capture(const BindingSource& source) {
    const Capture capture = *mCapture;
    const BindingSource old = source_of(capture.port, capture.device, capture.input);
    if (source == old) {
        end_capture(false);
        return;
    }
    const std::optional<Input> conflict = find_conflict(capture.port, capture.device, capture.input, source);
    set_source(capture.port, capture.device, capture.input, source);
    if (conflict) {
        // The other row takes this row's old input, so nothing is lost; if it can't hold it (or
        // there was none), it ends up unbound and says so.
        const bool swapped = old.bound() && set_source(capture.port, capture.device, *conflict, old);
        if (!swapped) {
            set_source(capture.port, capture.device, *conflict, BindingSource{});
        }
        const InputInfo& other = input_info(*conflict);
        const Rml::String message = swapped ? fmt::format("Swapped with {} ({}).", other.longName, other.hint)
                                            : fmt::format("{} is now unbound.", other.longName);
        // Said once, in the info column beside the row (no toast on top of it).
        show_notice(message);
    }
    end_capture(true);
}

void ControllerConfigWindow::clear_capture_binding() {
    const Capture capture = *mCapture;
    const bool changed = source_of(capture.port, capture.device, capture.input).bound();
    set_source(capture.port, capture.device, capture.input, BindingSource{});
    end_capture(changed);
}

void ControllerConfigWindow::end_capture(bool changed) {
    const int port = mCapture ? mCapture->port : -1;
    const bool keyboard = mCapture && mCapture->device == Device::Keyboard;
    mCapture.reset();
    // The input that finished the capture is still held: keep it away from navigation.
    mSuppressUntilNeutral = true;
    mSuppressPort = keyboard ? -1 : port;
    if (changed) {
        save_now();
        play_sound(kSoundBindingChanged);
    }
    refresh_info();
}

void ControllerConfigWindow::cancel_capture() {
    if (!mCapture) {
        return;
    }
    end_capture(false);
    play_sound(kSoundMenuClose);
}

// ---- actions -----------------------------------------------------------------------------------

void ControllerConfigWindow::restore_defaults(int port) {
    cancel_capture();
    PADRestoreDefaultMapping(static_cast<u32>(port));
    if (keyboard_active(port) || port == 0) {
        launcher::apply_default_keyboard(static_cast<unsigned>(port), true);
    }
    save_now();
    play_sound(kSoundBindingChanged);
    show_notice(fmt::format("Player {}'s controls are back to their defaults.", port + 1));
}

void ControllerConfigWindow::start_rumble_test(int port) {
    stop_rumble_test();
    PADControlMotor(static_cast<u32>(port), PAD_MOTOR_RUMBLE);
    mRumblePort = port;
    mRumbleUntil = now_seconds() + kRumbleSeconds;
    if (mDiagram != nullptr) {
        mDiagram->SetClass("rumbling", true);
    }
}

void ControllerConfigWindow::stop_rumble_test() {
    if (mRumblePort < 0) {
        return;
    }
    PADControlMotor(static_cast<u32>(mRumblePort), PAD_MOTOR_STOP_HARD);
    mRumblePort = -1;
    if (mDiagram != nullptr) {
        mDiagram->SetClass("rumbling", false);
    }
}

void ControllerConfigWindow::mark_dirty() {
    mSaveAt = now_seconds() + kSaveDelay;
}

void ControllerConfigWindow::save_now() {
    mSaveAt = 0.0;
    PADSerializeMappings(); // every controller's file plus keyboard_bindings.dat
}

void open_controller_config(int port) {
    pop_to_or_push<ControllerConfigWindow>([port](ControllerConfigWindow& window) { window.set_active_tab(port); }, port);
}

} // namespace mppm::ui
