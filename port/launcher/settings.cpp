#include "settings.hpp"

#include <borealis/config_codec.hpp>

#include <p2_host.h>

#include <dolphin/card.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

// The default lobby server (D26): a CMake cache string passed to mppm_launcher; empty until the
// official server is deployed, so the UI says "No lobby server is set" instead of contacting one.
#ifndef MPPM_DEFAULT_SERVER_URL
#define MPPM_DEFAULT_SERVER_URL ""
#endif

namespace mppm {
namespace {

using borealis::config::Overlay;
using borealis::config::VarOptions;

// ---- Sanitizers: keep stored values on the steps the UI offers ----

// Menu Size: 0 = automatic, else 75-200 in steps of 5 (DESIGN 9.3).
int sanitize_ui_scale(int v) {
    if (v <= 0) {
        return 0;
    }
    v = std::clamp(v, 75, 200);
    return (v + 2) / 5 * 5;
}

// Window size: 0 = Aurora's default, else 320-16384.
int sanitize_window_dim(int v) {
    return v <= 0 ? 0 : std::clamp(v, 320, 16384);
}

// The largest power of two <= v, within [1, max].
int floor_pow2(int v, int max) {
    int p = 1;
    while (p * 2 <= v && p * 2 <= max) {
        p *= 2;
    }
    return p;
}
int sanitize_anisotropy(int v) { return floor_pow2(v, 16); }
// Off or 4x only (live_options_design H1): 2x and 8x are not supported by every adapter for the
// frame buffer format, and a WebGPU error is fatal once Aurora runs. An old 2 is Off, an 8 is 4x.
int sanitize_msaa(int v) { return v >= 4 ? 4 : 1; }

// Auto and Short were removed (README 6): an old config.json that stored them gets the full take-off.
LaunchAnimation sanitize_launch_animation(LaunchAnimation v) {
    return v == LaunchAnimation::Off ? LaunchAnimation::Off : LaunchAnimation::Full;
}

// Environment flag rule (SETTINGS 1.3): unset or empty = no override, "0" = false, else true.
const char* env_value(const char* name) {
    const char* v = name != nullptr ? std::getenv(name) : nullptr;
    return v != nullptr && v[0] != '\0' ? v : nullptr;
}

template <typename T>
void set_overlay_from_text(Overlay& overlay, VarBase& base, std::string_view text) {
    auto* var = base.as<T>();
    if (var == nullptr) {
        throw borealis::config::InvalidValueError("type mismatch");
    }
    overlay.set(*var, borealis::config::Codec<T>::parse(text));
}

template <typename T>
void set_overlay_default(Overlay& overlay, VarBase& base) {
    auto* var = base.as<T>();
    if (var == nullptr) {
        throw borealis::config::InvalidValueError("type mismatch");
    }
    overlay.set(*var, var->default_value());
}

template <typename T>
void check_text(VarBase& base, std::string_view text) {
    if (base.as<T>() == nullptr) {
        throw borealis::config::InvalidValueError("type mismatch");
    }
    (void)borealis::config::Codec<T>::parse(text);
}

template <typename T>
void set_user_from_text(VarBase& base, std::string_view text) {
    auto* var = base.as<T>();
    if (var == nullptr) {
        throw borealis::config::InvalidValueError("type mismatch");
    }
    var->set(borealis::config::Codec<T>::parse(text));
}

struct MetaOptions {
    Apply apply   = Apply::Runtime;
    Net net       = Net::Local;
    const char* env = nullptr;
    EnvFormat envFormat = EnvFormat::Value;
    const char* netValue = nullptr;
};

template <typename T>
Meta meta(Var<T>& var, MetaOptions o = {}) {
    if (o.env != nullptr && o.envFormat == EnvFormat::Value && std::is_same_v<T, bool>) {
        o.envFormat = EnvFormat::Flag; // every boolean MPPM_* switch uses the flag rule
    }
    return Meta{&var, o.apply, o.net, o.env, o.netValue, o.envFormat, &set_overlay_from_text<T>, &set_user_from_text<T>,
        &set_overlay_default<T>, &check_text<T>};
}

std::vector<Meta> build_meta() {
    auto& s = settings();
    constexpr auto RT = Apply::Runtime;
    constexpr auto RS = Apply::Restart;
    constexpr auto NL = Apply::NextLaunch;
    constexpr auto S  = Net::Synced;
    constexpr auto X  = Net::Forced;
    (void)RT;
    return {
        // PRELAUNCH. discPath is RS only once a game is booted (settings_restart_pending).
        meta(s.backend.discPath, {.apply = RS}),
        meta(s.backend.discVerification),
        meta(s.backend.discFingerprint),
        meta(s.backend.graphicsBackend, {.apply = RS}),
        meta(s.backend.cardFileType, {.apply = RS}),
        meta(s.backend.skipLauncher, {.apply = NL, .env = "MPPM_SKIP_LAUNCHER"}),
        // VIDEO. The aspect is L: the simulation frustum uses a session constant (p2_net_sim_k),
        // whatever the window's shape (M5.1b).
        meta(s.video.fullscreen),
        meta(s.video.aspectRatio),
        meta(s.video.vsync, {.env = "MPPM_VSYNC"}),
        meta(s.video.frameRate, {.env = "MPPM_FRAME_RATE"}),
        meta(s.video.windowWidth, {.apply = NL, .env = "MPPM_WINDOW", .envFormat = EnvFormat::WindowW}),
        meta(s.video.windowHeight, {.apply = NL, .env = "MPPM_WINDOW", .envFormat = EnvFormat::WindowH}),
        meta(s.video.internalResolution),
        meta(s.video.resampler),
        meta(s.video.anisotropy), // Aurora takes it at runtime (aurora_set_texture_anisotropy)
        meta(s.video.fpsCounter),
        meta(s.video.allowCpuAdapter, {.apply = RS, .env = "MPPM_ALLOW_CPU_ADAPTER"}),
        meta(s.video.msaa, {.apply = RS}),
        // Off online until the purity check of fps_design 7.5 passes; then L.
        meta(s.video.smoothMotion, {.net = X, .env = "MPPM_SMOOTH_MOTION", .netValue = "false"}),
        // INPUT
        meta(s.input.rumble),
        meta(s.input.playerLed),
        meta(s.input.backgroundInput),
        // AUDIO
        meta(s.audio.masterVolume),
        meta(s.audio.musicVolume),
        meta(s.audio.effectsVolume),
        meta(s.audio.outputMode),
        meta(s.audio.muteInBackground),
        meta(s.audio.menuSounds),
        meta(s.audio.enabled, {.apply = RS, .env = "MPPM_AUDIO"}),
        meta(s.audio.outputDevice, {.apply = RS}),
        // GAMEPLAY
        meta(s.game.pauseInMenu, {.net = X, .netValue = "false"}),
        meta(s.game.pauseInBackground, {.net = X, .netValue = "false"}),
        meta(s.game.skipBootLogos, {.apply = NL}),
        meta(s.game.idleDemo),
        meta(s.game.titleSeason),
        meta(s.game.fastForward, {.net = X, .netValue = "false"}),
        // CHEATS: all part of the simulation.
        meta(s.cheat.unlockChallenge, {.net = S, .env = "MPPM_UNLOCK_CHALLENGE"}),
        meta(s.cheat.daySpeed, {.net = S, .env = "MPPM_TIME_SCALE", .envFormat = EnvFormat::Percent}),
        meta(s.cheat.sunsetOnDayOne, {.net = S, .env = "MPPM_ALLOW_SUNSET"}),
        meta(s.cheat.invincibleCaptains, {.net = S}),
        meta(s.cheat.infiniteSprays, {.net = S}),
        meta(s.cheat.pikminLimit, {.net = S}),
        // INTERFACE
        meta(s.interface.uiScale),
        meta(s.interface.reduceMotion),
        meta(s.interface.highContrast),
        meta(s.interface.glassEffects),
        meta(s.interface.liveBackground, {.apply = NL}),
        meta(s.interface.sceneArea, {.apply = NL, .env = "MPPM_SCENE_AREA"}),
        meta(s.interface.sceneTime, {.apply = NL, .env = "MPPM_SCENE_TIME"}),
        meta(s.interface.sceneMusic, {.apply = NL}),
        meta(s.interface.launchAnimation),
        meta(s.interface.checkForUpdates),
        meta(s.interface.controllerToasts),
        meta(s.interface.menuHint),
        meta(s.interface.shaderIndicator),
        meta(s.interface.hideHud),
        meta(s.interface.hudLayout),
        meta(s.interface.hudMaxAspect),
        meta(s.interface.advancedSettings),
        meta(s.interface.presetChosen),
        meta(s.interface.lastSeenVersion),
        // TOOLS
        meta(s.tools.perfOverlay),
        meta(s.tools.logLevel),
        meta(s.tools.panicScreen, {.env = "MPPM_PANIC_SCREEN"}),
        meta(s.tools.logFps, {.env = "MPPM_FPS"}),
        meta(s.tools.schedDebug, {.env = "MPPM_SCHED_DEBUG"}),
        meta(s.tools.audioDebug, {.env = "MPPM_AUDIO_DEBUG"}),
        meta(s.tools.audioVoiceLog, {.env = "MPPM_AUDIO_VLOG"}),
        meta(s.tools.audioDumpPath, {.apply = RS, .env = "MPPM_AUDIO_DUMP"}),
        // ONLINE: local to each player (the host's Synced values travel in the session, not these).
        meta(s.online.serverUrl, {.env = "MPPM_SERVER_URL"}),
        meta(s.online.playerName),
    };
}

// Turns an environment value into the var's --cvar syntax. Returns false when it sets nothing.
bool env_to_text(const Meta& m, const char* raw, std::string& out) {
    switch (m.envFormat) {
    case EnvFormat::Value:
        out = raw;
        return true;
    case EnvFormat::Flag:
        out = (raw[0] == '0' && raw[1] == '\0') ? "false" : "true";
        return true;
    case EnvFormat::Percent: {
        char* end = nullptr;
        const double factor = std::strtod(raw, &end);
        if (end == raw || !std::isfinite(factor) || factor < 0.0) {
            return false;
        }
        out = std::to_string(std::lround(factor * 100.0));
        return true;
    }
    case EnvFormat::WindowW:
    case EnvFormat::WindowH: {
        // "WxH" (also "W,H" or "W*H").
        unsigned w = 0, h = 0;
        char sep = 0;
        if (std::sscanf(raw, "%u%c%u", &w, &sep, &h) != 3 || (sep != 'x' && sep != 'X' && sep != ',' && sep != '*')) {
            return false;
        }
        out = std::to_string(m.envFormat == EnvFormat::WindowW ? w : h);
        return true;
    }
    }
    return false;
}

std::vector<std::pair<VarBase*, nlohmann::json>>& boot_snapshot() {
    static std::vector<std::pair<VarBase*, nlohmann::json>> sSnapshot;
    return sSnapshot;
}

// Keeps observer connections for the process lifetime.
std::vector<borealis::config::Connection>& connections() {
    static std::vector<borealis::config::Connection> sConnections;
    return sConnections;
}

} // namespace

Settings::Settings()
    : backend{
          .discPath{"backend.discPath", ""},
          .discVerification{"backend.discVerification", DiscVerification::Unknown},
          .discFingerprint{"backend.discFingerprint", ""},
          .graphicsBackend{"backend.graphicsBackend", "auto"},
          .cardFileType{"backend.cardFileType", CardFileType::GciFolder},
          .skipLauncher{"backend.skipLauncher", false},
      }
    , video{
          .fullscreen{"video.fullscreen", false},
          // Auto since ASPECT phase 1; the first-run preset picks Auto (Modern) or Original.
          .aspectRatio{"video.aspectRatio", AspectRatio::Auto},
          .vsync{"video.vsync", true},
          .frameRate{"video.frameRate", "original", {.sanitize = sanitize_frame_rate}},
          .windowWidth{"video.windowWidth", 0, {.min = 0, .max = 16384, .sanitize = sanitize_window_dim}},
          .windowHeight{"video.windowHeight", 0, {.min = 0, .max = 16384, .sanitize = sanitize_window_dim}},
          .internalResolution{"video.internalResolution", 0, {.min = 0, .max = 8}},
          .resampler{"video.resampler", Resampler::Bilinear},
          .anisotropy{"video.anisotropy", 1, {.min = 1, .max = 16, .sanitize = sanitize_anisotropy}},
          .fpsCounter{"video.fpsCounter", FpsCounter::Off},
          .allowCpuAdapter{"video.allowCpuAdapter", false},
          .msaa{"video.msaa", 1, {.min = 1, .max = 8, .sanitize = sanitize_msaa}}, // 1 or 4
          .smoothMotion{"video.smoothMotion", false, {.aliases = {"video.gameplay60"}}},
      }
    , input{
          .rumble{"input.rumble", true},
          .playerLed{"input.playerLed", true},
          .backgroundInput{"input.backgroundInput", true},
      }
    , audio{
          .masterVolume{"audio.masterVolume", 80, {.min = 0, .max = 100}},
          .musicVolume{"audio.musicVolume", 100, {.min = 0, .max = 100}},
          .effectsVolume{"audio.effectsVolume", 100, {.min = 0, .max = 100}},
          .outputMode{"audio.outputMode", SoundOutput::Stereo},
          .muteInBackground{"audio.muteInBackground", false},
          .menuSounds{"audio.menuSounds", true},
          .enabled{"audio.enabled", true},
          .outputDevice{"audio.outputDevice", ""},
      }
    , game{
          .pauseInMenu{"game.pauseInMenu", true},
          .pauseInBackground{"game.pauseInBackground", false},
          .skipBootLogos{"game.skipBootLogos", true},
          .idleDemo{"game.idleDemo", true},
          .titleSeason{"game.titleSeason", TitleSeason::Original},
          .fastForward{"game.fastForward", false},
      }
    , cheat{
          .unlockChallenge{"cheat.unlockChallenge", false},
          // The file range reaches 10000 so test scripts can still use MPPM_TIME_SCALE=50.
          .daySpeed{"cheat.daySpeed", 100, {.min = 0, .max = 10000}},
          .sunsetOnDayOne{"cheat.sunsetOnDayOne", false},
          .invincibleCaptains{"cheat.invincibleCaptains", false},
          .infiniteSprays{"cheat.infiniteSprays", false},
          .pikminLimit{"cheat.pikminLimit", 100, {.min = 100, .max = 200}},
      }
    , interface{
          .uiScale{"interface.uiScale", 100, {.min = 0, .max = 200, .sanitize = sanitize_ui_scale}},
          .reduceMotion{"interface.reduceMotion", false},
          .highContrast{"interface.highContrast", false},
          .glassEffects{"interface.glassEffects", GlassEffects::Blur},
          .liveBackground{"interface.liveBackground", true},
          .sceneArea{"interface.sceneArea", SceneArea::Forest},
          .sceneTime{"interface.sceneTime", SceneTime::Morning},
          .sceneMusic{"interface.sceneMusic", SceneMusic::Piklopedia},
          .launchAnimation{"interface.launchAnimation", LaunchAnimation::Full, {.sanitize = sanitize_launch_animation}},
          .checkForUpdates{"interface.checkForUpdates", true},
          .controllerToasts{"interface.controllerToasts", true},
          .menuHint{"interface.menuHint", true},
          .shaderIndicator{"interface.shaderIndicator", true},
          .hideHud{"interface.hideHud", false},
          // Becomes Edges when ASPECT phase 2 ships (package W8b).
          .hudLayout{"interface.hudLayout", HudLayout::Edges},
          .hudMaxAspect{"interface.hudMaxAspect", HudMaxAspect::Ratio16x9},
          .advancedSettings{"interface.advancedSettings", false},
          .presetChosen{"interface.presetChosen", false},
          .lastSeenVersion{"interface.lastSeenVersion", ""},
      }
    , tools{
          .perfOverlay{"tools.perfOverlay", false},
          .logLevel{"tools.logLevel", LogLevel::Info},
          .panicScreen{"tools.panicScreen", false},
          .logFps{"tools.logFps", false},
          .schedDebug{"tools.schedDebug", false},
          .audioDebug{"tools.audioDebug", false},
          .audioVoiceLog{"tools.audioVoiceLog", false},
          .audioDumpPath{"tools.audioDumpPath", "", {.persist = false}},
      }
    , online{
          // online.server_url is the plan's spelling (ONLINE_MULTIPLAYER.md 2.11); keys are camelCase.
          .serverUrl{"online.serverUrl", MPPM_DEFAULT_SERVER_URL,
              {.sanitize = sanitize_server_url, .aliases = {"online.server_url"}}},
          .playerName{"online.playerName", "", {.sanitize = sanitize_player_name}},
      } {}

Settings& settings() {
    static Settings sSettings;
    return sSettings;
}

std::span<const Meta> settings_meta() {
    static const std::vector<Meta> sMeta = build_meta();
    return sMeta;
}

MenuApply menu_apply(const Meta& m) {
    auto& s = settings();
    // Overrides of the Apply default (live_options_design 3.5).
    const VarBase* const kLive[] = {&s.video.msaa, &s.interface.liveBackground, &s.interface.sceneArea,
        &s.interface.sceneTime, &s.interface.sceneMusic};
    const VarBase* const kAtBoot[] = {&s.backend.discPath, &s.backend.cardFileType, &s.audio.enabled,
        &s.audio.outputDevice, &s.tools.audioDumpPath};
    if (std::ranges::find(kLive, m.var) != std::end(kLive)) {
        return MenuApply::Live;
    }
    if (std::ranges::find(kAtBoot, m.var) != std::end(kAtBoot)) {
        return MenuApply::AtBoot;
    }
    switch (m.apply) {
    case Apply::Runtime:
        return MenuApply::Live;
    case Apply::Restart:
        return MenuApply::Relaunch;
    case Apply::NextLaunch:
        return MenuApply::None;
    }
    return MenuApply::None;
}

MenuAction menu_action(const Meta& m, bool gameBooted) {
    switch (menu_apply(m)) {
    case MenuApply::Live:
        return MenuAction::Live;
    case MenuApply::AtBoot:
        return gameBooted ? MenuAction::Relaunch : MenuAction::Nothing;
    case MenuApply::Relaunch:
        return MenuAction::Relaunch;
    case MenuApply::None:
        return MenuAction::Nothing;
    }
    return MenuAction::Nothing;
}

const Meta* find_meta(const VarBase& var) {
    for (const Meta& m : settings_meta()) {
        if (m.var == &var) {
            return &m;
        }
    }
    return nullptr;
}

const Meta* find_meta(std::string_view key) {
    for (const Meta& m : settings_meta()) {
        if (m.var->key() == key) {
            return &m;
        }
    }
    // An old or alternative spelling (VarOptions::aliases), e.g. --cvar online.server_url=...
    for (const Meta& m : settings_meta()) {
        for (const std::string& alias : m.var->aliases()) {
            if (alias == key) {
                return &m;
            }
        }
    }
    return nullptr;
}

Overlay& cvar_overlay() {
    // Below the environment (SETTINGS 1.3), so not borealis's own cli_overlay(), which always
    // wins. A user edit of the var releases the value, as with borealis's.
    static Overlay sOverlay{"command line", {.priority = 150, .releaseOnUserSet = true}};
    return sOverlay;
}

Overlay& env_overlay() {
    static Overlay sOverlay{"environment", {.priority = 200, .releaseOnUserSet = false}};
    return sOverlay;
}

Overlay& net_overlay() {
    static Overlay sOverlay{"netplay", {.priority = 1000, .releaseOnUserSet = false}};
    return sOverlay;
}

int apply_cvar_overrides(std::span<const std::string> assignments) {
    int applied = 0;
    for (const std::string& assignment : assignments) {
        const auto eq = assignment.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "mppm: --cvar '%s': expected key=value\n", assignment.c_str());
            continue;
        }
        const std::string key = assignment.substr(0, eq);
        const std::string value = assignment.substr(eq + 1);
        const Meta* m = find_meta(key);
        if (m == nullptr) {
            std::fprintf(stderr, "mppm: --cvar: unknown setting '%s'\n", key.c_str());
            continue;
        }
        try {
            m->setOverlay(cvar_overlay(), *m->var, value);
            applied++;
        } catch (const borealis::config::InvalidValueError& e) {
            std::fprintf(stderr, "mppm: --cvar %s=%s: %s\n", key.c_str(), value.c_str(), e.what());
        }
    }
    return applied;
}

void apply_env_overrides() {
    for (const Meta& m : settings_meta()) {
        const char* raw = env_value(m.env);
        if (raw == nullptr) {
            continue;
        }
        std::string text;
        if (!env_to_text(m, raw, text)) {
            std::fprintf(stderr, "mppm: ignoring %s=%s (not a valid value)\n", m.env, raw);
            continue;
        }
        try {
            m.setOverlay(env_overlay(), *m.var, text);
        } catch (const borealis::config::InvalidValueError& e) {
            std::fprintf(stderr, "mppm: ignoring %s=%s: %s\n", m.env, raw, e.what());
        }
    }
}

const char* env_override_name(const VarBase& var) {
    const Meta* m = find_meta(var);
    return m != nullptr && env_overlay().contains(var) ? m->env : nullptr;
}

namespace {

// A JSON value as --cvar text, the way VarBase::to_string writes a value: a string raw, anything
// else as JSON.
std::string json_to_text(const nlohmann::json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

int count_net(Net net) {
    return static_cast<int>(std::ranges::count_if(settings_meta(), [net](const Meta& m) { return m.net == net; }));
}

} // namespace

bool net_lock_settings(const nlohmann::json& hostSynced) {
    if (!hostSynced.is_object()) {
        std::fprintf(stderr, "mppm: net: settings: the host's values are not an object\n");
        return false;
    }
    // Check everything first: a lock is all or nothing.
    for (const auto& [key, value] : hostSynced.items()) {
        const Meta* m = find_meta(key);
        if (m == nullptr || m->net != Net::Synced) {
            std::fprintf(stderr, "mppm: net: settings: '%s' is not a synced setting\n", key.c_str());
            return false;
        }
        try {
            m->checkText(*m->var, json_to_text(value));
        } catch (const borealis::config::InvalidValueError& e) {
            std::fprintf(stderr, "mppm: net: settings: %s=%s: %s\n", key.c_str(), value.dump().c_str(), e.what());
            return false;
        }
    }
    for (const Meta& m : settings_meta()) {
        if (m.net == Net::Forced && m.netValue != nullptr) {
            try {
                m.checkText(*m.var, m.netValue);
            } catch (const borealis::config::InvalidValueError& e) {
                std::fprintf(stderr, "mppm: net: settings: %s=%s: %s\n", m.var->key().c_str(), m.netValue, e.what());
                return false;
            }
        }
    }
    net_overlay().clear();
    for (const Meta& m : settings_meta()) {
        if (m.net == Net::Forced) {
            if (m.netValue != nullptr) {
                m.setOverlay(net_overlay(), *m.var, m.netValue);
            } else {
                m.setOverlayDefault(net_overlay(), *m.var);
            }
        } else if (m.net == Net::Synced) {
            const auto it = hostSynced.find(m.var->key());
            if (it != hostSynced.end()) {
                m.setOverlay(net_overlay(), *m.var, json_to_text(*it));
            } else {
                m.setOverlayDefault(net_overlay(), *m.var);
            }
        }
    }
    return true;
}

void net_unlock_settings() {
    net_overlay().clear();
}

bool net_settings_locked() {
    return !net_overlay().empty();
}

nlohmann::json net_synced_settings() {
    nlohmann::json out = nlohmann::json::object();
    for (const Meta& m : settings_meta()) {
        if (m.net == Net::Synced) {
            out[m.var->key()] = m.var->to_json();
        }
    }
    return out;
}

uint32_t net_synced_hash(const nlohmann::json& synced) {
    std::vector<std::pair<std::string, std::string>> lines; // key, line
    if (synced.is_object()) {
        for (const auto& [key, value] : synced.items()) {
            lines.emplace_back(key, key + "=" + value.dump() + "\n");
        }
    }
    std::sort(lines.begin(), lines.end());
    uint32_t h = 2166136261u;
    for (const auto& [key, line] : lines) {
        for (const unsigned char ch : line) {
            h ^= ch;
            h *= 16777619u;
        }
    }
    return h;
}

int net_forced_count() {
    return count_net(Net::Forced);
}

int net_synced_count() {
    return count_net(Net::Synced);
}

void bind_game_config() {
    auto& s = settings();
    auto& c = connections();
    // Mirror one var into gP2Config (runs now and on every change).
    const auto mirror = [&c](auto& var, auto&& fn) { c.push_back(var.observe(std::forward<decltype(fn)>(fn))); };

    mirror(s.cheat.daySpeed, [](int v) { gP2Config.daySpeed = static_cast<float>(v) / 100.0f; });
    mirror(s.cheat.sunsetOnDayOne, [](bool v) { gP2Config.sunsetOnDayOne = v; });
    mirror(s.cheat.unlockChallenge, [](bool v) { gP2Config.unlockChallenge = v; });
    mirror(s.cheat.invincibleCaptains, [](bool v) { gP2Config.invincibleCaptains = v; });
    mirror(s.cheat.infiniteSprays, [](bool v) { gP2Config.infiniteSprays = v; });
    mirror(s.game.idleDemo, [](bool v) { gP2Config.idleDemo = v; });
    mirror(s.game.skipBootLogos, [](bool v) { gP2Config.skipBootLogos = v; });
    mirror(s.game.titleSeason, [](TitleSeason v) {
        gP2Config.titleSeason = v == TitleSeason::Original ? -1 : static_cast<int>(v) - 1;
    });
    mirror(s.interface.hideHud, [](bool v) { gP2Config.hideHud = v; });
    mirror(s.backend.cardFileType, [](CardFileType v) {
        gP2Config.cardFileType = v == CardFileType::CardImage ? CARD_RAWIMAGE : CARD_GCIFOLDER;
    });
    mirror(s.audio.enabled, [](bool v) { gP2Config.audioEnabled = v; });
    mirror(s.tools.panicScreen, [](bool v) { gP2Config.panicScreen = v; });

    // Read off the game thread: their own setters (p2_host.h).
    mirror(s.audio.masterVolume, [](int v) {
        const float f = static_cast<float>(v) / 100.0f;
        p2_audio_set_master_gain(f * f); // squared, so the slider feels even
    });
    const auto audioDebug = [&s] { p2_audio_set_debug(s.tools.audioDebug.get(), s.tools.audioVoiceLog.get()); };
    mirror(s.tools.audioDebug, audioDebug);
    mirror(s.tools.audioVoiceLog, audioDebug);
    mirror(s.tools.audioDumpPath, [](const std::string& v) { p2_audio_set_dump_path(v.c_str()); });
    mirror(s.tools.schedDebug, [](bool v) { p2_sched_set_debug(v); });
}

void settings_take_boot_snapshot() {
    auto& snapshot = boot_snapshot();
    snapshot.clear();
    for (const Meta& m : settings_meta()) {
        if (m.apply == Apply::Restart) {
            snapshot.emplace_back(m.var, m.var->to_json());
        }
    }
}

std::vector<VarBase*> settings_restart_pending(bool gameBooted) {
    std::vector<VarBase*> pending;
    for (const auto& [var, value] : boot_snapshot()) {
        // The disc only needs a restart once a game runs; before that it applies on Play.
        if (!gameBooted && var == &settings().backend.discPath) {
            continue;
        }
        if (var->to_json() != value) {
            pending.push_back(var);
        }
    }
    return pending;
}

void settings_rebase_boot_snapshot(const VarBase& var) {
    for (auto& [snapVar, value] : boot_snapshot()) {
        if (snapVar == &var) {
            value = var.to_json();
        }
    }
}

std::string sanitize_frame_rate(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "original";
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value == "original" || value == "display" || value == "unlimited") {
        return value;
    }
    if (!value.empty() && std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        value.erase(0, std::min(value.find_first_not_of('0'), value.size() - 1)); // "00" -> "0", "060" -> "60"
    }
    if (value == "0" || value == "off" || value == "uncapped") {
        return "unlimited";
    }
    if (value == "match" || value == "auto") {
        return "display";
    }
    if (std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
        // Whole-string integer; a huge one saturates instead of overflowing.
        const long n = value.size() > 6 ? 1000 : std::strtol(value.c_str(), nullptr, 10);
        return std::to_string(std::clamp<long>(n, 30, 1000));
    }
    return "original";
}

int frame_rate_number(std::string_view value) {
    if (value.empty() || value.size() > 4) {
        return 0;
    }
    int n = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            return 0;
        }
        n = n * 10 + (ch - '0');
    }
    return n;
}

std::string sanitize_player_name(std::string value) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7F) {
            continue; // control characters
        }
        if ((c & 0xC0) != 0x80) {
            if (chars == 24) {
                break; // a 25th code point starts here
            }
            ++chars;
        }
        out.push_back(static_cast<char>(c));
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::string sanitize_server_url(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

const char* default_server_url() {
    return MPPM_DEFAULT_SERVER_URL;
}

LaunchAnimation effective_launch_animation() {
    return settings().interface.launchAnimation.get() == LaunchAnimation::Off ? LaunchAnimation::Off
                                                                               : LaunchAnimation::Full;
}

} // namespace mppm
