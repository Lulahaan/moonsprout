// Scene bridge between the launcher and the game's start-menu scene section
// (docs/launcher/SCENE.md 4). The game side lives in port/game/scene; this file owns the host half:
// whether the scene runs, the settings it reads (P2SceneConfig), the commands the UI sends, the
// states and cues the scene reports, and the pointer / idle input the scene's behaviour uses.
//
// Threading: the game calls the p2_launcher_scene_* functions from its main thread, which is also
// the thread launcher::update runs on, so plain state is enough.

#include "scene_bridge.hpp"

#include "settings.hpp"

#include <p2_host.h>

#include "../game/scene/SceneHost.h"

#include <aurora/lib/window.hpp>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace mppm::launcher::scene {
namespace {

bool sPending         = false;
State sState          = State::None;
int sCommand          = P2_SCENE_CMD_NONE;
bool sPlaySent        = false; // a Play reached the scene this run (sent by the UI or a fallback)
std::vector<Cue> sQueuedCues;
std::map<int, std::function<void(Cue)>> sCueListeners;
int sNextListener     = 1;
float sShipX          = 0.0f;
float sShipY          = 0.0f;
bool sShipOnScreen    = false;
bool sMenuSeenOpen    = false; // the start menu was up while the scene ran
uint64_t sReadyAtMs   = 0;     // when the scene reported Ready (0 = not yet)
uint64_t sTakeoffAtMs = 0;     // when the take-off began, for the cue log

// The config handed to the scene (live_options 3.2). Non-structural fields follow the settings at
// once; area and live wait for the backdrop while the scene is on screen.
constexpr uint64_t kCoverMs     = 300;  // backdrop fade-in (rcss 0.25 s) before a structural change
constexpr uint64_t kCoverHoldMs = 1000; // after handing it, until the scene answers
P2SceneConfig sHanded{};
bool sHandedValid      = false;
unsigned sGen          = 0;
uint64_t sStructSince  = 0; // a structural change waits for the backdrop since then (0 = none)
uint64_t sCoverHoldAt  = 0; // a structural change was handed while covered (0 = not)

// Pointer and idle tracking (polled; the launcher has no event hook for this module).
float sPointerX           = 0.5f;
float sPointerY           = 0.5f;
bool sPointerActive       = false;
uint64_t sLastInputMs     = 0;
std::array<bool, SDL_SCANCODE_COUNT> sKeys{};
SDL_MouseButtonFlags sButtons = 0;

// Skip input (consume_any_press). sBlocked* are the inputs held when Play was pressed; an edge only
// counts once all of them have been released.
std::array<bool, SDL_SCANCODE_COUNT> sBlockedKeys{};
SDL_MouseButtonFlags sBlockedButtons = 0;
std::map<SDL_JoystickID, uint32_t> sPadHeld;    // buttons held per gamepad, as of the last poll
std::map<SDL_JoystickID, uint32_t> sPadBlocked; // ... and those held when Play was pressed
bool sSkipArmed   = true;
bool sAnyPress    = false;
uint64_t sLastPollMs = 0; // last p2_launcher_scene_command call (heartbeat)
uint64_t sPollCount  = 0; // p2_launcher_scene_command calls

// Tuning keys (SceneHost.h).
unsigned sTuneHeld    = 0;
unsigned sTunePressed = 0;

// Test-only switches stay raw environment checks (SETTINGS 1.3): same flag rule as the settings.
bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

float env_seconds(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? static_cast<float>(std::atof(v)) : 0.0f;
}

State state_from_c(int s) {
    switch (s) {
    case P2_SCENE_LOADING:
        return State::Loading;
    case P2_SCENE_READY:
        return State::Ready;
    case P2_SCENE_TAKEOFF:
        return State::Takeoff;
    case P2_SCENE_FADING:
        return State::Fading;
    case P2_SCENE_DONE:
        return State::Done;
    case P2_SCENE_FAILED:
        return State::Failed;
    case P2_SCENE_PARKED:
        return State::Parked;
    default:
        return State::None;
    }
}

// SceneArea::Season -> P2SceneConfig.area by the local month (SCENE.md 12): Dec-Feb Valley of
// Repose, Mar-Aug Awakening Wood, Sep-Nov Wistful Wild. Perplexing Pool (summer) is not offered:
// its ship stands in a walled pit and the menu shows only concrete (SCENE.md 10.5).
int resolve_area(SceneArea area, const std::tm& now) {
    switch (area) {
    case SceneArea::Tutorial:
        return 0;
    case SceneArea::Forest:
        return 1;
    case SceneArea::Yakushima:
        return 2;
    case SceneArea::Last:
        return 3;
    case SceneArea::Season:
        break;
    }
    static constexpr int kByMonth[12] = {0, 0, 1, 1, 1, 1, 1, 1, 3, 3, 3, 0};
    return kByMonth[now.tm_mon % 12];
}

// SceneTime::Clock -> morning 5-10 h, day 10-16 h, evening 16-19 h, night otherwise.
int resolve_time(SceneTime time, const std::tm& now) {
    switch (time) {
    case SceneTime::Morning:
        return 0;
    case SceneTime::Day:
        return 1;
    case SceneTime::Evening:
        return 2;
    case SceneTime::Night:
        return 3;
    case SceneTime::Clock:
        break;
    }
    const int h = now.tm_hour;
    return h >= 5 && h < 10 ? 0 : h >= 10 && h < 16 ? 1 : h >= 16 && h < 19 ? 2 : 3;
}

int music_index(SceneMusic music) {
    switch (music) {
    case SceneMusic::Off:
        return 0;
    case SceneMusic::Piklopedia:
        return 1;
    case SceneMusic::WorldMap:
        return 2;
    case SceneMusic::Challenge:
        return 3;
    case SceneMusic::HiScore:
        return 4;
    }
    return 1;
}

// The config the settings ask for now.
P2SceneConfig desired_config() {
    const auto& st          = settings();
    const bool reduceMotion = st.interface.reduceMotion.get();
    const auto takeoff      = effective_launch_animation();
    const std::time_t t     = std::time(nullptr);
    std::tm now{};
    if (const std::tm* local = std::localtime(&t)) {
        now = *local;
    }
    P2SceneConfig out{};
    out.area         = resolve_area(st.interface.sceneArea.get(), now);
    out.timeOfDay    = resolve_time(st.interface.sceneTime.get(), now);
    out.music        = music_index(st.interface.sceneMusic.get());
    out.takeoff      = takeoff == LaunchAnimation::Off ? 0 : 2; // On (full) or Off; Short is gone
    out.reduceMotion = reduceMotion ? 1 : 0;
    // One end colour shared with the UI flash (DESIGN.md 7.2); navy for Off and reduce motion.
    out.fadeColor = (reduceMotion || takeoff == LaunchAnimation::Off) ? 0x050B1Au : 0xFFF6E0u;
    out.live      = st.interface.liveBackground.get() ? 1 : 0;
    return out;
}

bool same_config(const P2SceneConfig& a, const P2SceneConfig& b) {
    return a.area == b.area && a.timeOfDay == b.timeOfDay && a.music == b.music && a.takeoff == b.takeoff &&
           a.reduceMotion == b.reduceMotion && a.fadeColor == b.fadeColor && a.live == b.live;
}

void hand(const P2SceneConfig& next) {
    if (sHandedValid && same_config(next, sHanded)) {
        return;
    }
    sHanded      = next;
    sHandedValid = true;
    sGen++;
    std::fprintf(stderr, "mppm: scene: config gen %u area %d time %d music %d take-off %d reduce %d live %d\n", sGen,
                 next.area, next.timeOfDay, next.music, next.takeoff, next.reduceMotion, next.live);
}

// Per launcher frame: what the settings ask for, handed to the scene by the rules of 3.2.
void update_config() {
    const P2SceneConfig want = desired_config();
    if (!sHandedValid) {
        hand(want);
        return;
    }
    P2SceneConfig next = sHanded;
    next.timeOfDay     = want.timeOfDay;
    next.music         = want.music;
    next.takeoff       = want.takeoff;
    next.reduceMotion  = want.reduceMotion;
    next.fadeColor     = want.fadeColor;
    const uint64_t now = SDL_GetTicks();
    if (sCoverHoldAt != 0 && now - sCoverHoldAt >= kCoverHoldMs) {
        sCoverHoldAt = 0;
    }
    if (want.area != sHanded.area || want.live != sHanded.live) {
        const bool playing = sState == State::Takeoff || sState == State::Fading || sState == State::Done;
        // On screen: the scene draws behind an open menu, so the backdrop comes back first.
        const bool onScreen = sState == State::Ready && gP2Config.launcherOpen != 0;
        bool handNow        = false;
        if (playing) {
            sStructSince = 0; // never during the take-off; the section ends anyway
        } else if (!onScreen) {
            handNow = true;
        } else {
            if (sStructSince == 0) {
                sStructSince = now;
            }
            if (now - sStructSince >= kCoverMs) {
                handNow      = true;
                sCoverHoldAt = now; // the backdrop stays until the scene answers (1 s at most)
            }
        }
        if (handNow) {
            next.area    = want.area;
            next.live    = want.live;
            sStructSince = 0;
        }
    } else {
        sStructSince = 0; // back to the handed value before it was handed: nothing to cover
    }
    hand(next);
}

// Keys that never skip the take-off: modifiers and locks, F1-F24, Print Screen and the volume and
// media keys (window, capture and system shortcuts).
bool skip_key(int code) {
    if (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI) {
        return false;
    }
    if ((code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) || (code >= SDL_SCANCODE_F13 && code <= SDL_SCANCODE_F24)) {
        return false;
    }
    switch (code) {
    case SDL_SCANCODE_UNKNOWN:
    case SDL_SCANCODE_CAPSLOCK:
    case SDL_SCANCODE_NUMLOCKCLEAR:
    case SDL_SCANCODE_SCROLLLOCK:
    case SDL_SCANCODE_PRINTSCREEN:
    case SDL_SCANCODE_SYSREQ:
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_VOLUMEUP:
    case SDL_SCANCODE_VOLUMEDOWN:
    case SDL_SCANCODE_POWER:
        return false;
    default:
        break;
    }
    return code < SDL_SCANCODE_MODE; // mode, sleep, media and application keys follow
}

// Buttons held on every open gamepad, polled (the launcher's input layer opens them).
void poll_gamepads(bool focused, bool& pressed) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::map<SDL_JoystickID, uint32_t> held;
    for (int i = 0; ids != nullptr && i < count; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) {
            continue;
        }
        uint32_t mask = 0;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
            // The guide button opens system overlays (Steam); it does not skip.
            if (b != SDL_GAMEPAD_BUTTON_GUIDE && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                mask |= 1u << b;
            }
        }
        held[ids[i]] = mask;
        const auto prev = sPadHeld.find(ids[i]);
        const uint32_t was = prev != sPadHeld.end() ? prev->second : mask; // a new pad: no edges yet
        if (focused && (mask & ~was) != 0) {
            pressed = true;
        }
    }
    SDL_free(ids);
    sPadHeld = std::move(held);
}

void queue_play() {
    sCommand  = P2_SCENE_CMD_PLAY;
    sPlaySent = true;
}

// Pointer position, idle time and the tuning keys, once per launcher frame.
void poll_input() {
    const uint64_t now = SDL_GetTicks();
    if (sLastInputMs == 0) {
        sLastInputMs = now;
    }

    // Keyboard: any change counts as input and hands navigation back to keys (pointer inactive).
    int numKeys          = 0;
    const bool* keyboard = SDL_GetKeyboardState(&numKeys);
    bool keyChanged      = false;
    bool pressed         = false; // a went-down edge that may skip the take-off
    if (keyboard != nullptr) {
        const int n = std::min<int>(numKeys, SDL_SCANCODE_COUNT);
        const SDL_Keymod mods = SDL_GetModState();
        const bool chord = (mods & (SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
        for (int i = 0; i < n; i++) {
            if (keyboard[i] != sKeys[i]) {
                keyChanged = true;
                sKeys[i]   = keyboard[i];
                if (keyboard[i] && !chord && skip_key(i)) {
                    pressed = true;
                }
            }
            if (!keyboard[i]) {
                sBlockedKeys[i] = false;
            }
        }
    }

    // Mouse, in window coordinates; position as a fraction of the window.
    float mx = 0.0f;
    float my = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    int w = 0;
    int h = 0;
    if (SDL_Window* window = aurora::window::get_sdl_window()) {
        SDL_GetWindowSize(window, &w, &h);
    }
    if (w > 0 && h > 0) {
        const float fx = mx / static_cast<float>(w);
        const float fy = my / static_cast<float>(h);
        const bool moved = std::abs(fx - sPointerX) > 0.0005f || std::abs(fy - sPointerY) > 0.0005f;
        sPointerX = fx;
        sPointerY = fy;
        if (moved || buttons != sButtons) {
            sPointerActive = fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f;
            sLastInputMs   = now;
        }
    }
    if ((buttons & ~sButtons) != 0) {
        pressed = true;
    }
    sBlockedButtons &= buttons;
    sButtons = buttons;

    // Gamepads count while the window has focus, or always with Background Input on.
    SDL_Window* window = aurora::window::get_sdl_window();
    const bool focused = (window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0) ||
                         settings().input.backgroundInput.get();
    poll_gamepads(focused, pressed);
    for (auto it = sPadBlocked.begin(); it != sPadBlocked.end();) {
        const auto held = sPadHeld.find(it->first);
        it->second &= held != sPadHeld.end() ? held->second : 0u;
        it = it->second == 0 ? sPadBlocked.erase(it) : std::next(it);
    }

    // Skip edges: none until everything held at the Play press was let go (arm_skip).
    if (!sSkipArmed) {
        const bool keysHeld = std::find(sBlockedKeys.begin(), sBlockedKeys.end(), true) != sBlockedKeys.end();
        sSkipArmed = !keysHeld && sBlockedButtons == 0 && sPadBlocked.empty();
    } else if (pressed) {
        sAnyPress = true;
    }

    if (keyChanged) {
        sPointerActive = false;
        sLastInputMs   = now;
    }

    // Tuning keys (SceneHost.h); only sampled when the mode is on.
    static const bool tune = env_flag("MPPM_SCENE_TUNE");
    if (tune && keyboard != nullptr) {
        static constexpr std::pair<SDL_Scancode, unsigned> kMap[] = {
            {SDL_SCANCODE_RIGHTBRACKET, P2_TUNE_NEXT_CLIP}, {SDL_SCANCODE_LEFTBRACKET, P2_TUNE_PREV_CLIP},
            {SDL_SCANCODE_P, P2_TUNE_PAUSE},                {SDL_SCANCODE_PERIOD, P2_TUNE_STEP},
            {SDL_SCANCODE_COMMA, P2_TUNE_STEP_BACK},        {SDL_SCANCODE_TAB, P2_TUNE_NEXT_ACTOR},
            {SDL_SCANCODE_C, P2_TUNE_FREE_CAM},             {SDL_SCANCODE_BACKSPACE, P2_TUNE_RESET},
            {SDL_SCANCODE_F8, P2_TUNE_PRINT},               {SDL_SCANCODE_J, P2_TUNE_CAM_LEFT},
            {SDL_SCANCODE_L, P2_TUNE_CAM_RIGHT},            {SDL_SCANCODE_I, P2_TUNE_CAM_FWD},
            {SDL_SCANCODE_K, P2_TUNE_CAM_BACK},             {SDL_SCANCODE_O, P2_TUNE_CAM_UP},
            {SDL_SCANCODE_U, P2_TUNE_CAM_DOWN},             {SDL_SCANCODE_Y, P2_TUNE_YAW_LEFT},
            {SDL_SCANCODE_H, P2_TUNE_YAW_RIGHT},            {SDL_SCANCODE_MINUS, P2_TUNE_FOV_IN},
            {SDL_SCANCODE_EQUALS, P2_TUNE_FOV_OUT},         {SDL_SCANCODE_T, P2_TUNE_PITCH_UP},
            {SDL_SCANCODE_G, P2_TUNE_PITCH_DOWN},           {SDL_SCANCODE_B, P2_TUNE_ARM_FIX},
        };
        unsigned held = 0;
        for (const auto& [code, bit] : kMap) {
            if (code < numKeys && keyboard[code]) {
                held |= bit;
            }
        }
        sTunePressed |= held & ~sTuneHeld; // collected until the game reads them
        sTuneHeld = held;
    }
}

} // namespace

bool available() {
    return true;
}

void arm_skip() {
    // Everything held right now (the Play press was just handled, so SDL's state has it) blocks the
    // skip until it is released.
    sBlockedKeys.fill(false);
    int numKeys = 0;
    if (const bool* keyboard = SDL_GetKeyboardState(&numKeys)) {
        for (int i = 0; i < std::min<int>(numKeys, SDL_SCANCODE_COUNT); i++) {
            sBlockedKeys[i] = keyboard[i] || sKeys[i];
        }
    }
    sBlockedButtons = SDL_GetMouseState(nullptr, nullptr) | sButtons;
    bool ignored = false;
    poll_gamepads(false, ignored);
    sPadBlocked.clear();
    for (const auto& [id, mask] : sPadHeld) {
        if (mask != 0) {
            sPadBlocked[id] = mask;
        }
    }
    sSkipArmed = false;
    sAnyPress  = false;
}

bool consume_any_press() {
    return std::exchange(sAnyPress, false);
}

bool skip_armed() {
    return sSkipArmed;
}

uint64_t heartbeat_count() {
    return sPollCount;
}

double heartbeat_age_ms() {
    return sLastPollMs == 0 ? -1.0 : static_cast<double>(SDL_GetTicks() - sLastPollMs);
}

void arm() {
    // A game booted behind the start menu always runs the scene section, parked while Live Start
    // Menu is off (live_options D1), so the setting can turn it on and off with no relaunch.
    sPending = available();
    sState   = sPending ? State::Loading : State::None;
    sStructSince = 0;
    sCoverHoldAt = 0;
    hand(desired_config()); // the whole config, structural part included (no generation bump if unchanged)
    std::fprintf(stderr, "mppm: scene: armed (live %d)\n", sHanded.live);
    sCommand = P2_SCENE_CMD_NONE;
    sPlaySent     = false;
    sMenuSeenOpen = false;
    sReadyAtMs    = 0;
    sTakeoffAtMs  = 0;
    sLastPollMs   = 0;
    sQueuedCues.clear();
    sShipOnScreen = false;
}

State state() {
    return sState;
}

bool live() {
    return sState != State::None && sHandedValid && sHanded.live != 0;
}

bool cover_wanted() {
    return sStructSince != 0 || sCoverHoldAt != 0;
}

bool busy() {
    return cover_wanted() || sState == State::Loading;
}

int on_cue(std::function<void(Cue)> fn) {
    const int id = sNextListener++;
    sCueListeners.emplace(id, std::move(fn));
    return id;
}

void remove_cue_listener(int id) {
    sCueListeners.erase(id);
}

bool ship_screen(float& x, float& y) {
    x = sShipX;
    y = sShipY;
    return sShipOnScreen;
}

void send(Command command) {
    if (command == Command::Play) {
        queue_play();
    } else {
        sCommand = P2_SCENE_CMD_SKIP;
    }
}

void update() {
    poll_input();
    update_config();

    const bool sceneIdle = sState == State::Ready || sState == State::Failed || sState == State::Loading ||
                           sState == State::Parked;
    if (sceneIdle && !sPlaySent) {
        // gP2Config.launcherOpen is what the last launcher frame saw (prelaunch_open() itself is not
        // safe here: ui::update may have just destroyed a closed menu).
        const bool menuOpen = gP2Config.launcherOpen != 0;
        sMenuSeenOpen       = sMenuSeenOpen || menuOpen;
        // The start menu went away without telling the scene (a menu that predates the take-off
        // choreography, or any other way of closing it): start the game rather than leave the
        // scene idling with no menu over it.
        if (sMenuSeenOpen && !menuOpen && sState != State::Loading) {
            queue_play();
        }
        // MPPM_SCENE_AUTOPLAY=<seconds>: press Play that long after the scene is ready (headless
        // runs, SCENE.md 8.3).
        static const float autoplay = env_seconds("MPPM_SCENE_AUTOPLAY");
        if (autoplay > 0.0f && sReadyAtMs != 0 && SDL_GetTicks() - sReadyAtMs >= static_cast<uint64_t>(autoplay * 1000.0f)) {
            std::fprintf(stderr, "mppm: scene: autoplay\n");
            queue_play();
        }
    }

    if (sQueuedCues.empty()) {
        return;
    }
    const auto cues = std::exchange(sQueuedCues, {});
    for (const Cue cue : cues) {
        for (auto& [id, fn] : sCueListeners) {
            fn(cue);
        }
    }
}

} // namespace mppm::launcher::scene

namespace scene = mppm::launcher::scene;

extern "C" {

int p2_launcher_scene_pending(void) {
    return scene::sPending ? 1 : 0;
}

void p2_launcher_scene_config(P2SceneConfig* out) {
    if (out == nullptr) {
        return;
    }
    if (!scene::sHandedValid) {
        scene::hand(scene::desired_config());
    }
    *out = scene::sHanded;
}

unsigned p2_launcher_scene_config_gen(void) {
    return scene::sGen;
}

int p2_launcher_scene_command(void) {
    scene::sLastPollMs = SDL_GetTicks(); // polled once per scene frame: the take-off's heartbeat
    scene::sPollCount++;
    return std::exchange(scene::sCommand, P2_SCENE_CMD_NONE);
}

void p2_launcher_scene_pointer(float* x, float* y, int* active) {
    if (x != nullptr) {
        *x = scene::sPointerX;
    }
    if (y != nullptr) {
        *y = scene::sPointerY;
    }
    if (active != nullptr) {
        *active = scene::sPointerActive ? 1 : 0;
    }
}

int p2_launcher_scene_idle_seconds(void) {
    if (scene::sLastInputMs == 0) {
        return 0;
    }
    return static_cast<int>((SDL_GetTicks() - scene::sLastInputMs) / 1000);
}

void p2_launcher_scene_state(int state) {
    const scene::State next = scene::state_from_c(state);
    // The scene section exists once it reports anything: H1 must not create a second one.
    scene::sPending = false;
    // The scene answered a structural change (parked, loading, ready...): the menu's cover follows
    // the state from here.
    if (next != scene::sState) {
        scene::sCoverHoldAt = 0;
    }
    if ((next == scene::State::Ready || next == scene::State::Parked) && scene::sReadyAtMs == 0) {
        scene::sReadyAtMs = SDL_GetTicks();
    }
    if (next == scene::State::Takeoff && scene::sState != scene::State::Takeoff) {
        scene::sTakeoffAtMs = SDL_GetTicks();
    }
    if (next != scene::sState) {
        std::fprintf(stderr, "mppm: scene: state %d\n", state);
        if (next == scene::State::Done && scene::sTakeoffAtMs != 0) {
            std::fprintf(stderr, "mppm: scene: done at %llu ms\n",
                         static_cast<unsigned long long>(SDL_GetTicks() - scene::sTakeoffAtMs));
        }
    }
    scene::sState = next;
}

void p2_launcher_scene_reloading(void) {
    // The section is about to be created again: H1 creates the next one while pending is set.
    std::fprintf(stderr, "mppm: scene: reload\n");
    scene::sPending      = true;
    scene::sState        = scene::State::Loading;
    scene::sCoverHoldAt  = 0; // Loading keeps the backdrop (busy) until the new section is ready
    scene::sReadyAtMs    = 0;
    scene::sLastPollMs   = 0;
    scene::sShipOnScreen = false;
    scene::sQueuedCues.clear();
}

void p2_launcher_scene_cue(int cue) {
    if (cue >= P2_SCENE_CUE_LAUNCH_BEGIN && cue <= P2_SCENE_CUE_EXIT_FRAME) {
        std::fprintf(stderr, "mppm: scene: cue %d at %llu ms\n", cue,
                     static_cast<unsigned long long>(SDL_GetTicks() - scene::sTakeoffAtMs));
        scene::sQueuedCues.push_back(static_cast<scene::Cue>(cue));
    }
}

void p2_launcher_scene_ship_screen(float x, float y, int onScreen) {
    scene::sShipX        = x;
    scene::sShipY        = y;
    scene::sShipOnScreen = onScreen != 0;
}

void p2_launcher_scene_debug(P2SceneDebug* out) {
    if (out != nullptr) {
        *out         = P2SceneDebug{};
        out->tune    = scene::env_flag("MPPM_SCENE_TUNE") ? 1 : 0;
        out->speed   = scene::env_seconds("MPPM_SCENE_SPEED");
        out->speed   = out->speed > 0.0f ? out->speed : 1.0f;
        out->hold    = scene::env_seconds("MPPM_SCENE_HOLD");
        out->clipLog = scene::env_flag("MPPM_SCENE_CLIPLOG") ? 1 : 0;
        // Eye, target, fov and an optional up vector, comma separated.
        if (const char* cam = std::getenv("MPPM_SCENE_CAM"); cam != nullptr && cam[0] != '\0') {
            float v[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
            int n       = 0;
            const char* p = cam;
            while (n < 10 && *p != '\0') {
                char* end = nullptr;
                v[n]      = std::strtof(p, &end);
                if (end == p) {
                    break;
                }
                n++;
                p = end;
                while (*p == ',' || *p == ' ') {
                    p++;
                }
            }
            if (n == 7 || n == 10) {
                out->hasCam = 1;
                std::copy(std::begin(v), std::end(v), out->cam);
            } else {
                std::fprintf(stderr, "scene: MPPM_SCENE_CAM needs 7 or 10 numbers, got %d; ignored\n", n);
            }
        }
    }
}

void p2_launcher_scene_tune_input(P2SceneTuneInput* out) {
    if (out != nullptr) {
        out->held    = scene::sTuneHeld;
        out->pressed = std::exchange(scene::sTunePressed, 0u);
    }
}

} // extern "C"
