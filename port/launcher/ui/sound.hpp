#pragma once
// UI sound cues. The launcher can't assume game audio exists (no disc yet), so cues go through a
// handler the host installs once the game's sound system is up.
#include <cstdint>

namespace mppm::ui {

enum UiSound : std::uint32_t {
    kSoundNone,
    kSoundClick,          // button pressed
    kSoundPlay,           // "Play" pressed
    kSoundBindingChanged, // input binding changed
    kSoundMenuOpen,       // menu bar / window shown
    kSoundMenuClose,
    kSoundWindowOpen,
    kSoundWindowClose,
    kSoundTabChanged,
    kSoundItemFocus,
    kSoundItemChange,
    kSoundItemEnable,
    kSoundItemDisable,
    kSoundWarning,
};

using SoundHandler = void (*)(UiSound sound);
void set_sound_handler(SoundHandler handler) noexcept;
void play_sound(UiSound sound) noexcept;

} // namespace mppm::ui
