// Start-menu settings that apply at once (live_options_design 3.5, 3.8). See live_apply.hpp.

#include "live_apply.hpp"

#include "disc.hpp"
#include "mppm/launcher.hpp"
#include "prelaunch.hpp"
#include "restart.hpp"
#include "scene_bridge.hpp"
#include "settings.hpp"
#include "video.hpp"
#include "ui/document.hpp"
#include "ui/settings_window.hpp"
#include "ui/ui.hpp"

#include <aurora/lib/window.hpp>
#include <borealis/config.hpp>
#include <borealis/file_select.hpp>
#include <nlohmann/json.hpp>
#include <p2_host.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mppm::launcher::live {
namespace {

using borealis::config::VarBase;

constexpr const char* kResumeEnv   = "MPPM_RESUME";
constexpr const char* kSeamlessEnv = "MPPM_SEAMLESS_RESTART";
// The relaunch waits this long after the change (a quick change back cancels it), then the veil
// takes kVeilCloseMs to close (rcss below) before the exec.
constexpr uint64_t kRelaunchDelayMs = 350;
constexpr uint64_t kVeilCloseMs     = 150;
// The new process: the veil starts fading on this frame, over kVeilOpenMs.
constexpr int kVeilFadeFrame     = 3;
constexpr uint64_t kVeilOpenMs   = 200;

// ---- The veil: a navy sheet over everything, taking input while it is up ----

const Rml::String kVeilSource = R"RML(
<rml>
<head>
<style>
body {
    position: absolute;
    left: 0; top: 0; right: 0; bottom: 0;
    width: 100%;
    height: 100%;
    z-index: 1000;
    background-color: #050B1A;
    opacity: 0;
    transition: opacity 0.15s linear-in-out;
}
body.on { opacity: 1; }
body.instant { transition: none; }
body.opening { transition: opacity 0.2s cubic-out; }
</style>
</head>
<body></body>
</rml>
)RML";

class Veil : public ui::Document {
public:
    Veil() : Document(kVeilSource, false, ui::DocumentScope::None) {}

    void show() override {
        if (mDocument != nullptr) {
            // Takes keyboard focus so nothing under it reacts; z-index keeps it on top whatever
            // gets focus later.
            mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document, Rml::ScrollFlag::None);
        }
    }
    void set_class(const char* cls, bool on) {
        if (mDocument != nullptr) {
            mDocument->SetClass(cls, on);
        }
    }

protected:
    bool handle_nav_command(Rml::Event&, ui::NavCommand) override { return true; }
};

// ---- State ----

struct Resume {
    bool valid = false;
    std::string tab;
    std::string key;
    float scroll = 0.0f;
    int x = 0, y = 0;
    bool havePos = false;
    bool maximized = false;
    uint64_t wallMs = 0;
};

Resume sResume;                     // parsed from MPPM_RESUME in preinit
std::string sBootBackend;
bool sBootBackendTaken = false;
std::vector<borealis::config::Connection> sConnections;

// Change log: every var's value as last seen on the menu.
std::map<const VarBase*, nlohmann::json> sMenuValues;

// Relaunch scheduling (old process).
std::vector<const VarBase*> sRelaunchVars; // the vars a relaunch is waiting for, this frame
uint64_t sRelaunchSinceMs = 0;              // first frame the set was not empty; 0 = none
enum class VeilPhase { None, Closing, Opening };
VeilPhase sVeilPhase = VeilPhase::None;
uint64_t sVeilSinceMs = 0;
int sVeilFrames = 0;
Veil* sVeil = nullptr;
bool sRelaunching = false;

uint64_t wall_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// disc::inspect reads the image header: remember the last answer per path.
bool disc_ready(const std::string& path) {
    static std::string sPath;
    static bool sReady = false;
    static bool sKnown = false;
    if (!sKnown || path != sPath) {
        sPath  = path;
        sReady = !path.empty() && disc::inspect(path).state == disc::State::Ready;
        sKnown = true;
    }
    return sReady;
}

const char* action_name(MenuAction a) {
    switch (a) {
    case MenuAction::Live:
        return "live";
    case MenuAction::Relaunch:
        return "relaunch";
    case MenuAction::Nothing:
        break;
    }
    return "nothing";
}

// The action this change takes now, with the two exceptions of 3.5: a backend that resolves to
// the running one, and a disc that is not usable, need nothing.
MenuAction effective_action(const Meta& m, bool booted) {
    const MenuAction a = menu_action(m, booted);
    if (a != MenuAction::Relaunch) {
        return a;
    }
    auto& s = settings();
    if (m.var == &s.backend.graphicsBackend && ui::settings_backend_is_running(s.backend.graphicsBackend.get())) {
        return MenuAction::Nothing;
    }
    if (m.var == &s.backend.discPath && !disc_ready(s.backend.discPath.get())) {
        return MenuAction::Nothing;
    }
    return a;
}

void log_changes(bool booted) {
    for (const Meta& m : settings_meta()) {
        nlohmann::json value = m.var->to_json();
        auto it = sMenuValues.find(m.var);
        if (it == sMenuValues.end()) {
            sMenuValues.emplace(m.var, std::move(value));
            continue;
        }
        if (it->second == value) {
            continue;
        }
        it->second = std::move(value);
        std::string key(m.var->key());
        std::fprintf(stderr, "mppm: live: %s -> %s\n", key.c_str(), action_name(effective_action(m, booted)));
    }
}

ui::SettingsWindow* find_settings_window() {
    ui::SettingsWindow* found = nullptr;
    ui::for_each_visible_document([&found](ui::Document& doc) {
        if (auto* w = dynamic_cast<ui::SettingsWindow*>(&doc); w != nullptr && w->active()) {
            found = w;
        }
    });
    return found;
}

ui::Prelaunch* find_menu() {
    return dynamic_cast<ui::Prelaunch*>(ui::find_document(ui::DocumentScope::Prelaunch));
}

// Idle (3.5): the top document is the start menu or a Settings window, the menu is in no phase
// (no launch, no file dialog) and no file dialog of Settings is open.
bool menu_idle() {
    ui::Prelaunch* menu = find_menu();
    if (menu == nullptr || !menu->idle() || borealis::file_select::busy()) {
        return false;
    }
    ui::Document* top = ui::top_document();
    return top == menu || dynamic_cast<ui::SettingsWindow*>(top) != nullptr;
}

void push_veil(bool opaque) {
    auto veil = std::make_unique<Veil>();
    sVeil = veil.get();
    if (opaque) {
        sVeil->set_class("instant", true);
        sVeil->set_class("on", true);
    }
    ui::push_document(std::move(veil));
    if (!opaque) {
        sVeil->set_class("on", true); // after the first style pass, so the opacity transitions
    }
}

void close_veil() {
    if (sVeil != nullptr) {
        sVeil->force_hide(true);
        sVeil = nullptr;
        ui::uncover_top_document(); // Settings gives focus back to its row (mLastFocus)
    }
}

std::string resume_text() {
    std::string out;
    if (ui::SettingsWindow* w = find_settings_window()) {
        const auto loc = w->location();
        if (!loc.tab.empty()) {
            out += "tab=" + loc.tab + ";";
        }
        if (!loc.key.empty()) {
            out += "key=" + loc.key + ";";
        }
        out += "scroll=" + std::to_string(static_cast<int>(loc.scroll)) + ";";
    }
    if (SDL_Window* window = aurora::window::get_sdl_window()) {
        const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        int x = 0, y = 0;
        if (!maximized && !settings().video.fullscreen.get() && SDL_GetWindowPosition(window, &x, &y)) {
            out += "x=" + std::to_string(x) + ";y=" + std::to_string(y) + ";";
        }
        out += std::string("max=") + (maximized ? "1" : "0") + ";";
    }
    out += "t=" + std::to_string(wall_ms());
    return out;
}

void begin_relaunch() {
    std::string keys;
    for (const VarBase* var : sRelaunchVars) {
        keys += (keys.empty() ? "" : ",") + std::string(var->key());
    }
    std::fprintf(stderr, "mppm: relaunch: for %s\n", keys.c_str());
    const std::string resume = resume_text();
    SDL_setenv_unsafe(kResumeEnv, resume.c_str(), 1);
    push_veil(false);
    p2_audio_set_duck(0.0f); // the music fades with the picture
    sVeilPhase   = VeilPhase::Closing;
    sVeilSinceMs = SDL_GetTicks();
    sVeilFrames  = 0;
    sRelaunching = true;
}

void parse_resume(const char* text) {
    Resume r;
    std::string_view rest = text;
    bool haveX = false, haveY = false;
    while (!rest.empty()) {
        const size_t end = rest.find(';');
        const std::string_view item = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view k = item.substr(0, eq);
        const std::string v(item.substr(eq + 1));
        if (k == "tab") {
            r.tab = v;
        } else if (k == "key") {
            r.key = v;
        } else if (k == "scroll") {
            r.scroll = std::strtof(v.c_str(), nullptr);
        } else if (k == "x") {
            r.x = std::atoi(v.c_str());
            haveX = true;
        } else if (k == "y") {
            r.y = std::atoi(v.c_str());
            haveY = true;
        } else if (k == "max") {
            r.maximized = v == "1";
        } else if (k == "t") {
            r.wallMs = std::strtoull(v.c_str(), nullptr, 10);
        }
    }
    r.havePos = haveX && haveY;
    r.valid   = true;
    sResume   = std::move(r);
}

} // namespace

void preinit() {
    const char* env = std::getenv(kResumeEnv);
    if (env != nullptr && env[0] != '\0') {
        parse_resume(env);
        SDL_unsetenv_unsafe(kResumeEnv);
    }
}

void init() {
    (void)boot_backend_id();
    auto& s = settings();
    // MSAA is live on the start menu only (D2); in game the restart banner stays.
    sConnections.push_back(s.video.msaa.on_change([](int v) {
        if (prelaunch_open()) {
            video::set_msaa(v);
        }
    }));
    // A graphics backend that resolves to the one already running needs no restart, in game too
    // (e.g. back to Automatic after a choice that failed and fell back to it).
    sConnections.push_back(s.backend.graphicsBackend.on_change([] {
        auto& var = settings().backend.graphicsBackend;
        if (ui::settings_backend_is_running(var.get())) {
            settings_rebase_boot_snapshot(var);
        }
    }));
    // The window comes back where it was (X11 and Windows; Wayland places windows itself).
    if (sResume.valid) {
        if (SDL_Window* window = aurora::window::get_sdl_window()) {
            if (sResume.havePos) {
                SDL_SetWindowPosition(window, sResume.x, sResume.y);
            }
            if (sResume.maximized) {
                SDL_MaximizeWindow(window);
            }
        }
    }
}

void restore_ui() {
    if (!sResume.valid || sVeilPhase != VeilPhase::None) {
        return;
    }
    if (!sResume.tab.empty()) {
        ui::Document* opener = find_menu();
        ui::SettingsWindow& w = ui::open_settings_window(true, opener);
        w.restore(sResume.tab, sResume.key, sResume.scroll);
    }
    push_veil(true);
    sVeilPhase   = VeilPhase::Opening;
    sVeilFrames  = 0;
    sVeilSinceMs = 0;
}

void update() {
    // The veil runs whatever else happens.
    if (sVeilPhase == VeilPhase::Closing) {
        sVeilFrames++;
        // Opaque, and one more frame presented with it: now replace the process.
        if (SDL_GetTicks() - sVeilSinceMs >= kVeilCloseMs + 20 && sVeilFrames >= 2) {
            SDL_setenv_unsafe(kSeamlessEnv, "1", 1);
            request_restart(RestartTarget::StartMenu); // does not return
        }
        return;
    }
    if (sVeilPhase == VeilPhase::Opening) {
        sVeilFrames++;
        if (sVeilFrames == kVeilFadeFrame && sVeil != nullptr) {
            sVeil->set_class("instant", false);
            sVeil->set_class("opening", true);
            sVeil->set_class("on", false);
            sVeilSinceMs = SDL_GetTicks();
            if (sResume.wallMs != 0) {
                const uint64_t now = wall_ms();
                std::fprintf(stderr, "mppm: relaunch: resumed in %llu ms\n",
                             static_cast<unsigned long long>(now > sResume.wallMs ? now - sResume.wallMs : 0));
            } else {
                std::fprintf(stderr, "mppm: relaunch: resumed\n");
            }
        } else if (sVeilFrames > kVeilFadeFrame && SDL_GetTicks() - sVeilSinceMs >= kVeilOpenMs + 30) {
            close_veil();
            if (ui::SettingsWindow* w = find_settings_window()) {
                const auto loc = w->location();
                std::fprintf(stderr, "mppm: relaunch: settings on %s / %s\n", loc.tab.c_str(),
                             loc.key.empty() ? "(no row)" : loc.key.c_str());
            }
            sVeilPhase = VeilPhase::None;
            sResume    = {};
        }
    }

    if (!prelaunch_open()) {
        sMenuValues.clear();
        sRelaunchVars.clear();
        sRelaunchSinceMs = 0;
        return;
    }
    const bool booted = game_booted();
    log_changes(booted);

    sRelaunchVars.clear();
    for (VarBase* var : settings_restart_pending(booted)) {
        const Meta* m = find_meta(*var);
        if (m == nullptr) {
            continue;
        }
        const MenuAction a = effective_action(*m, booted);
        if (a == MenuAction::Relaunch) {
            sRelaunchVars.push_back(var);
        } else if (!(var == &settings().backend.discPath && menu_action(*m, booted) == MenuAction::Relaunch)) {
            // Live, or read at boot (or by the next start): this process already has it. A disc
            // that is not usable stays pending: the game keeps the disc it booted.
            settings_rebase_boot_snapshot(*var);
        }
    }
    if (sRelaunchVars.empty()) {
        sRelaunchSinceMs = 0;
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (sRelaunchSinceMs == 0) {
        sRelaunchSinceMs = now;
    }
    if (sVeilPhase == VeilPhase::None && now - sRelaunchSinceMs >= kRelaunchDelayMs && menu_idle()) {
        begin_relaunch();
    }
}

void on_game_booted() {
    for (const Meta& m : settings_meta()) {
        if (menu_apply(m) == MenuApply::AtBoot) {
            settings_rebase_boot_snapshot(*m.var);
        }
    }
}

bool relaunching() {
    return sRelaunching;
}

bool pending_relaunch() {
    return !sRelaunchVars.empty() || sRelaunching;
}

bool busy(const VarBase& var) {
    if (std::ranges::find(sRelaunchVars, &var) != sRelaunchVars.end()) {
        return true;
    }
    auto& s = settings();
    return (&var == &s.interface.liveBackground || &var == &s.interface.sceneArea) && scene::busy();
}

const std::string& boot_backend_id() {
    if (!sBootBackendTaken) {
        sBootBackendTaken = true;
        sBootBackend      = settings().backend.graphicsBackend.get();
    }
    return sBootBackend;
}

} // namespace mppm::launcher::live
