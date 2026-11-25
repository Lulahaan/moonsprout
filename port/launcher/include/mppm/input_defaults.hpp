#pragma once
// Default keyboard layout (docs/launcher/SETTINGS.md 8, "Keyboard"). Port 1 gets a layout out of the
// box so the game is playable without a controller; the other ports start with no keys.
//
// Bindings themselves are stored by Aurora (keyboard_bindings.dat in the data folder, written by
// PADSerializeMappings), never in config.json.

namespace mppm::launcher {

// Gives `port` its default keys.
//
//   force = false (startup): only when the player has never saved bindings and the port has none
//           active; at most once per port and run. Saved bindings, including a keyboard the player
//           switched off, are kept.
//   force = true ("Restore Defaults"): clears the port's keys (PADClearKeyBindings) and binds the
//           defaults again. Port 1 is switched on; other ports keep their on/off state.
//
// Calls PADInit first (it is idempotent), so the launcher may call this before the game boots.
// Returns whether bindings changed.
bool apply_default_keyboard(unsigned port, bool force);

// The default scancode for a GameCube button (`axis` false, a PAD_BUTTON_* / PAD_TRIGGER_* bit) or
// axis direction (`axis` true, a PAD_AXIS_* index) on `port`; PAD_KEY_INVALID (-1) when it has none.
int default_key(unsigned port, bool axis, unsigned short input);

} // namespace mppm::launcher
