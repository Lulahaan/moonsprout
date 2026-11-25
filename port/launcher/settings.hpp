#pragma once
// Launcher and game settings (borealis::config vars, persisted to <user data>/config.json).
//
// docs/launcher/SETTINGS.md is the reference for every key: its default, range, help text, hook
// and netplay class. Keys are stable: they are the save format and the --cvar / environment
// override names. A key is never renamed; a rename goes through VarOptions::aliases.
//
// Layers, lowest to highest: default < user (config.json) < --cvar < environment (MPPM_*) <
// netplay. Only the user layer is saved. A var supplied by the environment or netplay layer is
// locked (VarBase::locked()): its control is disabled and says why.
//
// Game code never includes this header: bind_game_config() mirrors the values it needs into
// gP2Config (port/include/p2_config.h).

#include <borealis/config.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mppm {

using borealis::config::Var;
using borealis::config::VarBase;

// ---- Enumerations. Stored as the lower-case names in their tables (settings.cpp), never as
// numbers, so config.json stays readable and values can be added later. ----

enum class DiscVerification : uint8_t { Unknown, Verified, MismatchAccepted };
enum class CardFileType : uint8_t { GciFolder, CardImage };
// SETTINGS 3 / ASPECT 4. Every value is offered since ASPECT phase 1.
enum class AspectRatio : uint8_t { Auto, Original, Ratio16x10, Ratio16x9, Ratio21x9, Stretch };
enum class Resampler : uint8_t { Bilinear, Area };
enum class FpsCounter : uint8_t { Off, TopLeft, TopRight, BottomLeft, BottomRight };
enum class SoundOutput : uint8_t { Stereo, Mono };
enum class TitleSeason : uint8_t { Original, Spring, Summer, Autumn, Winter };
enum class GlassEffects : uint8_t { Off, Blur, Full };
enum class SceneArea : uint8_t { Forest, Tutorial, Yakushima, Last, Season };
enum class SceneTime : uint8_t { Morning, Day, Evening, Night, Clock };
enum class SceneMusic : uint8_t { Piklopedia, WorldMap, Challenge, HiScore, Off };
// README 6: the take-off is On (Full) or Off. Auto and Short are kept only so old config files
// parse; the sanitizer reads them as Full.
enum class LaunchAnimation : uint8_t { Auto, Full, Short, Off };
enum class HudLayout : uint8_t { Centered, Edges, Stretched };
enum class HudMaxAspect : uint8_t { Ratio16x9, Ratio21x9, Unlimited };
enum class LogLevel : uint8_t { Debug, Info, Warning, Error };

// Name tables, found by ADL from borealis::config (config_enum_values).
constexpr auto config_enum_values(DiscVerification) {
    using enum DiscVerification;
    return borealis::config::enum_table<DiscVerification>(
        {{Unknown, "unknown"}, {Verified, "verified"}, {MismatchAccepted, "mismatch-accepted"}});
}
constexpr auto config_enum_values(CardFileType) {
    using enum CardFileType;
    return borealis::config::enum_table<CardFileType>({{GciFolder, "gci-folder"}, {CardImage, "card-image"}});
}
constexpr auto config_enum_values(AspectRatio) {
    using enum AspectRatio;
    return borealis::config::enum_table<AspectRatio>({{Auto, "auto"},
        {Original, "original"},
        {Ratio16x10, "16:10"},
        {Ratio16x9, "16:9"},
        {Ratio21x9, "21:9"},
        {Stretch, "stretch"}});
}
constexpr auto config_enum_values(Resampler) {
    using enum Resampler;
    return borealis::config::enum_table<Resampler>({{Bilinear, "bilinear"}, {Area, "area"}});
}
constexpr auto config_enum_values(FpsCounter) {
    using enum FpsCounter;
    return borealis::config::enum_table<FpsCounter>({{Off, "off"},
        {TopLeft, "top-left"},
        {TopRight, "top-right"},
        {BottomLeft, "bottom-left"},
        {BottomRight, "bottom-right"}});
}
constexpr auto config_enum_values(SoundOutput) {
    using enum SoundOutput;
    return borealis::config::enum_table<SoundOutput>({{Stereo, "stereo"}, {Mono, "mono"}});
}
constexpr auto config_enum_values(TitleSeason) {
    using enum TitleSeason;
    return borealis::config::enum_table<TitleSeason>({{Original, "original"},
        {Spring, "spring"},
        {Summer, "summer"},
        {Autumn, "autumn"},
        {Winter, "winter"}});
}
constexpr auto config_enum_values(GlassEffects) {
    using enum GlassEffects;
    return borealis::config::enum_table<GlassEffects>({{Off, "off"}, {Blur, "blur"}, {Full, "full"}});
}
constexpr auto config_enum_values(SceneArea) {
    using enum SceneArea;
    return borealis::config::enum_table<SceneArea>({{Forest, "forest"},
        {Tutorial, "tutorial"},
        {Yakushima, "yakushima"},
        {Last, "last"},
        {Season, "season"}});
}
constexpr auto config_enum_values(SceneTime) {
    using enum SceneTime;
    return borealis::config::enum_table<SceneTime>(
        {{Morning, "morning"}, {Day, "day"}, {Evening, "evening"}, {Night, "night"}, {Clock, "clock"}});
}
constexpr auto config_enum_values(SceneMusic) {
    using enum SceneMusic;
    return borealis::config::enum_table<SceneMusic>({{Piklopedia, "piklopedia"},
        {WorldMap, "worldmap"},
        {Challenge, "challenge"},
        {HiScore, "hiscore"},
        {Off, "off"}});
}
constexpr auto config_enum_values(LaunchAnimation) {
    using enum LaunchAnimation;
    return borealis::config::enum_table<LaunchAnimation>(
        {{Auto, "auto"}, {Full, "full"}, {Short, "short"}, {Off, "off"}});
}
constexpr auto config_enum_values(HudLayout) {
    using enum HudLayout;
    return borealis::config::enum_table<HudLayout>(
        {{Centered, "centered"}, {Edges, "edges"}, {Stretched, "stretched"}});
}
constexpr auto config_enum_values(HudMaxAspect) {
    using enum HudMaxAspect;
    return borealis::config::enum_table<HudMaxAspect>(
        {{Ratio16x9, "16:9"}, {Ratio21x9, "21:9"}, {Unlimited, "unlimited"}});
}
constexpr auto config_enum_values(LogLevel) {
    using enum LogLevel;
    return borealis::config::enum_table<LogLevel>(
        {{Debug, "debug"}, {Info, "info"}, {Warning, "warning"}, {Error, "error"}});
}


// Stable lower-case name of an enum value (its stored form), or "" for an unknown value.
template <typename E>
std::string_view enum_name(E value) {
    return config_enum_values(E{}).name(value).value_or(std::string_view{});
}

struct Settings {
    Settings();

    // PRELAUNCH (SETTINGS 2): needed before the game boots.
    struct Backend {
        Var<std::string> discPath;                // last disc image the user picked
        Var<DiscVerification> discVerification;   // hidden: result of Verify Disc
        Var<std::string> discFingerprint;         // hidden: "<size>:<mtime>" of the verified file
        Var<std::string> graphicsBackend;         // "auto", "vulkan", "d3d12", ... (backend_from_id)
        Var<CardFileType> cardFileType;           // memory card format (Adv)
        Var<bool> skipLauncher;                   // INTERFACE row "Skip Start Menu"
    } backend;

    // VIDEO (SETTINGS 3, ASPECT 4).
    struct Video {
        Var<bool> fullscreen;
        Var<AspectRatio> aspectRatio;
        Var<bool> vsync;
        Var<std::string> frameRate;               // "original", "display", "unlimited" or 30-1000
        Var<int> windowWidth;                     // hidden: 0 = Aurora's default, else 320-16384
        Var<int> windowHeight;
        Var<int> internalResolution;              // 0 = Auto (window), 1-8 = multiples of 448 lines
        Var<Resampler> resampler;
        Var<int> anisotropy;                      // 1, 2, 4, 8, 16
        Var<FpsCounter> fpsCounter;
        Var<bool> allowCpuAdapter;                // Adv
        Var<int> msaa;                            // Exp: 1, 2, 4, 8
        Var<bool> smoothMotion;                   // phase 2 row; was video.gameplay60 (alias)
    } video;

    // INPUT (SETTINGS 4). Bindings live in Aurora's own files, not here.
    struct Input {
        Var<bool> rumble;
        Var<bool> playerLed;
        Var<bool> backgroundInput;
    } input;

    // AUDIO (SETTINGS 5). Percentages are 0-100.
    struct Audio {
        Var<int> masterVolume;
        Var<int> musicVolume;
        Var<int> effectsVolume;
        Var<SoundOutput> outputMode;
        Var<bool> muteInBackground;
        Var<bool> menuSounds;
        Var<bool> enabled;                        // Adv
        Var<std::string> outputDevice;            // later; "" = system default
    } audio;

    // GAMEPLAY (SETTINGS 6).
    struct Game {
        Var<bool> pauseInMenu;
        Var<bool> pauseInBackground;
        Var<bool> skipBootLogos;
        Var<bool> idleDemo;
        Var<TitleSeason> titleSeason;
        Var<bool> fastForward;                    // later
    } game;

    // CHEATS (SETTINGS 7).
    struct Cheat {
        Var<bool> unlockChallenge;
        Var<int> daySpeed;                        // percent: UI 0, 25-400; file 0-10000
        Var<bool> sunsetOnDayOne;
        Var<bool> invincibleCaptains;
        Var<bool> infiniteSprays;
        Var<int> pikminLimit;                     // later: 100-200
    } cheat;

    // INTERFACE (SETTINGS 9.1, DESIGN 9, SCENE 12, ASPECT 4).
    struct Interface {
        Var<int> uiScale;                         // 0 = automatic, else 75-200 step 5 (percent)
        Var<bool> reduceMotion;
        Var<bool> highContrast;
        Var<GlassEffects> glassEffects;
        Var<bool> liveBackground;                 // "Live Start Menu"
        Var<SceneArea> sceneArea;
        Var<SceneTime> sceneTime;
        Var<SceneMusic> sceneMusic;
        Var<LaunchAnimation> launchAnimation;     // "Take-off"
        Var<bool> checkForUpdates;
        Var<bool> controllerToasts;
        Var<bool> menuHint;
        Var<bool> shaderIndicator;
        Var<bool> hideHud;
        Var<HudLayout> hudLayout;
        Var<HudMaxAspect> hudMaxAspect;
        Var<bool> advancedSettings;
        Var<bool> presetChosen;                   // hidden: the first-run preset was answered
        Var<std::string> lastSeenVersion;         // hidden: for the "what's new" notice
    } interface;

    // TOOLS (SETTINGS 9.2). Developer rows are Adv.
    struct Tools {
        Var<bool> perfOverlay;
        Var<LogLevel> logLevel;
        Var<bool> panicScreen;
        Var<bool> logFps;
        Var<bool> schedDebug;
        Var<bool> audioDebug;
        Var<bool> audioVoiceLog;
        Var<std::string> audioDumpPath;           // not saved, no UI (MPPM_AUDIO_DUMP)
    } tools;

    // ONLINE (SETTINGS 18, M5.3b). Both Local (L) and Runtime: mirrored into mppm::online::configure.
    struct Online {
        Var<std::string> serverUrl;               // lobby server; default MPPM_DEFAULT_SERVER_URL (CMake)
        Var<std::string> playerName;              // shown to the other players; "" = "Player"
    } online;
};

Settings& settings();

// ---- Metadata (SETTINGS 1.2): the one table every mechanism reads (overrides, restart
// detection, netplay, resets, the settings window's badges). ----

enum class Apply : uint8_t {
    Runtime,    // RT: takes effect at once
    Restart,    // RS: needs a restart of the app (restart banner)
    NextLaunch, // NL: read once at the next launch; no prompt
};

enum class Net : uint8_t {
    Local,  // L: free to differ between players
    Synced, // S: part of the simulation; the host's value is forced on every peer
    Forced, // X: fixed to netValue while online
};

// How an environment variable's text becomes the var's value.
enum class EnvFormat : uint8_t {
    Value,   // the var's own CLI syntax (Codec<T>::parse)
    Flag,    // unset or empty: no override; "0": false; anything else: true
    Percent, // a float factor, stored as round(factor * 100) (MPPM_TIME_SCALE)
    WindowW, // width part of "WxH" (MPPM_WINDOW)
    WindowH, // height part of "WxH"
};

struct Meta {
    VarBase* var;
    Apply apply;
    Net net;
    const char* env;      // MPPM_* override name, or nullptr
    const char* netValue; // Forced: value in --cvar syntax; otherwise nullptr
    EnvFormat envFormat;
    // Sets the var's value in an overlay from --cvar syntax. Throws
    // borealis::config::InvalidValueError when the text does not parse.
    void (*setOverlay)(borealis::config::Overlay& overlay, VarBase& var, std::string_view text);
    // Sets the var's user value from --cvar syntax, as a choice in Settings would (on_change fires,
    // the cvar overlay releases it). Throws borealis::config::InvalidValueError on bad text.
    void (*setUser)(VarBase& var, std::string_view text);
    // Sets the var's default value in an overlay (the netplay lock of a Synced var the host did not
    // send).
    void (*setOverlayDefault)(borealis::config::Overlay& overlay, VarBase& var);
    // Parses --cvar syntax without setting anything. Throws borealis::config::InvalidValueError.
    void (*checkText)(VarBase& var, std::string_view text);
};

// How a change made on the start menu applies (live_options_design 3.5). The in-game behaviour
// stays Meta::apply's. Defaults follow Apply (Runtime: Live, Restart: Relaunch, NextLaunch: None);
// a few keys override it (settings.cpp, menu_apply).
enum class MenuApply : uint8_t {
    Live,     // applies at once on the menu (the scene rows, MSAA, every Runtime var)
    AtBoot,   // read when the game boots: nothing to do before that, a relaunch after
    Relaunch, // a short relaunch back to the same Settings row (graphics backend, ...)
    None,     // read each time the app starts (Skip Start Menu, window size, ...); no badge
};
enum class MenuAction { Nothing, Live, Relaunch };

MenuApply menu_apply(const Meta& meta);
// What a change of this var on the start menu needs now. AtBoot is Relaunch once the game has
// booted behind the menu, else Nothing; None is Nothing.
MenuAction menu_action(const Meta& meta, bool gameBooted);

// Every var of Settings, in declaration order.
std::span<const Meta> settings_meta();
// The entry for a var or a key, or nullptr.
const Meta* find_meta(const VarBase& var);
const Meta* find_meta(std::string_view key);

// ---- Overrides (SETTINGS 1.3) ----

// --cvar overlay (priority 150, released by a user edit of that var).
borealis::config::Overlay& cvar_overlay();
// Environment overlay (priority 200, locks its vars). Filled from the MPPM_* variables.
borealis::config::Overlay& env_overlay();
// Netplay overlay (priority 1000, locks its vars). Filled by net_lock_settings (SETTINGS 14).
borealis::config::Overlay& net_overlay();

// Applies "key=value" overrides into cvar_overlay(). Unknown keys and bad values are reported on
// stderr and skipped. Returns the number applied.
int apply_cvar_overrides(std::span<const std::string> assignments);
// Fills env_overlay() from every Meta::env variable that is set.
void apply_env_overrides();
// The environment variable that locks this var, or nullptr (for the "Set by ..." help line).
const char* env_override_name(const VarBase& var);

// ---- Netplay lock (SETTINGS 14) ----

// Fills net_overlay(): every Net::Forced var takes Meta::netValue; every Net::Synced var takes
// hostSynced[key] (the var's to_json() form), or its default when the key is absent. Returns false
// and locks nothing when a value does not parse or a key is not a Synced var (the reason is
// printed). Harness runs call it with an empty object (launcher::preinit); M5.3b calls it at the
// start barrier with the host's values.
bool net_lock_settings(const nlohmann::json& hostSynced);
// Releases every var of the lock (net_overlay().clear()).
void net_unlock_settings();
bool net_settings_locked();
// {key: effective value (to_json())} of every Net::Synced var: the M5.3b handshake payload.
nlohmann::json net_synced_settings();
// FNV-1a 32 over one line "key=<value.dump()>\n" per key, keys sorted.
uint32_t net_synced_hash(const nlohmann::json& synced);
// The counts of Net::Forced and Net::Synced vars (the "settings locked" line).
int net_forced_count();
int net_synced_count();

// ---- Bridges ----

// Mirrors game-facing settings into gP2Config and the off-game-thread setters (audio, scheduler),
// now and on every later change. Call once, after the overlays are applied.
void bind_game_config();

// ---- Restart detection (SETTINGS 11) ----

// Records the current value of every Apply::Restart var. Called at the end of launcher::preinit.
void settings_take_boot_snapshot();
// The Apply::Restart vars whose value differs from the boot snapshot (changing a value back
// clears it). backend.discPath only counts once a game has booted (gameBooted).
std::vector<VarBase*> settings_restart_pending(bool gameBooted);
// Re-bases one var of the boot snapshot on its current value. The boot flow calls it for
// backend.discPath with the disc the game actually boots, so a disc picked in the start menu before
// Play does not count as a pending restart once the game runs.
void settings_rebase_boot_snapshot(const VarBase& var);

// ---- Helpers ----

// video.frameRate's stored form (SETTINGS 3): trims and lower-cases; "0", "off", "uncapped" ->
// "unlimited"; "match", "auto" -> "display"; a whole-string integer clamps to 30-1000; anything
// else -> "original".
std::string sanitize_frame_rate(std::string value);
// The number of a numeric video.frameRate value, or 0 for "original", "display" and "unlimited".
int frame_rate_number(std::string_view value);

// online.playerName's stored form: control characters removed, spaces trimmed, at most 24 code
// points (the lobby server applies the same rule).
std::string sanitize_player_name(std::string value);
// online.serverUrl's stored form: surrounding spaces trimmed.
std::string sanitize_server_url(std::string value);
// The build's default lobby server (the CMake cache string MPPM_DEFAULT_SERVER_URL; empty = none).
const char* default_server_url();

// The take-off actually used for the next launch: Off or Full. Reduce Motion does not change it
// (the scene keeps its cameras still instead). Never returns Auto or Short.
LaunchAnimation effective_launch_animation();

} // namespace mppm
