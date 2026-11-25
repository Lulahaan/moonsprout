// Host half of the game hooks for settings (docs/launcher/SETTINGS.md 5.1 and 6):
//
//   - the two-way sync between the launcher's config and the game's own Options screen for sound
//     mode, music volume, effects volume and rumble ("config.json wins; the card follows").
//
// The game half lives in src/sysGCU/commonSaveData.cpp (p2_game_set_options, the call from
// Mgr::setup) and src/plugProjectEbisawaU/ebiScreenOption.cpp (the call from saveRam). Everything
// here runs on the game thread while no other game thread is active (Mgr::setup and saveRam run
// on a game thread; settings callbacks run from launcher::update), so no locking is needed.

#include "settings.hpp"

#include <p2_host.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mppm {
namespace {

// The game's values: CommonSaveData::Mgr::SoundMode and the card's 0-255 volume bytes.
constexpr int kGameSoundMono   = 0;
constexpr int kGameSoundStereo = 1;
constexpr float kGameVolumeMax = 255.0f;

bool sGameReady = false; // the game's sound system exists (Mgr::setup has run once)
bool sSyncing   = false; // guards against re-entry while a change travels one way
std::vector<borealis::config::Connection> sConnections;

// The launcher's effective values, pushed through the game's own setters.
void push_to_game() {
    if (!sGameReady || sSyncing) {
        return;
    }
    const Settings& s = settings();
    sSyncing          = true;
    p2_game_set_options(s.audio.outputMode.get() == SoundOutput::Mono ? kGameSoundMono : kGameSoundStereo,
        static_cast<float>(s.audio.musicVolume.get()) / 100.0f,
        static_cast<float>(s.audio.effectsVolume.get()) / 100.0f, s.input.rumble.get() ? 1 : 0);
    sSyncing = false;
}

// Rounds like the game's ROUND_F32_TO_U8 for the non-negative values used here.
int round_u8(float v) {
    return static_cast<int>(v + 0.5f);
}

// The 0-255 byte the game stores for a config percentage (Mgr::setBgmVolume).
int game_volume_byte(int percent) {
    return round_u8(static_cast<float>(percent) / 100.0f * kGameVolumeMax);
}

// What the Options screen shows and writes back for a config percentage when the player leaves
// that row alone: it has 11 steps (TOptionParameter::loadRam truncates to 0-10).
int options_screen_byte(int percent) {
    const int step = static_cast<int>(static_cast<float>(game_volume_byte(percent)) / kGameVolumeMax * 10.0f);
    return round_u8(static_cast<float>(step) / 10.0f * kGameVolumeMax);
}

// Takes a volume reported by the Options screen into a var. A value that is only the Options
// screen's coarser rounding of the current setting (e.g. 55% shown as 5 of 10) keeps the setting.
void take_volume(Var<int>& var, int gameByte) {
    if (var.locked() || gameByte == options_screen_byte(var.get())) {
        return;
    }
    const int percent = std::clamp(static_cast<int>(std::lround(gameByte * 100.0 / kGameVolumeMax)), 0, 100);
    if (percent != var.get()) {
        var.set(percent);
    }
}

template <typename T>
void take_value(Var<T>& var, T value) {
    if (!var.locked() && var.get() != value) {
        var.set(value);
    }
}

// Changing one of the four settings in the launcher reaches the game at once (once it exists).
void bind_settings() {
    if (!sConnections.empty()) {
        return;
    }
    Settings& s = settings();
    sConnections.push_back(s.audio.outputMode.on_change([] { push_to_game(); }));
    sConnections.push_back(s.audio.musicVolume.on_change([] { push_to_game(); }));
    sConnections.push_back(s.audio.effectsVolume.on_change([] { push_to_game(); }));
    sConnections.push_back(s.input.rumble.on_change([] { push_to_game(); }));
}

} // namespace

} // namespace mppm

// ---- C bridge (p2_host.h) ----

// Game -> host, from the end of CommonSaveData::Mgr::setup: the options were just read from the
// card (or reset to defaults) and the sound system exists. The launcher's values replace them.
extern "C" void p2_host_apply_game_options(void) {
    mppm::bind_settings();
    mppm::sGameReady = true;
    mppm::push_to_game();
}

// Game -> host, from the end of TOptionParameter::saveRam: the game's Options screen changed an
// option (it calls this for every change it previews, and again when "Don't save changes" puts the
// old values back). The values are the game's own: sound mode 0 mono / 1 stereo / 2 surround,
// volumes 0-255, rumble 0/1.
extern "C" void p2_host_game_options_changed(int soundMode, int musicVol, int seVol, int rumble) {
    using namespace mppm;
    if (sSyncing) {
        return;
    }
    Settings& s = settings();
    sSyncing    = true;
    // Surround is read as stereo: the host mixer has no rear channels.
    take_value(s.audio.outputMode, soundMode == kGameSoundMono ? SoundOutput::Mono : SoundOutput::Stereo);
    take_volume(s.audio.musicVolume, musicVol);
    take_volume(s.audio.effectsVolume, seVol);
    take_value(s.input.rumble, rumble != 0);
    sSyncing = false;
    // Push the effective values back: a locked var (environment or netplay) reverts the game's
    // change, and a setting finer than the Options screen's steps keeps its exact value.
    push_to_game();
}
