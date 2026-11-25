#pragma once

#include <RmlUi/Core/Input.h>

union SDL_Event;

namespace mppm::ui::input {

inline constexpr char kNavAxisEvent[] = "navaxis";

// The pad's Y button (and the keyboard key bound to it, Q by default) arrives as this key: "Reset
// to Default" in the settings window (DESIGN 5.17). A key no keyboard has, so it never collides
// with typing or with a bound key.
inline constexpr Rml::Input::KeyIdentifier kResetKey = Rml::Input::KI_F13;

void handle_event(const SDL_Event& event) noexcept;
void update_input() noexcept;
void reset_input_state() noexcept;
void sync_input_block() noexcept;
void release_input_block() noexcept;
// Whether the last input the player made came from a gamepad (a button, or a stick or trigger
// pushed past the press threshold) rather than the keyboard or mouse. Button prompts follow it
// (DESIGN 5.17), so a pad that is merely connected does not turn "Esc" into "B". Starts false.
bool last_input_gamepad() noexcept;
// Whether the last input was a mouse click or wheel (not a mere pointer move, so a window that
// opens under a resting pointer does not count). Windows hide their button prompts while it holds
// (DESIGN 5.17); any key or pad button brings them back. Starts false.
bool last_input_pointer() noexcept;

}  // namespace mppm::ui
