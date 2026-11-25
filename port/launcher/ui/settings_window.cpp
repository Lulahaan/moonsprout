#include "settings_window.hpp"
#include "p2_data_dir.hpp"

#include "config_bind.hpp"
#include "input.hpp"
#include "modal.hpp"
#include "pane.hpp"
#include "preset.hpp"
#include "ui.hpp"

#include "../branding.hpp"
#include "../disc.hpp"
#include "../live_apply.hpp"
#include "../restart.hpp"
#include "../settings.hpp"
#include "../video.hpp"
#include "mppm/launcher.hpp"
#include "text_field.hpp"

#include <aurora/aurora.h>
#include <aurora/lib/window.hpp>
#include <dolphin/pad.h>
#include <borealis/file_select.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <p2_online.hpp>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mppm::ui {

using borealis::config::VarBase;

namespace {

// Material Symbols Rounded glyphs (DESIGN 3.7).
constexpr const char* kGlyphLock = "";        // lock
constexpr const char* kGlyphRestart = "";     // restart_alt
constexpr const char* kGlyphSchedule = "";    // schedule: next launch
constexpr const char* kGlyphScience = "";     // science: experimental
constexpr const char* kGlyphInfo = "";        // info
constexpr const char* kGlyphApplying = "";    // autorenew: applying on the start menu
constexpr const char* kGlyphWarning = "";     // warning

// Whether the wide model of ASPECT.md (phases 1 and 2, packages W8a/W8b) has shipped: lists the fixed
// 16:10 / 16:9 / 21:9 shapes and shows the HUD Layout and HUD Width Limit rows.
constexpr bool kWideAspects = true;
// SCENE.md phase 6 (package W9) adds the other start-menu locations; Perplexing Pool stays out (8.4).
// Greyed again after QA (SCENE.md 10.6): Wistful Wild shows the backdrop's edge in two take-off
// shots, buries the cast's feet and keeps large leaves over the logo; Valley of Repose renders its
// rock ridge as vertical streaks on about half of launches. A stored value still loads the area.
constexpr bool kAllSceneAreas = false;

// ---- Hooks registered by the packages that own a button's feature ----

struct ActionHook {
    std::function<void()> handler;
    std::function<std::string()> status;
};

std::array<ActionHook, static_cast<size_t>(SettingsAction::Count)>& action_handlers() {
    static std::array<ActionHook, static_cast<size_t>(SettingsAction::Count)> sHandlers;
    return sHandlers;
}

const std::function<void()>& action_handler(SettingsAction action) {
    return action_handlers()[static_cast<size_t>(action)].handler;
}

Rml::String action_status(SettingsAction action) {
    const auto& hook = action_handlers()[static_cast<size_t>(action)];
    if (!hook.handler) {
        return "Not available yet";
    }
    return hook.status ? Rml::String{hook.status()} : Rml::String{};
}

// ---- Small formatting helpers ----

std::string product() { return branding::kProductName; }

// The folder where settings, saves and logs live (the same one launcher::preinit gives Aurora).
const std::string& data_folder() {
    return mppm::data_dir_utf8();
}

// disc::inspect reads the image's header; rows ask every frame, so remember the last answer.
const disc::Info& inspect_disc(const std::string& path) {
    static std::string sPath = "\x01";
    static disc::Info sInfo;
    if (path != sPath) {
        sPath = path;
        sInfo = disc::inspect(path);
    }
    return sInfo;
}

bool disc_ready() {
    return inspect_disc(settings().backend.discPath.get()).state == disc::State::Ready;
}

// The path with the home folder shortened to "~", for one-line display.
std::string abbreviate_path(const std::string& path) {
    if (const char* home = SDL_getenv("HOME"); home != nullptr && home[0] != '\0') {
        const std::string_view h = home;
        if (path.starts_with(h) && (path.size() == h.size() || path[h.size()] == '/')) {
            return "~" + path.substr(h.size());
        }
    }
    return path;
}

bool open_folder(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string url = "file://";
    url += path;
    return SDL_OpenURL(url.c_str());
}

// "Auto (matches window)" or "3x (2389x1344)": the game draws 448 lines per step, and the width
// follows the picture's shape (SETTINGS 3, Internal Resolution).
Rml::String internal_resolution_label(int n) {
    if (n <= 0) {
        return "Auto (matches window)";
    }
    const auto& s = settings();
    float aspect = 608.0f / 448.0f; // the game's own picture (Original)
    const AspectRatio mode = s.video.aspectRatio.get();
    switch (mode) {
    case AspectRatio::Ratio16x10:
        aspect = 16.0f / 10.0f;
        break;
    case AspectRatio::Ratio16x9:
        aspect = 16.0f / 9.0f;
        break;
    case AspectRatio::Ratio21x9:
        aspect = 64.0f / 27.0f;
        break;
    case AspectRatio::Stretch:
    case AspectRatio::Auto:
        if (mode == AspectRatio::Stretch || kWideAspects) {
            const AuroraWindowSize size = aurora::window::get_window_size();
            if (size.width > 0 && size.height > 0) {
                aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
            }
        }
        break;
    case AspectRatio::Original:
        break;
    }
    const int height = 448 * n;
    const int width = static_cast<int>(std::lround(height * aspect));
    return fmt::format("{}x ({}×{})", n, width, height);
}

Rml::String day_speed_label(int v) {
    if (v == 0) {
        return "Sun stopped";
    }
    if (v == 100) {
        return "Normal";
    }
    return fmt::format("{}%", v);
}

// Menu Size's largest step: 4/3 of the window-fit base is all any window holds (ui.cpp).
constexpr int kMenuSizeTop = 130;

Rml::String ui_scale_label(int v) {
    if (v == 0) {
        return "Automatic";
    }
    // Above what the window holds the menus stop growing: say what is used instead.
    const int max = std::max(max_ui_scale_percent(), 100);
    return v > max ? fmt::format("{}% (using {}%)", v, max) : fmt::format("{}%", v);
}

std::vector<int> range_values(int first, int last, int step, std::vector<int> head = {}) {
    for (int v = first; v <= last; v += step) {
        head.push_back(v);
    }
    return head;
}

// ---- Graphics backends (SETTINGS 2) ----

struct BackendInfo {
    AuroraBackend backend;
    const char* id;
    const char* name;
};
constexpr BackendInfo kBackends[] = {
    {BACKEND_VULKAN, "vulkan", "Vulkan"},
    {BACKEND_D3D12, "d3d12", "Direct3D 12"},
    {BACKEND_D3D11, "d3d11", "Direct3D 11"},
    {BACKEND_METAL, "metal", "Metal"},
    {BACKEND_OPENGL, "opengl", "OpenGL"},
    {BACKEND_OPENGLES, "opengles", "OpenGL ES"},
    {BACKEND_WEBGPU, "webgpu", "WebGPU"},
};

const char* backend_name(AuroraBackend backend) {
    for (const auto& b : kBackends) {
        if (b.backend == backend) {
            return b.name;
        }
    }
    return nullptr;
}

const BackendInfo* backend_by_id(std::string_view id) {
    for (const auto& b : kBackends) {
        if (id == b.id) {
            return &b;
        }
    }
    return nullptr;
}

// The backend id this run was started with (captured by live::init, before any window opens).
const std::string& boot_backend_id() {
    return launcher::live::boot_backend_id();
}

// The backend asked for at boot when Aurora could not start it and fell back to another one
// (OpenGL ES without libEGL, say), else nullptr.
const BackendInfo* failed_backend() {
    const BackendInfo* asked = backend_by_id(boot_backend_id());
    return asked != nullptr && asked->backend != aurora_get_backend() ? asked : nullptr;
}

// Whether `id` picks the backend that is running now, so choosing it needs no restart. Automatic
// does when this run was started on Automatic or its explicit choice failed (the fallback is
// Automatic's own pick).
bool backend_id_is_running(std::string_view id) {
    if (id == "auto") {
        return boot_backend_id() == "auto" || backend_by_id(boot_backend_id()) == nullptr ||
               failed_backend() != nullptr;
    }
    const BackendInfo* b = backend_by_id(id);
    return b != nullptr && b->backend == aurora_get_backend();
}

// Frame Rate (SETTINGS 3, fps_design 3.3). Built with the window, so "Match Display" names the
// refresh rate of the display the window is on when the window opened.
constexpr int kFrameRateSteps[] = {60, 90, 120, 144, 165, 240};

std::vector<Choice<std::string>> frame_rate_choices() {
    std::vector<Choice<std::string>> choices;
    choices.push_back({"original", "Original (30 / 60)"});
    const float hz = video::display_refresh_hz();
    choices.push_back({"display", hz > 0.0f ? fmt::format("Match Display ({:.0f} Hz)", hz) : Rml::String{"Match Display"}});
    for (const int n : kFrameRateSteps) {
        choices.push_back({std::to_string(n), fmt::format("{} FPS", n)});
    }
    choices.push_back({"unlimited", "Unlimited"});
    // A number from the file or --cvar that the list does not offer stays visible as itself.
    const std::string& current = settings().video.frameRate.get();
    if (frame_rate_number(current) > 0 &&
        std::none_of(choices.begin(), choices.end(), [&](const auto& c) { return c.value == current; }))
    {
        choices.push_back({current, fmt::format("{} FPS (custom)", current)});
    }
    return choices;
}

std::vector<Choice<std::string>> backend_choices() {
    std::vector<Choice<std::string>> choices;
    // "Automatic" names the backend it picked for this run (only known when it did the picking).
    const char* running = backend_name(aurora_get_backend());
    choices.push_back({"auto", running != nullptr && backend_id_is_running("auto")
                                   ? fmt::format("Automatic ({})", running)
                                   : Rml::String{"Automatic"}});
    const BackendInfo* failed = failed_backend();
    size_t count = 0;
    const AuroraBackend* available = aurora_get_available_backends(&count);
    for (size_t i = 0; i < count; i++) {
        for (const auto& b : kBackends) {
            if (b.backend == available[i]) {
                // A backend that failed to start says so, and names the one used instead.
                choices.push_back({b.id, &b == failed && running != nullptr
                                             ? fmt::format("{} (failed, using {})", b.name, running)
                                             : Rml::String{b.name}});
            }
        }
    }
    return choices;
}

// ---- Disc picker (the SDL dialog answers on another thread) ----

std::mutex sPickMutex;
std::optional<std::string> sPickedDisc;

void pick_disc() {
    if (borealis::file_select::busy()) {
        return;
    }
    borealis::file_select::FileOptions options;
    options.parentWindow = aurora::window::get_sdl_window();
    options.filters = {
        {"GameCube Disc Images", disc::kImageFilter},
        {"All Files", "*"},
    };
    borealis::file_select::open_file(std::move(options), [](borealis::file_select::Result result) {
        // A cancel can arrive as one empty location (SDL's zenity backend): not a choice.
        if (result.status == borealis::file_select::Status::Selected && !result.locations.empty() && !result.locations.front().empty()) {
            std::lock_guard lk(sPickMutex);
            sPickedDisc = result.locations.front();
        }
    });
}

// ---- "Next launch" detection: values of Apply::NextLaunch vars when the first settings window
// opened. Nothing changes them before that (they have no hotkeys), so this equals the boot value.

std::map<const VarBase*, nlohmann::json>& next_launch_snapshot() {
    static std::map<const VarBase*, nlohmann::json> sSnapshot;
    static bool sTaken = false;
    if (!sTaken) {
        sTaken = true;
        for (const Meta& m : settings_meta()) {
            if (m.apply == Apply::NextLaunch) {
                sSnapshot.emplace(m.var, m.var->to_json());
            }
        }
    }
    return sSnapshot;
}

bool next_launch_pending(const VarBase& var) {
    const auto& snapshot = next_launch_snapshot();
    const auto it = snapshot.find(&var);
    return it != snapshot.end() && it->second != var.to_json();
}

// ---- Controls the stock set doesn't have ----

// A row whose value steps through a fixed list with Left / Right (Confirm steps forward and wraps),
// with a label per value: Internal Resolution, Day Speed, Menu Size. Chevrons either side of the
// value show that it steps (DESIGN 5.6, like the slider's); the one at an end of the list dims, and
// a click on either steps that way.
class StepperButton : public BaseControlledSelectButton {
public:
    struct Props {
        Rml::String key;
        std::vector<int> values;
        std::function<Rml::String(int)> format;
        std::function<int()> getValue;
        std::function<void(int)> setValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
    };

    StepperButton(Rml::Element* parent, Props props)
        : BaseControlledSelectButton(parent, {.key = props.key, .submit = false}),
          mProps(std::move(props)) {
        mRoot->SetClass("stepper-button", true);
        auto* doc = mRoot->GetOwnerDocument();
        mChevLeft = mRoot->InsertBefore(doc->CreateElement("step-chev"), mValueElem);
        mChevLeft->SetClass("l", true);
        mChevRight = append(mRoot, "step-chev");
        mChevRight->SetClass("r", true);
        for (Rml::Element* chev : {mChevLeft, mChevRight}) {
            const int direction = chev == mChevLeft ? -1 : 1;
            Component::listen(chev, Rml::EventId::Mousedown, [this](Rml::Event& event) {
                if (!disabled()) {
                    mRoot->Focus(true);
                }
                event.StopPropagation();
            });
            Component::listen(chev, Rml::EventId::Click, [this, direction](Rml::Event& event) {
                if (!disabled()) {
                    step(direction, false);
                }
                event.StopPropagation();
            });
        }
        update();
    }

    void update() override {
        BaseControlledSelectButton::update();
        const auto& values = mProps.values;
        const int current = mProps.getValue ? mProps.getValue() : 0;
        const bool atStart = values.empty() || current <= values.front();
        const bool atEnd = values.empty() || current >= values.back();
        if (atStart != mAtStart) {
            mAtStart = atStart;
            mChevLeft->SetClass("end", atStart);
        }
        if (atEnd != mAtEnd) {
            mAtEnd = atEnd;
            mChevRight->SetClass("end", atEnd);
        }
    }

    bool modified() const override { return mProps.isModified && mProps.isModified(); }
    bool disabled() const override { return mProps.isDisabled && mProps.isDisabled(); }

protected:
    Rml::String format_value() override {
        const int v = mProps.getValue();
        return mProps.format ? mProps.format(v) : fmt::to_string(v);
    }

    bool handle_nav_command(NavCommand cmd) override {
        if (cmd == NavCommand::Right) {
            step(1, false);
        } else if (cmd == NavCommand::Left) {
            step(-1, false);
        } else if (cmd == NavCommand::Confirm) {
            step(1, true);
        } else {
            return false;
        }
        // Left / Right stay the adjust keys at the ends of the list too: holding Right to the
        // last value must not run on into the help pane's "Reset to Default".
        return true;
    }

private:
    void step(int direction, bool wrap) {
        const auto& values = mProps.values;
        if (values.empty() || !mProps.setValue) {
            return;
        }
        const int current = mProps.getValue();
        int next = current;
        if (direction > 0) {
            // The first listed value above the current one (works for values off the list too).
            const auto it = std::ranges::find_if(values, [current](int v) { return v > current; });
            // Nothing above: Right stays put (a value above the list, from an older config, must
            // not drop on an "increase" key); Confirm wraps to the start.
            next = it != values.end() ? *it : (wrap ? values.front() : current);
        } else {
            const auto it = std::ranges::find_if(
                values.rbegin(), values.rend(), [current](int v) { return v < current; });
            next = it != values.rend() ? *it : values.front();
        }
        if (next == current) {
            return;
        }
        mProps.setValue(next);
        play_sound(kSoundItemChange);
    }

    Props mProps;
    Rml::Element* mChevLeft = nullptr;
    Rml::Element* mChevRight = nullptr;
    int mAtStart = -1;
    int mAtEnd = -1;
};

template <typename T>
StepperButton::Props bind_stepper(borealis::config::Var<T>& var, StepperButton::Props props) {
    props.getValue = [&var] { return static_cast<int>(var.get()); };
    props.setValue = [&var](int v) { var.set(static_cast<T>(v)); };
    props.isModified = [&var] { return var.modified(); };
    props.isDisabled = detail::config_disabled(var, std::move(props.isDisabled));
    return props;
}

// Text whose content follows a function, refreshed every frame (only touched when it changes).
class LiveRml : public Component {
public:
    LiveRml(Rml::Element* parent, const Rml::String& tag, std::function<Rml::String()> rml)
        : Component(append(parent, tag)), mRml(std::move(rml)) {
        update();
    }

    void update() override {
        Rml::String rml = mRml();
        if (rml != mCurrent) {
            mRoot->SetInnerRML(rml);
            set_display(mRoot, rml.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
            mCurrent = std::move(rml);
        }
        Component::update();
    }

    // Text only: never a focus target (a new element's `focus: none` style is not computed yet
    // when the help pane is built and focused in the same frame).
    bool focus() override { return false; }

private:
    std::function<Rml::String()> mRml;
    Rml::String mCurrent = "\x01"; // never equal to a real value: the first update always writes
};

// The name of the keyboard key Player 1 has bound to the pad's Y button ("Q" by default), or
// empty when none is.
Rml::String key_bound_to_y() {
    u32 count = 0;
    const PADKeyButtonBinding* keys = PADGetKeyButtonBindings(0, &count);
    for (u32 i = 0; keys != nullptr && i < count; ++i) {
        if (keys[i].padButton == PAD_BUTTON_Y) {
            const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(keys[i].scancode));
            return name != nullptr ? Rml::String{name} : Rml::String{};
        }
    }
    return {};
}

// "Reset to Default" in the help pane: shown only while the value differs from its default, and
// disabled while an override locks it.
class ResetButton : public ControlledButton {
public:
    ResetButton(Rml::Element* parent, VarBase& var, std::function<bool(const VarBase&)> modified)
        : ControlledButton(parent,
              {
                  .text = "Reset to Default",
                  .isDisabled = [&var] { return var.locked(); },
              }),
          mVar(var), mModified(std::move(modified)) {
        mRoot->SetClass("compact", true);
        mRoot->SetClass("reset-button", true);
        // The shortcut that also resets the focused row (DESIGN 5.17): Y on a controller, and the
        // keyboard key bound to Y (Q by default). On sliders and steppers Left / Right stay the
        // adjust keys, so this is the keyboard and controller way to reset them.
        // Only the glyph of the device last used shows (DESIGN 5.17), as the footer prompts do: a
        // pad orb next to a key cap read as two keyboard keys, and Y on the keyboard does nothing.
        if (const Rml::String key = key_bound_to_y(); !key.empty()) {
            mKeyGlyph = append(mRoot, "reset-key");
            append_text(mKeyGlyph, key);
        }
        mPadGlyph = append(mRoot, "reset-key");
        mPadGlyph->SetClass("pad", true);
        append_text(mPadGlyph, "Y");
        update();
    }

    void update() override {
        ControlledButton::update();
        const bool show = mModified(mVar);
        if (show != mShown) {
            mShown = show;
            set_display(mRoot, show ? Rml::Style::Display::Block : Rml::Style::Display::None);
        }
        const int pad = input::last_input_gamepad() || mKeyGlyph == nullptr ? 1 : 0;
        if (pad != mPadShown) {
            mPadShown = pad;
            if (mKeyGlyph != nullptr) {
                set_display(mKeyGlyph, pad ? Rml::Style::Display::None : Rml::Style::Display::InlineBlock);
            }
            set_display(mPadGlyph, pad ? Rml::Style::Display::InlineBlock : Rml::Style::Display::None);
        }
    }

    // Hidden (the value is the default): nothing to land on.
    bool focus() override { return mShown == 1 && ControlledButton::focus(); }

private:
    VarBase& mVar;
    std::function<bool(const VarBase&)> mModified;
    Rml::Element* mKeyGlyph = nullptr;
    Rml::Element* mPadGlyph = nullptr;
    int mShown = -1;
    int mPadShown = -1;
};

// A file path for a <file-path>, as one span per folder: the line can then wrap after a slash
// instead of in the middle of a name.
Rml::String path_rml(const std::string& path) {
    Rml::String rml;
    size_t begin = 0;
    while (begin < path.size()) {
        size_t end = path.find_first_of("/\\", begin);
        end = end == std::string::npos ? path.size() : end + 1;
        rml += fmt::format("<path-part>{}</path-part>", escape(path.substr(begin, end - begin)));
        begin = end;
    }
    return rml;
}

Rml::String note_rml(const char* glyph, const char* cls, const Rml::String& text) {
    return fmt::format("<help-note class=\"{}\"><note-icon>{}</note-icon><note-text>{}</note-text></help-note>",
        cls, glyph, text);
}

// Whether a var differs from its default, for any of the types the settings use.
bool var_modified(const VarBase& var) {
    bool result = false;
    const auto check = [&]<typename T>(T*) {
        if (auto* v = var.as<T>()) {
            result = v->modified();
        }
    };
    check(static_cast<bool*>(nullptr));
    check(static_cast<int*>(nullptr));
    check(static_cast<std::string*>(nullptr));
    check(static_cast<DiscVerification*>(nullptr));
    check(static_cast<CardFileType*>(nullptr));
    check(static_cast<AspectRatio*>(nullptr));
    check(static_cast<Resampler*>(nullptr));
    check(static_cast<FpsCounter*>(nullptr));
    check(static_cast<SoundOutput*>(nullptr));
    check(static_cast<TitleSeason*>(nullptr));
    check(static_cast<GlassEffects*>(nullptr));
    check(static_cast<SceneArea*>(nullptr));
    check(static_cast<SceneTime*>(nullptr));
    check(static_cast<SceneMusic*>(nullptr));
    check(static_cast<LaunchAnimation*>(nullptr));
    check(static_cast<HudLayout*>(nullptr));
    check(static_cast<HudMaxAspect*>(nullptr));
    check(static_cast<LogLevel*>(nullptr));
    return result;
}

}  // namespace

// ---- Rows ----

enum class Tier : uint8_t {
    Normal,
    Advanced,     // shown only with Advanced Settings on
    Experimental, // likewise, plus the Experimental badge
};

struct RowSpec {
    Rml::String label;
    Rml::String help;                      // RML
    Tier tier = Tier::Normal;
    std::function<bool()> isDisabled;      // beyond the var's lock
    Rml::String disabledNote;              // shown in the help pane while isDisabled() holds
    std::function<void(Pane&)> extraHelp;  // controls under the help text
};

struct SettingsWindow::Row {
    RowSpec spec;
    SettingsWindow* window = nullptr;
    Pane* pane = nullptr;            // the controls pane the row sits in
    Component* control = nullptr;
    VarBase* var = nullptr;          // nullptr for buttons
    const Meta* meta = nullptr;
    Rml::String defaultText;         // "On", "Original (4:3)", ...
    Rml::Element* badges = nullptr;
    unsigned badgeMask = ~0u;
};

struct SettingsWindow::TabState {
    std::vector<std::unique_ptr<Row>> rows;
    int hiddenAdvancedChanged = 0;
    std::vector<VarBase*> hiddenAdvanced; // every hidden row's var, for the footer's live count
};

namespace {

enum Badge : unsigned {
    kBadgeLock = 1u << 0,
    kBadgeRestart = 1u << 1,
    kBadgeNextLaunch = 1u << 2,
    kBadgeExperimental = 1u << 3,
    kBadgeApplying = 1u << 4,
};

// Apply::Restart, and the disc only once the game has booted (read each frame: the game can boot
// behind the start menu while Settings is open).
bool restart_applies(const SettingsWindow::Row& row) {
    return row.meta != nullptr && row.meta->apply == Apply::Restart &&
           (row.var != &settings().backend.discPath || launcher::game_booted());
}

// Why a locked var can't be changed here, or "" when it can.
Rml::String lock_reason(const VarBase& var) {
    if (!var.locked()) {
        return {};
    }
    if (const char* env = env_override_name(var)) {
        return fmt::format("Set by the environment variable <b>{}</b>. Remove it to change this here.", env);
    }
    if (net_overlay().contains(var)) {
        // The lock is on while p2_net_active(): a replay or harness run, or an online match (locked
        // with the host's values at its start, SETTINGS 14 and 18).
        return mppm::online::in_game() ? "Set by the host of this online game."
                                       : "Locked while a replay or harness session runs.";
    }
    const auto* overlay = var.overlay();
    return fmt::format("Set by {}.", overlay != nullptr ? escape(overlay->name()) : "an override");
}

}  // namespace

// Builds the rows of one tab into its two panes.
class RowBuilder {
public:
    RowBuilder(SettingsWindow& window, Pane& left, Pane& right)
        : mWindow(window), mLeft(left), mRight(right) {
        // Disabled rows still take focus, so their help can say why (SETTINGS 9.2).
        mLeft.set_focus_disabled(true);
    }

    ~RowBuilder() {
        mPendingSection.clear(); // a heading whose rows were all hidden
        if (mWindow.mTab->hiddenAdvancedChanged > 0) {
            const int n = mWindow.mTab->hiddenAdvancedChanged;
            mLeft.add_rml(fmt::format("<advanced-note>{} advanced option{} changed.</advanced-note>",
                n, n == 1 ? " is" : "s are"));
            add_action({.label = "Show Advanced Settings",
                           .help = "Advanced settings are hidden. Turn on Advanced Settings in "
                                   "INTERFACE to see and change them."},
                [] { settings().interface.advancedSettings.set(true); });
        }
    }

    void section(const Rml::String& title) { mPendingSection = title; }

    void notice(const Rml::String& rml, const char* cls) {
        flush_section();
        auto* el = mLeft.add_rml(rml);
        el->SetClass("settings-notice", true);
        el->SetClass(cls, true);
    }

    void toggle(Var<bool>& var, RowSpec spec) {
        if (!visible(var, spec.tier)) {
            return;
        }
        auto& c = mLeft.add_child<BoolButton>(bind(var, {.key = spec.label, .isDisabled = spec.isDisabled}));
        finish(c, &var, var.default_value() ? "On" : "Off", std::move(spec));
    }

    template <typename T>
    void choices(Var<T>& var, std::vector<Choice<T>> list, RowSpec spec,
        std::function<int()> getValue = {}) {
        if (!visible(var, spec.tier)) {
            return;
        }
        Rml::String defaultText;
        for (const auto& choice : list) {
            if (choice.value == var.default_value()) {
                defaultText = choice.label;
            }
        }
        auto& c = mLeft.add_child<DropdownButton>(bind_choices(var, std::move(list),
            {.key = spec.label, .getValue = std::move(getValue), .isDisabled = spec.isDisabled}));
        finish(c, &var, defaultText, std::move(spec));
    }

    void number(Var<int>& var, NumberButton::Props props, RowSpec spec) {
        if (!visible(var, spec.tier)) {
            return;
        }
        props.key = spec.label;
        props.isDisabled = spec.isDisabled;
        const Rml::String defaultText = fmt::format("{}{}{}", props.prefix, var.default_value(), props.suffix);
        // The var's own range is the file's; the props' range is what the UI offers.
        const int min = props.min, max = props.max;
        auto bound = bind(var, std::move(props));
        bound.min = min;
        bound.max = max;
        auto& c = mLeft.add_child<NumberButton>(std::move(bound));
        finish(c, &var, defaultText, std::move(spec));
    }

    void stepper(Var<int>& var, std::vector<int> values,
        std::function<Rml::String(int)> format, RowSpec spec) {
        if (!visible(var, spec.tier)) {
            return;
        }
        const Rml::String defaultText = format(var.default_value());
        auto& c = mLeft.add_child<StepperButton>(bind_stepper(var,
            {.key = spec.label, .values = std::move(values), .format = std::move(format),
                .isDisabled = spec.isDisabled}));
        finish(c, &var, defaultText, std::move(spec));
    }

    // A row that does something when pressed. `value` (optional) is shown on the right; `var`
    // (optional) is the setting the action edits, for its lock and restart badges.
    ControlledSelectButton& add_action(RowSpec spec, std::function<void()> onPressed,
        std::function<Rml::String()> value = {}, VarBase* var = nullptr) {
        flush_section();
        auto& c = mLeft.add_select_button({
            .key = spec.label,
            .getValue = std::move(value),
            .isDisabled = spec.isDisabled,
        });
        c.root()->SetClass("action-row", true);
        if (onPressed) {
            c.on_pressed([onPressed = std::move(onPressed)] {
                play_sound(kSoundClick);
                onPressed();
            });
        }
        finish(c, var, {}, std::move(spec));
        return c;
    }

    // A button row for a feature another package provides (SettingsAction).
    ControlledSelectButton& external_action(RowSpec spec, SettingsAction action) {
        auto isDisabled = std::move(spec.isDisabled);
        spec.isDisabled = [action, isDisabled] {
            return !action_handler(action) || (isDisabled && isDisabled());
        };
        if (spec.disabledNote.empty()) {
            spec.disabledNote = "Not available yet.";
        }
        return add_action(std::move(spec), [action] {
            if (const auto& handler = action_handler(action)) {
                handler();
            }
        }, [action] { return action_status(action); });
    }

    // A text row (online.serverUrl, online.playerName): the value, a placeholder while empty.
    void text(Var<std::string>& var, Rml::String placeholder, int maxLength, Rml::String defaultText, RowSpec spec) {
        if (!visible(var, spec.tier)) {
            return;
        }
        auto isDisabled = spec.isDisabled;
        auto& c = mLeft.add_child<TextField>(TextField::Props{
            .key = spec.label,
            .getValue = [&var] { return Rml::String{var.get()}; },
            .setValue = [&var](Rml::String value) { var.set(std::move(value)); },
            .placeholder = std::move(placeholder),
            .isDisabled = detail::config_disabled(var, std::move(isDisabled)),
            .isModified = [&var] { return var.modified(); },
            .maxLength = maxLength,
        });
        finish(c, &var, std::move(defaultText), std::move(spec));
    }

    // A read-only row (focusable so its help can be read).
    ControlledSelectButton& info(RowSpec spec, Rml::String value) {
        flush_section();
        auto& c = mLeft.add_select_button({
            .key = spec.label,
            .getValue = [value = std::move(value)] { return value; },
            .submit = false,
        });
        c.root()->SetClass("info-row", true);
        finish(c, nullptr, {}, std::move(spec));
        return c;
    }

private:
    // Advanced and experimental rows are only built with Advanced Settings on; changed hidden ones
    // are counted for the note at the end of the tab.
    bool visible(VarBase& var, Tier tier) {
        if (tier == Tier::Normal || settings().interface.advancedSettings.get()) {
            flush_section();
            return true;
        }
        mWindow.mTab->hiddenAdvanced.push_back(&var);
        if (var_modified(var)) {
            mWindow.mTab->hiddenAdvancedChanged++;
        }
        return false;
    }

    void flush_section() {
        if (!mPendingSection.empty()) {
            mLeft.add_section(mPendingSection);
            mPendingSection.clear();
        }
    }

    void finish(Component& control, VarBase* var, Rml::String defaultText, RowSpec spec) {
        auto row = std::make_unique<SettingsWindow::Row>();
        row->spec = std::move(spec);
        row->window = &mWindow;
        row->pane = &mLeft;
        row->control = &control;
        row->var = var;
        row->meta = var != nullptr ? find_meta(*var) : nullptr;
        row->defaultText = std::move(defaultText);
        // Badges sit between the label and the value (SETTINGS 1.5, row anatomy).
        auto* root = control.root();
        auto badges = root->GetOwnerDocument()->CreateElement("row-badges");
        Rml::Element* valueElem = nullptr;
        for (int i = 0; i < root->GetNumChildren(); i++) {
            const Rml::String& tag = root->GetChild(i)->GetTagName();
            if (tag == "value" || (tag == "step-chev" && valueElem == nullptr)) {
                valueElem = root->GetChild(i);
                if (tag == "step-chev") {
                    break; // a stepper: badges go before its left chevron
                }
            }
        }
        row->badges = valueElem != nullptr ? root->InsertBefore(std::move(badges), valueElem)
                                           : root->AppendChild(std::move(badges));
        SettingsWindow::Row* rowPtr = row.get();
        mLeft.register_control(control, mRight, [rowPtr](Pane& pane) { build_help(pane, *rowPtr); });
        // Pane::register_control ignores disabled rows; locked ones still explain themselves on hover.
        Pane* right = &mRight;
        control.listen(root, Rml::EventId::Mouseover, [rowPtr, right, &control](Rml::Event&) {
            if (control.disabled()) {
                right->clear();
                build_help(*right, *rowPtr);
            }
        });
        mWindow.mTab->rows.push_back(std::move(row));
    }

public:
    // The help pane for one row (SETTINGS 1.5): help text, notes (lock, restart, next launch,
    // experimental, why disabled), extra controls, then "Default: ..." and Reset when modified.
    static void build_help(Pane& pane, SettingsWindow::Row& row) {
        pane.add_rml(fmt::format("<help-title>{}</help-title>", escape(row.spec.label)));
        if (!row.spec.help.empty()) {
            pane.add_rml(row.spec.help)->SetClass("help-body", true);
        }
        pane.add_child<LiveRml>("help-notes", [&row] {
            Rml::String rml;
            VarBase* var = row.var;
            if (var != nullptr) {
                if (Rml::String reason = lock_reason(*var); !reason.empty()) {
                    rml += note_rml(kGlyphLock, "lock", reason);
                }
            }
            if (row.spec.isDisabled && row.spec.isDisabled() && !row.spec.disabledNote.empty()) {
                rml += note_rml(kGlyphInfo, "info", row.spec.disabledNote);
            }
            if (row.window->mPrelaunch && row.meta != nullptr) {
                // On the start menu (live_options_design 3.9): everything applies as it changes,
                // a few rows through a short relaunch.
                const MenuAction action = menu_action(*row.meta, launcher::game_booted());
                if (action == MenuAction::Relaunch) {
                    rml += note_rml(kGlyphRestart, "restart",
                        fmt::format("{} restarts for a moment to apply this. You come straight back here.", product()));
                } else if (menu_apply(*row.meta) == MenuApply::None) {
                    rml += note_rml(kGlyphSchedule, "next-launch", fmt::format("Used each time {} starts.", product()));
                }
                auto& s = settings();
                if (var == &s.interface.liveBackground && s.interface.liveBackground.get() &&
                    !launcher::game_booted() && !disc_ready())
                {
                    rml += note_rml(kGlyphInfo, "info", "Shows once you choose a disc image.");
                }
            } else {
                if (restart_applies(row)) {
                    // The disc is only a restart row once the game has booted (restart_applies).
                    const auto pending = settings_restart_pending(true);
                    const bool isPending = std::ranges::find(pending, var) != pending.end();
                    rml += note_rml(kGlyphRestart, isPending ? "restart pending" : "restart",
                        isPending ? fmt::format("Takes effect after {} restarts.", product())
                                  : Rml::String{"Changes take effect after a restart."});
                }
                if (row.meta != nullptr && row.meta->apply == Apply::NextLaunch) {
                    rml += note_rml(kGlyphSchedule, "next-launch",
                        fmt::format("Takes effect the next time {} starts.", product()));
                }
            }
            if (row.spec.tier == Tier::Experimental) {
                rml += note_rml(kGlyphScience, "experimental", "Experimental: this may not work properly yet.");
            }
            return rml;
        });
        if (row.spec.extraHelp) {
            row.spec.extraHelp(pane);
        }
        if (row.var != nullptr && !row.defaultText.empty()) {
            pane.add_child<LiveRml>("help-default", [&row] {
                return var_modified(*row.var)
                           ? fmt::format("Default: {}", escape(row.defaultText))
                           : Rml::String{};
            });
            auto& reset = pane.add_child<ResetButton>(*row.var, var_modified);
            reset.on_pressed([&row] {
                row.var->reset();
                play_sound(kSoundItemChange);
                row.window->mPendingRowFocus = &row;
            });
        }
    }

private:
    SettingsWindow& mWindow;
    Pane& mLeft;
    Pane& mRight;
    Rml::String mPendingSection;
};

// The Data Folder row, the same on PRELAUNCH and TOOLS: the short path as its value, and in the
// help pane the full path and "Open Folder" (pressing the row opens it too).
void add_data_folder_row(RowBuilder& rows) {
    rows.add_action(
        {
            .label = "Data Folder",
            .help = fmt::format("Where {} keeps your settings, saves, controller layouts and logs.", product()),
            .extraHelp =
                [](Pane& pane) {
                    pane.add_rml(fmt::format("<file-path>{}</file-path>", path_rml(data_folder())));
                    auto& open = pane.add_button("Open Folder");
                    open.root()->SetClass("compact", true);
                    open.on_pressed([] {
                        if (open_folder(data_folder())) {
                            play_sound(kSoundClick);
                        }
                    });
                },
        },
        [] { open_folder(data_folder()); },
        [] {
            static const Rml::String sShort = abbreviate_path(data_folder());
            return sShort;
        });
}

// ---- The window ----

SettingsWindow::SettingsWindow(bool prelaunch)
    : Window(Props{.styleSheets = {"res/rml/settings.rcss"}}), mPrelaunch(prelaunch),
      mTab(std::make_unique<TabState>()) {
    mRoot->SetClass("settings", true);
    next_launch_snapshot(); // taken on first open
    // On open, on every tab and after a relaunch (restore) the pointer rests wherever it was, or
    // the X server centres it: a row laid out under it must neither look hovered nor take the help
    // pane from the keyboard / pad focus until the pointer really moves (SCENE 9: the pointer is
    // inactive during pad or keyboard navigation). Mouseover only reaches the rows
    // (Pane::register_control) through this capture.
    const auto pointerMoved = [this](Rml::Event& event) {
        if (!mHoverHold) {
            return true;
        }
        const Rml::Vector2f at(event.GetParameter("mouse_x", 0.0f), event.GetParameter("mouse_y", 0.0f));
        if (!mHoverAnchorSet) {
            mHoverAnchorSet = true;
            mHoverAnchor    = at;
        }
        const Rml::Vector2f d = at - mHoverAnchor;
        if (d.x * d.x + d.y * d.y < 24.0f * 24.0f) {
            return false;
        }
        set_hover_hold(false);
        return true;
    };
    listen(mRoot, Rml::EventId::Mouseover, [pointerMoved](Rml::Event& event) {
        if (!pointerMoved(event)) {
            event.StopImmediatePropagation();
        }
    }, true);
    listen(mRoot, Rml::EventId::Mousemove, [pointerMoved](Rml::Event& event) { pointerMoved(event); }, true);
    set_hover_hold(true);

    const auto tab = [this](const char* title, void (SettingsWindow::*build)(Rml::Element*)) {
        mTabTitles.emplace_back(title);
        add_tab(title, [this, build, title](Rml::Element* content) {
            mTab = std::make_unique<TabState>();
            if (mActiveTab != title) {
                set_hover_hold(true); // a new list under a resting pointer (a rebuild keeps it as it is)
            }
            mActiveTab = title;
            (this->*build)(content);
        });
    };
    if (prelaunch) {
        tab("Prelaunch", &SettingsWindow::build_prelaunch);
    }
    tab("Video", &SettingsWindow::build_video);
    tab("Input", &SettingsWindow::build_input);
    tab("Audio", &SettingsWindow::build_audio);
    tab("Gameplay", &SettingsWindow::build_gameplay);
    tab("Cheats", &SettingsWindow::build_cheats);
    tab("Interface", &SettingsWindow::build_interface);
    tab("Online", &SettingsWindow::build_online);
    tab("Tools", &SettingsWindow::build_tools);

    // Restart banner (DESIGN 5.10), after the content so the window's column puts it at the bottom.
    mBanner = append(mRoot, "restart-banner");
    append_text(append(mBanner, "banner-icon"), kGlyphRestart);
    mBannerText = append(mBanner, "banner-text");
    set_text_content(mBannerText, fmt::format("Some changes take effect after {} restarts.", product()));
    auto later = std::make_unique<Button>(mBanner, "Later");
    later->root()->SetClass("compact", true);
    later->on_pressed([this] {
        play_sound(kSoundClick);
        mBannerDismissed = true;
        focus_from_banner();
    });
    auto now = std::make_unique<Button>(mBanner, "Restart Now");
    now->root()->SetClass("compact", true);
    now->root()->SetClass("confirm", true);
    now->on_pressed([this] {
        play_sound(kSoundClick);
        restart_now();
    });
    mBannerButtons.push_back(std::move(later));
    mBannerButtons.push_back(std::move(now));
    set_display(mBanner, Rml::Style::Display::None);

    // Y on a controller (and the key bound to it, Q by default) resets the focused row to its
    // default, like the help pane's "Reset to Default" (DESIGN 5.17). R stays the R trigger's key
    // (the next tab), as every bound key drives the menus like the pad button it stands for.
    listen(Rml::EventId::Keydown, [this](Rml::Event& event) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
        const bool modifier = event.GetParameter<int>("ctrl_key", 0) ||
                              event.GetParameter<int>("alt_key", 0) ||
                              event.GetParameter<int>("meta_key", 0);
        if (key == input::kResetKey && !modifier && visible() && active() &&
            reset_row_of(event.GetTargetElement()))
        {
            event.StopPropagation();
        }
    });

    // Turning Advanced Settings on or off shows or hides rows: rebuild the tab after this frame's
    // event handling (never from inside the toggle's own handler).
    mConnections.push_back(settings().interface.advancedSettings.on_change([this] { request_rebuild(); }));
    // A graphics backend that resolves to the one already running is re-based by live_apply.
}

SettingsWindow::~SettingsWindow() = default;

SettingsWindow::Location SettingsWindow::location() const {
    Location loc;
    loc.tab = mActiveTab;
    if (!mContentComponents.empty()) {
        loc.scroll = mContentComponents.front()->root()->GetScrollTop();
    }
    Rml::Element* focused = mLastFocus.get();
    if (auto* context = mDocument != nullptr ? mDocument->GetContext() : nullptr) {
        if (Rml::Element* f = context->GetFocusElement(); f != nullptr && owns_element(f)) {
            focused = f;
        }
    }
    for (const auto& row : mTab->rows) {
        if (focused != nullptr && row->control->contains(focused)) {
            // An action row (Reset All Settings, Save Manager...) has no key: its label stands in.
            loc.key = row->var != nullptr ? std::string(row->var->key()) : "row:" + std::string(row->spec.label);
        }
    }
    return loc;
}

void SettingsWindow::restore(const std::string& tab, const std::string& key, float scroll) {
    mInitialOpen = false; // no open sound
    mRoot->SetClass("resume", true);
    mResumeFrames = 4;
    for (size_t i = 0; i < mTabTitles.size(); i++) {
        if (mTabTitles[i] == tab && mActiveTab != tab) {
            set_active_tab(static_cast<int>(i));
        }
    }
    bool focused = false;
    for (const auto& row : mTab->rows) {
        const bool match = row->var != nullptr ? row->var->key() == key : "row:" + std::string(row->spec.label) == key;
        if (!focused && !key.empty() && match) {
            focused = row->control->focus();
        }
    }
    if (auto* context = mDocument->GetContext(); context != nullptr && context->GetFocusElement() != nullptr &&
                                                  owns_element(context->GetFocusElement()))
    {
        mLastFocus = context->GetFocusElement()->GetObserverPtr();
        // The ring too: RmlUi sends no focus event to an element that already had focus (taken
        // without the visible flag while the tab was built), so it would stay without one until
        // the next key press.
        context->GetFocusElement()->SetPseudoClass("focus-visible", true);
    }
    set_hover_hold(true);
    mRestoreScrollTop = scroll;
    mRestoreScrollFrames = 3;
    std::fprintf(stderr, "mppm: settings: restored tab=%s key=%s focused=%d\n", mActiveTab.c_str(), key.c_str(),
        focused ? 1 : 0);
}

void SettingsWindow::set_hover_hold(bool hold) {
    mHoverHold = hold;
    if (hold) {
        mHoverAnchorSet = false;
    }
    mRoot->SetClass("pointer-idle", hold);
}

void SettingsWindow::focus_from_banner() {
    // Back to the control the banner was reached from, else the list, else the tab.
    if (mBannerReturn && mBannerReturn->IsVisible(true) && mBannerReturn->Focus(true)) {
        mBannerReturn->ScrollIntoView(Rml::ScrollIntoViewOptions{
            Rml::ScrollAlignment::Nearest,
            Rml::ScrollAlignment::Nearest,
            Rml::ScrollBehavior::Smooth,
            Rml::ScrollParentage::Closest,
        });
        return;
    }
    if (!mContentComponents.empty() && mContentComponents.front()->focus()) {
        return;
    }
    Window::focus();
}

void SettingsWindow::request_rebuild() {
    mRebuildPending = true;
}

void SettingsWindow::update() {
    if (mRebuildPending) {
        mRebuildPending = false;
        // Keep focus on the row with the same label (usually the toggle that caused this).
        Rml::String focusedLabel;
        auto* focused = mDocument->GetContext() != nullptr ? mDocument->GetContext()->GetFocusElement() : nullptr;
        for (const auto& row : mTab->rows) {
            if (row->control->contains(focused)) {
                focusedLabel = row->spec.label;
            }
        }
        if (!mContentComponents.empty()) {
            mRestoreScrollTop = mContentComponents.front()->root()->GetScrollTop();
        }
        refresh_active_tab();
        mPendingRowFocus = nullptr; // its row is gone
        bool refocused = false;
        for (const auto& row : mTab->rows) {
            if (!focusedLabel.empty() && row->spec.label == focusedLabel) {
                refocused = row->control->focus();
            }
        }
        if (refocused || focusedLabel.empty()) {
            // The new rows are laid out during the next frames: put the list back where it was.
            mRestoreScrollFrames = 2;
        } else if (!mContentComponents.empty()) {
            // The focused row went away (e.g. "Show Advanced Settings"): land on the first
            // advanced row that was changed, else the first advanced row, else the first row, and
            // bring it into view once the new list is laid out.
            const Row* target = nullptr;
            for (const auto& row : mTab->rows) {
                if (row->spec.tier != Tier::Normal && row->var != nullptr && var_modified(*row->var)) {
                    target = row.get();
                    break;
                }
            }
            for (const auto& row : mTab->rows) {
                if (target == nullptr && row->spec.tier != Tier::Normal) {
                    target = row.get();
                }
            }
            if (target != nullptr ? target->control->focus() : mContentComponents.front()->focus()) {
                if (auto* context = mDocument->GetContext(); context != nullptr && context->GetFocusElement()) {
                    mScrollTo = context->GetFocusElement()->GetObserverPtr();
                    mScrollToFrames = 2;
                }
            }
        }
    }
    if (mPendingRowFocus != nullptr) {
        // "Reset to Default" was pressed: back to its row, which is no longer selected.
        const Row* pendingRow = std::exchange(mPendingRowFocus, nullptr);
        for (const auto& row : mTab->rows) {
            if (row.get() == pendingRow && row->control->focus()) {
                row->pane->set_selected_item(-1);
            }
        }
    }
    {
        std::optional<std::string> picked;
        {
            std::lock_guard lk(sPickMutex);
            picked.swap(sPickedDisc);
        }
        if (picked && !settings().backend.discPath.locked()) {
            auto& backend = settings().backend;
            if (*picked != backend.discPath.get()) {
                backend.discPath.set(*picked);
                // A new file has not been verified (SETTINGS 2, Disc Image).
                backend.discVerification.reset();
                backend.discFingerprint.reset();
                play_sound(kSoundItemChange);
            }
        }
    }
    Window::update();
    if (auto* context = mDocument->GetContext()) {
        Rml::Element* focused = context->GetFocusElement();
        if (focused != nullptr && focused != mDocument && owns_element(focused)) {
            mLastFocus = focused->GetObserverPtr();
            // Where leaving the restart banner goes back to (Up, Later), whichever way it was reached.
            if (focused->Closest("content") != nullptr) {
                mBannerReturn = focused->GetObserverPtr();
            }
        }
    }
    if (mRestoreScrollFrames > 0 && !mContentComponents.empty()) {
        mRestoreScrollFrames--;
        mContentComponents.front()->root()->SetScrollTop(mRestoreScrollTop);
    }
    if (mScrollToFrames > 0) {
        --mScrollToFrames;
        if (mScrollTo) {
            mScrollTo->ScrollIntoView(Rml::ScrollIntoViewOptions{
                mScrollToNearest ? Rml::ScrollAlignment::Nearest : Rml::ScrollAlignment::Center,
                Rml::ScrollAlignment::Nearest,
                Rml::ScrollBehavior::Instant,
                Rml::ScrollParentage::Closest,
            });
        }
        if (mScrollToFrames == 0) {
            mScrollToNearest = false; // after the last frame's scroll, which must still use it
        }
    }
    if (mResumeFrames > 0 && --mResumeFrames == 0) {
        // Laid out at its place: the close transition (which hides the document) is needed again.
        mRoot->SetClass("resume", false);
        // Showing the window moved focus to its body: put it back on the restored row, with its
        // ring (Focus(true)), so a keyboard or gamepad user sees where they are at once.
        if (auto* context = mDocument->GetContext(); context != nullptr && mLastFocus && mLastFocus->IsVisible(true)) {
            Rml::Element* row = mLastFocus.get();
            if (context->GetFocusElement() != row) {
                row->Focus(true);
            }
            row->SetPseudoClass("focus-visible", true);
        }
    }
    update_rows();
    update_banner();
    // The "N advanced options changed" footer counts hidden rows at build time: rebuild the tab
    // when that count goes stale (Reset All Settings, Choose Preset, a hotkey).
    if (!mRebuildPending && !mTab->hiddenAdvanced.empty()) {
        const auto changed = std::ranges::count_if(mTab->hiddenAdvanced, [](const VarBase* var) { return var_modified(*var); });
        if (changed != mTab->hiddenAdvancedChanged) {
            request_rebuild();
        }
    }
}

void SettingsWindow::update_rows() {
    const auto pending = settings_restart_pending(launcher::game_booted());
    for (const auto& row : mTab->rows) {
        unsigned mask = 0;
        if (row->var != nullptr) {
            if (row->var->locked()) {
                mask |= kBadgeLock;
            }
            if (mPrelaunch) {
                // On the start menu nothing waits for a restart (live_options_design 3.9): a row
                // being applied says so instead.
                if (launcher::live::busy(*row->var)) {
                    mask |= kBadgeApplying;
                }
            } else {
                if (std::ranges::find(pending, row->var) != pending.end()) {
                    mask |= kBadgeRestart;
                }
                if (row->meta != nullptr && row->meta->apply == Apply::NextLaunch &&
                    next_launch_pending(*row->var))
                {
                    mask |= kBadgeNextLaunch;
                }
            }
        }
        if (row->spec.tier == Tier::Experimental) {
            mask |= kBadgeExperimental;
        }
        if (mask == row->badgeMask) {
            continue;
        }
        row->badgeMask = mask;
        // Priority order (SETTINGS 1.5): lock, restart required, next launch, experimental.
        struct BadgeInfo {
            Badge bit;
            const char* cls;
            const char* glyph;
            const char* text;
        };
        static constexpr BadgeInfo kBadges[] = {
            {kBadgeLock, "lock", kGlyphLock, "Locked"},
            {kBadgeApplying, "applying", kGlyphApplying, "Applying…"},
            {kBadgeRestart, "restart", kGlyphRestart, "Restart required"},
            {kBadgeNextLaunch, "next-launch", kGlyphSchedule, "Next launch"},
            {kBadgeExperimental, "experimental", kGlyphScience, "Experimental"},
        };
        Rml::String rml;
        for (const auto& badge : kBadges) {
            if ((mask & badge.bit) != 0) {
                rml += fmt::format("<row-badge class=\"{}\"><badge-icon>{}</badge-icon><badge-text>{}</badge-text></row-badge>",
                    badge.cls, badge.glyph, badge.text);
            }
        }
        row->badges->SetInnerRML(rml);
    }
}

void SettingsWindow::update_banner() {
    // Never on the start menu: changes there apply at once or through a relaunch (3.9).
    const size_t count = mPrelaunch ? 0 : settings_restart_pending(launcher::game_booted()).size();
    if (count > mBannerPendingCount) {
        mBannerDismissed = false; // a new change brings "Later" back
    }
    mBannerPendingCount = count;
    const bool show = count > 0 && !mBannerDismissed;
    const bool shown = mBanner->HasAttribute("visible");
    if (show != shown) {
        if (show) {
            mBanner->SetAttribute("visible", "");
            set_display(mBanner, Rml::Style::Display::Flex);
            // The banner takes the bottom of the window and the list gets shorter: the row just
            // changed (usually the one that brought the banner) must not end up behind it.
            if (auto* focused = mDocument->GetContext()->GetFocusElement();
                focused != nullptr && focused->Closest("content") != nullptr && owns_element(focused))
            {
                mScrollTo = focused->GetObserverPtr();
                mScrollToFrames = 2;
                mScrollToNearest = true;
            }
        } else {
            // Don't leave focus on a button that disappears.
            for (const auto& button : mBannerButtons) {
                if (button->contains(mDocument->GetContext()->GetFocusElement())) {
                    focus_from_banner();
                }
            }
            mBanner->RemoveAttribute("visible");
            set_display(mBanner, Rml::Style::Display::None);
        }
    }
    for (const auto& button : mBannerButtons) {
        button->update();
    }
}

void SettingsWindow::restart_now() {
    if (mPrelaunch) {
        // Nobody is playing: straight back into the start menu (SETTINGS 11).
        launcher::request_restart(launcher::RestartTarget::StartMenu);
        return;
    }
    const auto dismiss = [](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = fmt::format("Restart {}?", product()),
        .bodyText = fmt::format("Some changes take effect after {} restarts. Progress since your last save "
                                "will be lost.",
            product()),
        .actions =
            {
                {.label = "Later", .onPressed = dismiss},
                {.label = "Restart Now",
                    .onPressed = [](Modal&) { launcher::request_restart(launcher::RestartTarget::SameAsNow); }},
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
    if (auto* doc = top_document()) {
        doc->focus();
    }
}

bool SettingsWindow::consume_close_request() {
    // One question on the way out when restart-only changes are waiting and the banner was not
    // already answered with "Later" (SETTINGS 11: a footer banner and one modal).
    if (mPrelaunch || mCloseAsked || mBannerDismissed || settings_restart_pending(launcher::game_booted()).empty()) {
        return false;
    }
    mCloseAsked = true;
    // Leaving: Escape / B mean "Later" and close Settings, as the question is only about the
    // restart. Restart Now restarts at once; this dialog already says what restarting costs.
    const auto later = [this](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
        request_close();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = fmt::format("Restart {}?", product()),
        .bodyText = fmt::format("Some changes take effect after {} restarts. Progress since your last save "
                                "will be lost.",
            product()),
        .actions =
            {
                {.label = "Later", .onPressed = later},
                {.label = "Restart Now",
                    .onPressed =
                        [](Modal&) { launcher::request_restart(launcher::RestartTarget::SameAsNow); }},
            },
        .onDismiss = later,
        .icon = "warning",
    }));
    if (auto* doc = top_document()) {
        doc->focus();
    }
    return true;
}

void SettingsWindow::uncover() {
    if (mWasVisible) {
        Window::uncover();
        return;
    }
    // A modal on top closed: focus returns to the control that opened it.
    if (mLastFocus && mLastFocus->IsVisible(true) && mLastFocus->Focus(true)) {
        return;
    }
    Window::uncover();
}

void SettingsWindow::hide(bool close) {
    borealis::config::Registry::global().flush();
    Window::hide(close);
}

bool SettingsWindow::reset_row_of(Rml::Element* target) {
    // Typing in a field: R is a character, not the shortcut.
    if (target == nullptr || target->GetTagName() == "input") {
        return false;
    }
    for (const auto& row : mTab->rows) {
        if (!row->control->contains(target)) {
            continue;
        }
        VarBase* var = row->var;
        if (var == nullptr || var->locked() || !var_modified(*var) ||
            (row->spec.isDisabled && row->spec.isDisabled()))
        {
            return false;
        }
        var->reset();
        play_sound(kSoundItemChange);
        return true;
    }
    return false;
}

bool SettingsWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    auto* target = event.GetTargetElement();
    const bool bannerVisible = mBanner->HasAttribute("visible");
    const bool inBanner = target != nullptr && target->Closest("restart-banner") != nullptr;
    Pane* rows = nullptr;
    Pane* help = nullptr;
    if (mContentComponents.size() >= 2) {
        rows = dynamic_cast<Pane*>(mContentComponents[0].get());
        help = dynamic_cast<Pane*>(mContentComponents[1].get());
    }
    if (inBanner) {
        if (cmd == NavCommand::Up) {
            // Back to the control Down came from, else the last row.
            bool moved = false;
            if (mBannerReturn && mBannerReturn->IsVisible(true) && mBannerReturn->Focus(true)) {
                mBannerReturn->ScrollIntoView(Rml::ScrollIntoViewOptions{
                    Rml::ScrollAlignment::Nearest,
                    Rml::ScrollAlignment::Nearest,
                    Rml::ScrollBehavior::Smooth,
                    Rml::ScrollParentage::Closest,
                });
                moved = true;
            } else if (rows != nullptr) {
                moved = rows->focus_last();
            }
            if (moved) {
                play_sound(kSoundItemFocus);
            }
            return true;
        }
        if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
            const size_t next = cmd == NavCommand::Right ? 1 : 0;
            if (!mBannerButtons[next]->contains(target) && mBannerButtons[next]->focus()) {
                play_sound(kSoundItemFocus);
            }
            return true;
        }
        if (cmd == NavCommand::Down) {
            return true;
        }
    } else if (bannerVisible && cmd == NavCommand::Down && target != nullptr && target->Closest("content") != nullptr) {
        // Past the last row: on to the banner's "Restart Now".
        if (mBannerButtons.back()->focus()) {
            mBannerReturn = target->GetObserverPtr();
            play_sound(kSoundItemFocus);
            return true;
        }
    }

    // Inside the two panes, Left and Right only move between a row and its help pane's buttons;
    // they never fall through to the tab bar (the tabs have their own keys: L1/R1, PageUp/Down).
    if (rows != nullptr && help != nullptr && target != nullptr) {
        if (rows->contains(target)) {
            switch (cmd) {
            case NavCommand::Left:
                return true;
            case NavCommand::Right:
                // The help pane may show the row under the mouse pointer: show the focused row's
                // help before moving into it.
                for (const auto& row : mTab->rows) {
                    if (row->control->contains(target)) {
                        help->clear();
                        RowBuilder::build_help(*help, *row);
                    }
                }
                // The pane marked the row selected on the way: undo that when there is nothing
                // to move to.
                if (help->focus()) {
                    play_sound(kSoundItemFocus);
                } else {
                    rows->set_selected_item(-1);
                }
                return true;
            case NavCommand::Confirm:
                return true; // a row that ignores Confirm (disabled, read-only) stays put
            default:
                break;
            }
        } else if (help->contains(target)) {
            switch (cmd) {
            case NavCommand::Up:
            case NavCommand::Down:
            case NavCommand::Right:
            case NavCommand::Confirm:
                return true; // the pane already moved between its buttons where it could
            case NavCommand::Left:
            case NavCommand::Cancel:
                // Back to the row the help belongs to.
                if (rows->focus()) {
                    rows->set_selected_item(-1);
                    play_sound(kSoundItemFocus);
                }
                return true;
            default:
                break;
            }
        }
    }
    return Window::handle_nav_command(event, cmd);
}

// ---- Tabs ----

void SettingsWindow::build_prelaunch(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("Game");
    rows.add_action(
        {
            .label = "Disc Image",
            .help = fmt::format("The disc image {} plays from: your own copy of Pikmin 2 for "
                                "GameCube (USA). Nothing in it is copied or changed.<br/><br/>"
                                "Formats: {}.",
                product(), disc::kFormatList),
            .isDisabled = [] { return settings().backend.discPath.locked(); },
            .extraHelp =
                [](Pane& pane) {
                    pane.add_child<LiveRml>("help-status", [] {
                        const std::string& path = settings().backend.discPath.get();
                        if (path.empty()) {
                            return Rml::String{};
                        }
                        const disc::Info& info = inspect_disc(path);
                        return fmt::format("<status-line class=\"{}\">{}</status-line><file-path>{}</file-path>",
                            info.state == disc::State::Ready ? "good" : "bad",
                            escape(disc::state_label(info.state)), path_rml(abbreviate_path(path)));
                    });
                },
        },
        [] { pick_disc(); },
        [] {
            const std::string& path = settings().backend.discPath.get();
            if (path.empty()) {
                return Rml::String{"(none)"};
            }
            return Rml::String{std::filesystem::path(path).filename().string()};
        },
        &s.backend.discPath); // a disc changed after the game booted needs a restart
    rows.external_action(
        {
            .label = "Verify Disc",
            .help = fmt::format("Reads the whole disc and compares it with a known good copy. This can "
                                "take a while on a slow drive. {} works without it, but a bad dump can "
                                "cause crashes that look like bugs.",
                product()),
            .isDisabled = [] { return !disc_ready(); },
            .disabledNote = "Choose a working disc image first.",
        },
        SettingsAction::VerifyDisc);
    rows.external_action(
        {
            .label = "Save Manager",
            .help = "Import, export, back up and delete your Pikmin 2 saves.",
            .isDisabled = [] { return !disc_ready(); },
            .disabledNote = "Choose a working disc image first.",
        },
        SettingsAction::SaveManager);

    rows.section("System");
    add_data_folder_row(rows);
    rows.choices(s.backend.graphicsBackend, backend_choices(),
        {
            .label = "Graphics Backend",
            .help = fmt::format("How {} talks to your graphics card. Leave this on Automatic unless you "
                                "see graphical problems or a crash at startup.",
                product()),
            .extraHelp =
                [](Pane& pane) {
                    pane.add_child<LiveRml>("help-notes", [] {
                        const BackendInfo* failed = failed_backend();
                        const char* running = backend_name(aurora_get_backend());
                        if (failed == nullptr || running == nullptr ||
                            settings().backend.graphicsBackend.get() != failed->id)
                        {
                            return Rml::String{};
                        }
                        return note_rml(kGlyphWarning, "warning",
                            fmt::format("{} could not start on this computer, so {} is using {}. Choose "
                                        "Automatic to stop trying it.",
                                failed->name, product(), running));
                    });
                },
        },
        [choices = backend_choices()] {
            // An unknown or unavailable id falls back to Automatic without rewriting the file.
            const std::string& id = settings().backend.graphicsBackend.get();
            for (size_t i = 0; i < choices.size(); i++) {
                if (choices[i].value == id) {
                    return static_cast<int>(i);
                }
            }
            return 0;
        });
    rows.choices(s.backend.cardFileType,
        {{CardFileType::GciFolder, "GCI Folder (recommended)"}, {CardFileType::CardImage, "Memory Card Image"}},
        {
            .label = "Save Format",
            .help = "How saves are stored on disk. GCI Folder keeps one file per save and works directly "
                    "with Dolphin. Memory Card Image keeps a single .raw card file.<br/><br/>Changing this "
                    "does not move your saves; use the Save Manager to import them.",
            .tier = Tier::Advanced,
        });
}

// The live note under the Frame Rate help: VSync holds the picture to the display's refresh.
static Rml::String frame_rate_vsync_note() {
    const auto& v = settings().video;
    const float hz = video::display_refresh_hz();
    if (!v.vsync.get() || hz <= 0.0f) {
        return {};
    }
    const std::string& rate = v.frameRate.get();
    const int n = frame_rate_number(rate);
    if (rate != "unlimited" && static_cast<float>(n) <= hz + 0.5f) {
        return {};
    }
    return note_rml(kGlyphInfo, "info",
        fmt::format("VSync is on, so no more than your display's {:.0f} Hz is shown.", hz));
}

void SettingsWindow::build_video(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("Display");
    rows.toggle(s.video.fullscreen,
        {.label = "Fullscreen", .help = "Fill the whole screen. You can also press F11 or Alt+Enter at any time."});
    std::vector<Choice<AspectRatio>> aspects = {
        {AspectRatio::Auto, "Automatic (fill the window)"},
        {AspectRatio::Original, "Original (4:3)"},
    };
    if (kWideAspects) {
        aspects.push_back({AspectRatio::Ratio16x10, "16:10"});
        aspects.push_back({AspectRatio::Ratio16x9, "16:9"});
        aspects.push_back({AspectRatio::Ratio21x9, "21:9"});
    }
    aspects.push_back({AspectRatio::Stretch, "Stretch to Window"});
    rows.choices(s.video.aspectRatio, std::move(aspects),
        {
            .label = "Aspect Ratio",
            .help = "Automatic shows more of the world on wide screens without stretching anything. "
                    "Original keeps the picture exactly as on a GameCube, with bars on wide screens. "
                    "Stretch fills the window and makes everything wider than intended.",
        });
    rows.toggle(s.video.vsync,
        {
            .label = "VSync",
            .help = "Match the picture to your monitor's refresh rate to avoid tearing. Turn it off only "
                    "if you notice input delay. The game's speed does not change either way.",
        });
    rows.choices(s.video.frameRate, frame_rate_choices(),
        {
            .label = "Frame Rate",
            .help = fmt::format("How many pictures {0} shows each second. Original keeps the game as on a "
                                "GameCube: 30 in the field, 60 in menus. Higher settings make the start "
                                "menu and {0}'s own menus smoother. In the field the game still shows 30 "
                                "pictures a second unless Smooth Motion is on. The game's speed never "
                                "changes.",
                product()),
            .extraHelp =
                [](Pane& pane) { pane.add_child<LiveRml>("help-notes", [] { return frame_rate_vsync_note(); }); },
        },
        [choices = frame_rate_choices()] {
            // An unknown value shows as Original without rewriting the file.
            const std::string& value = settings().video.frameRate.get();
            for (size_t i = 0; i < choices.size(); i++) {
                if (choices[i].value == value) {
                    return static_cast<int>(i);
                }
            }
            return 0;
        });
    // fps_design 3.3 / 7: in-between pictures in the field and caves. Experimental until the
    // remaining parts move smoothly too (particles, shadows, HUD; fps_design 7.6, 3b-3e).
    rows.toggle(s.video.smoothMotion,
        {
            .label = "Smooth Motion",
            .help = "Draws in-between pictures so the game moves at up to the Frame Rate. The game's "
                    "speed and rules do not change. Characters and the camera move smoothly; "
                    "particles, shadows and the HUD still change 30 times a second. Off during "
                    "cutscenes, pauses and two-player games. May cause small visual glitches.",
            .tier = Tier::Experimental,
            .isDisabled = [] { return settings().video.frameRate.get() == "original"; },
            .disabledNote = "Choose a Frame Rate above Original first.",
        });
    rows.add_action(
        {
            .label = "Reset Window Size",
            .help = "Return to a 1280×960 window in the middle of the screen.",
            .isDisabled = [] { return settings().video.fullscreen.locked(); },
        },
        [] { video::reset_window_size(); });

    rows.section("Quality");
    rows.stepper(s.video.internalResolution, range_values(0, 8, 1), internal_resolution_label,
        {
            .label = "Internal Resolution",
            .help = "The resolution the game is drawn at. Auto matches your window and looks sharpest. "
                    "Lower values run faster on slow graphics hardware; 1x is the original GameCube "
                    "resolution.",
        });
    rows.choices(s.video.resampler,
        {{Resampler::Bilinear, "Smooth (Bilinear)"}, {Resampler::Area, "Sharp Downscale (Area)"}},
        {
            .label = "Output Scaling",
            .help = "How the picture is resized to fit the window. Smooth suits most setups. Sharp "
                    "Downscale looks cleaner when Internal Resolution is higher than your window.",
        });
    rows.choices(s.video.anisotropy, {{1, "Original"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}},
        {
            .label = "Texture Filtering",
            .help = "Keeps ground and wall textures sharp at shallow angles. Costs almost nothing on "
                    "modern graphics cards.",
        });

    rows.section("Overlay");
    rows.choices(s.video.fpsCounter,
        {{FpsCounter::Off, "Off"}, {FpsCounter::TopLeft, "Top Left"}, {FpsCounter::TopRight, "Top Right"},
            {FpsCounter::BottomLeft, "Bottom Left"}, {FpsCounter::BottomRight, "Bottom Right"}},
        {.label = "FPS Counter", .help = "Show the current frame rate in a corner of the screen."});

    rows.section("Advanced");
    rows.toggle(s.video.allowCpuAdapter,
        {
            .label = "Allow Software Rendering",
            .help = fmt::format("Let {} run on a CPU renderer when no graphics card is available. Very "
                                "slow; meant for testing.",
                product()),
            .tier = Tier::Advanced,
        });
    rows.choices(s.video.msaa, {{1, "Off"}, {4, "4x"}},
        {
            .label = "Anti-Aliasing (MSAA)",
            .help = "Smooths jagged edges. Character shadows may render incorrectly with it on. "
                    "Raising Internal Resolution is the safer way to get a cleaner picture.",
            .tier = Tier::Experimental,
        });
}

void SettingsWindow::build_input(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("Controllers");
    rows.external_action(
        {
            .label = "Configure Controllers",
            .help = "Choose which controller or keyboard each player uses, and change every button.",
        },
        SettingsAction::ConfigureControllers);
    rows.toggle(s.input.rumble,
        {
            .label = "Rumble",
            .help = "Controller vibration when a captain takes damage and during big moments. This is "
                    "the same option as in the game's own Options screen.",
        });
    rows.toggle(s.input.playerLed,
        {
            .label = "Player Colour Light",
            .help = "Light up controllers that have a light bar in the player's colour: red for Player "
                    "1, blue for Player 2.",
        });

    rows.section("General");
    rows.toggle(s.input.backgroundInput,
        {
            .label = "Input While in Background",
            .help = fmt::format("Keep reading your controller when the {} window is not focused. "
                                "Keyboard input always stops when you switch away.",
                product()),
        });
    rows.info(
        {
            .label = "Menu Button",
            .help = fmt::format("Open the {} menu with F1 on a keyboard, or Back (or R + Start) on a "
                                "controller.",
                product()),
        },
        "F1 / Back / R + Start");
}

void SettingsWindow::build_audio(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("Volume");
    rows.number(s.audio.masterVolume, {.min = 0, .max = 100, .step = 5, .suffix = "%"},
        {.label = "Master Volume", .help = fmt::format("Overall loudness of everything {} plays.", product())});
    rows.number(s.audio.musicVolume, {.min = 0, .max = 100, .step = 5, .suffix = "%"},
        {.label = "Music Volume", .help = "Loudness of the background music."});
    rows.number(s.audio.effectsVolume, {.min = 0, .max = 100, .step = 5, .suffix = "%"},
        {.label = "Effects Volume", .help = "Loudness of sound effects and Pikmin voices."});

    rows.section("Output");
    rows.choices(s.audio.outputMode, {{SoundOutput::Stereo, "Stereo"}, {SoundOutput::Mono, "Mono"}},
        {
            .label = "Sound Output",
            .help = "Stereo for speakers and headphones. Mono plays the same sound on both sides.",
        });
    rows.toggle(s.audio.muteInBackground,
        {.label = "Mute in Background", .help = "Silence the game while you are using another window."});
    rows.toggle(s.audio.menuSounds,
        {.label = "Menu Sounds", .help = fmt::format("Play sounds when you move through {}'s menus.", product())});
    rows.toggle(s.audio.enabled,
        {
            .label = "Enable Audio",
            .help = "Turn the whole sound system off. Only useful for troubleshooting.",
            .tier = Tier::Advanced,
        });
}

void SettingsWindow::build_gameplay(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("General");
    rows.toggle(s.game.pauseInMenu,
        {
            .label = "Pause While Menu Is Open",
            .help = fmt::format("Freeze the game while the {} menu is open, so nothing happens to your "
                                "Pikmin while you change settings.",
                product()),
        });
    rows.toggle(s.game.pauseInBackground,
        {.label = "Pause in Background", .help = "Pause the game when you switch to another window."});

    rows.section("Startup");
    rows.toggle(s.game.skipBootLogos,
        {
            .label = "Skip Boot Logos",
            .help = "Go straight to the title screen instead of showing the Nintendo and Dolby logos "
                    "each time. Only matters when the start menu is skipped.",
        });

    rows.section("Title Screen");
    rows.toggle(s.game.idleDemo,
        {
            .label = "Demo Movies When Idle",
            .help = "After a while on the title screen, the game plays its demo movies. Turn this off "
                    "to stay on the title screen.",
        });
    rows.choices(s.game.titleSeason,
        {{TitleSeason::Original, "As Original"}, {TitleSeason::Spring, "Spring"}, {TitleSeason::Summer, "Summer"},
            {TitleSeason::Autumn, "Autumn"}, {TitleSeason::Winter, "Winter"}},
        {
            .label = "Season",
            .help = "The title screen changes with the seasons. Pick a favourite, or leave it to the game.",
        });
}

void SettingsWindow::build_cheats(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.notice(fmt::format("<note-icon>{}</note-icon><note-text>Cheats change how the game plays. "
                            "Progress you make with cheats on is saved like any other progress.</note-text>",
                    kGlyphWarning),
        "danger");

    rows.section("Modes");
    rows.toggle(s.cheat.unlockChallenge,
        {
            .label = "Unlock Challenge Mode",
            .help = "Offer Challenge Mode on the title menu right away, without finding the key in story "
                    "mode. Your save is not changed.",
        });

    rows.section("Time");
    rows.stepper(s.cheat.daySpeed, range_values(25, 400, 25, {0}), day_speed_label,
        {
            .label = "Day Speed",
            .help = "How fast the day passes. Below 100% gives you longer days; above makes them shorter. "
                    "At 0% the sun never sets, so end the day yourself from the pause menu.",
        });
    rows.toggle(s.cheat.sunsetOnDayOne,
        {
            .label = "Allow Sunset on Day 1",
            .help = "Let you choose Go to Sunset from the pause menu on the first day, which the game "
                    "normally forbids. Skipping the first day's tutorial steps can leave the story in an "
                    "odd state.",
        });

    rows.section("Captains");
    rows.toggle(s.cheat.invincibleCaptains,
        {
            .label = "Invincible Captains",
            .help = "Olimar, Louie and the President take no damage from enemies and hazards. Falling "
                    "into a pit still counts.",
        });
    rows.toggle(s.cheat.infiniteSprays,
        {
            .label = "Unlimited Sprays",
            .help = "Using a spray does not use it up. You still need to have found at least one of that "
                    "spray.",
        });
}

void SettingsWindow::build_interface(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();
    const auto liveOff = [] { return !settings().interface.liveBackground.get(); };
    const Rml::String liveOffNote = "Turn on Live Start Menu to choose this.";

    rows.section(product());
    // The stepper stops at 130%: above 4/3 of the window-fit base the layout would drop below the
    // 960x540 dp it holds at (ui.cpp max_ui_scale), on any window. 135-200 from an older config
    // still shows, with the size actually used.
    rows.stepper(s.interface.uiScale, range_values(75, kMenuSizeTop, 5, {0}), ui_scale_label,
        {
            .label = "Menu Size",
            .help = fmt::format("Size of {}'s own menus. 100% fits the menus to the window and grows "
                                "with it. This does not change the game's HUD.",
                product()),
            .extraHelp =
                [](Pane& pane) {
                    pane.add_child<LiveRml>("help-notes", [] {
                        const int max = std::max(max_ui_scale_percent(), 100);
                        if (settings().interface.uiScale.get() <= max) {
                            return Rml::String{};
                        }
                        // Only a very small window holds less than the stepper's top value.
                        return note_rml(kGlyphInfo, "info",
                            max < kMenuSizeTop
                                ? fmt::format("This window has room for {}% at most; menus grow further when "
                                              "the window does.",
                                      max)
                                : fmt::format("Menus are using {}%, the largest size their layout fits.", max));
                    });
                },
        });
    rows.toggle(s.interface.reduceMotion,
        {.label = "Reduce Motion", .help = "Replace sliding and bouncing menu animations with quick fades."});
    rows.toggle(s.interface.highContrast,
        {.label = "High Contrast", .help = "Solid panels and stronger outlines for easier reading."});
    rows.choices(s.interface.glassEffects,
        {{GlassEffects::Off, "Off"}, {GlassEffects::Blur, "Blur"}, {GlassEffects::Full, "Full"}},
        {.label = "Glass Effects", .help = fmt::format("How see-through {}'s panels are. Off is fastest.", product())});
    rows.toggle(s.backend.skipLauncher,
        {
            .label = "Skip Start Menu",
            .help = fmt::format("Start the game as soon as {} opens. You can still reach every setting "
                                "with F1 in game.",
                product()),
        });
    rows.toggle(s.interface.liveBackground,
        {
            .label = "Live Start Menu",
            .help = "Show Olimar and his Pikmin behind the start menu. Turn this off for a still backdrop, "
                    "which starts faster on slow computers.",
        });

    rows.section("Start Menu");
    rows.choices(s.interface.sceneArea,
        {{SceneArea::Forest, "Awakening Wood"}, {SceneArea::Tutorial, "Valley of Repose", kAllSceneAreas},
            {SceneArea::Last, "Wistful Wild", kAllSceneAreas},
            {SceneArea::Season, "Follow the Season", kAllSceneAreas}},
        {.label = "Location", .help = "Where Olimar's ship has landed behind the start menu.",
            .isDisabled = liveOff, .disabledNote = liveOffNote});
    rows.choices(s.interface.sceneTime,
        {{SceneTime::Morning, "Morning"}, {SceneTime::Day, "Day"}, {SceneTime::Evening, "Evening"},
            {SceneTime::Night, "Night"}, {SceneTime::Clock, "Follow the Clock"}},
        {.label = "Time of Day", .help = "The light behind the start menu. Follow the Clock matches the time on your computer.",
            .isDisabled = liveOff, .disabledNote = liveOffNote});
    rows.choices(s.interface.sceneMusic,
        {{SceneMusic::Piklopedia, "Piklopedia"}, {SceneMusic::WorldMap, "World Map"},
            {SceneMusic::Challenge, "Challenge Mode"}, {SceneMusic::HiScore, "High Scores"},
            {SceneMusic::Off, "Off"}},
        {.label = "Music", .help = "The tune that plays in the start menu, from the game's own soundtrack.",
            .isDisabled = liveOff, .disabledNote = liveOffNote});
    rows.choices(s.interface.launchAnimation,
        {{LaunchAnimation::Full, "On"}, {LaunchAnimation::Off, "Off"}},
        {.label = "Take-off",
            .help = "The take-off when you press Play. Press any button to skip it. Reduce Motion keeps "
                    "the cameras still.",
            .isDisabled = liveOff, .disabledNote = liveOffNote});

    rows.section("Updates");
    rows.toggle(s.interface.checkForUpdates,
        {
            .label = "Check for Updates",
            .help = fmt::format("Look for a new {0} release when the start menu opens. {0} only tells you "
                                "about it; it never downloads or installs anything. The check contacts "
                                "GitHub and sends no personal information.",
                product()),
        });
    // The row shows a result only once it was pressed: a quiet startup check that failed is not
    // the user's question.
    static bool sCheckNowPressed = false;
    rows.add_action(
        {
            .label = "Check Now",
            .help = fmt::format("Look for a new {} release now.", product()),
            .isDisabled = [] { return !action_handler(SettingsAction::CheckForUpdates); },
            .disabledNote = "Not available yet.",
        },
        [] {
            if (const auto& handler = action_handler(SettingsAction::CheckForUpdates)) {
                sCheckNowPressed = true;
                handler();
            }
        },
        [] { return sCheckNowPressed ? action_status(SettingsAction::CheckForUpdates) : Rml::String{}; });

    rows.section("Notifications");
    rows.toggle(s.interface.controllerToasts,
        {.label = "Controller Messages", .help = "Tell you when a controller connects or disconnects."});
    rows.toggle(s.interface.menuHint,
        {.label = "Menu Reminder", .help = "Show \"Press F1 for the menu\" for a moment when the game starts."});
    rows.toggle(s.interface.shaderIndicator,
        {
            .label = "Shader Activity",
            .help = "Show a small spinner while the graphics card prepares new effects. Short stutters "
                    "while it is visible are normal the first time you see something.",
        });

    rows.section("Game");
    rows.toggle(s.interface.hideHud,
        {
            .label = "Hide HUD",
            .help = "Hide the game's on-screen display: Pikmin counts, the sun meter and health. Handy for "
                    "screenshots.",
        });
    if (kWideAspects) {
        rows.choices(s.interface.hudLayout,
            {{HudLayout::Centered, "Centred"}, {HudLayout::Edges, "At the Edges"}, {HudLayout::Stretched, "Stretched"}},
            {
                .label = "HUD Layout",
                .help = "Where the HUD sits on wide screens. At the Edges keeps each part in its corner; "
                        "Centred keeps the original 4:3 layout in the middle.",
            });
        rows.choices(s.interface.hudMaxAspect,
            {{HudMaxAspect::Ratio16x9, "16:9"}, {HudMaxAspect::Ratio21x9, "21:9"}, {HudMaxAspect::Unlimited, "Unlimited"}},
            {
                .label = "HUD Width Limit",
                .help = "On very wide screens, keep the HUD within this shape so it stays in view.",
            });
    }

    rows.section("Advanced");
    rows.toggle(s.interface.advancedSettings,
        {
            .label = "Advanced Settings",
            .help = "Show extra options meant for troubleshooting and testing, including experimental "
                    "features that may not work properly.",
        });
}

void SettingsWindow::build_online(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();

    rows.section("You");
    rows.text(s.online.playerName, "Player", 24, "Player",
        {
            .label = "Player Name",
            .help = "Shown to the other players in online games.",
        });

    rows.section("Server");
    const Rml::String builtIn = default_server_url();
    rows.text(s.online.serverUrl, "None", 256, builtIn.empty() ? Rml::String{"none yet"} : builtIn,
        {
            .label = "Lobby Server",
            .help = "The server that runs lobbies and join codes. Every relay address comes from it. Use your "
                    "own to play on a self-hosted server.",
            .extraHelp =
                [](Pane& pane) {
                    // While no default exists the row says so even before anything was typed.
                    pane.add_child<LiveRml>("help-default", [] {
                        return default_server_url()[0] == '\0' && !settings().online.serverUrl.modified()
                                   ? Rml::String{"Default: none yet"}
                                   : Rml::String{};
                    });
                },
        });
}

bool SettingsWindow::show_tab(const std::string& title) {
    const auto it = std::ranges::find(mTabTitles, title);
    if (it == mTabTitles.end() || !set_active_tab(static_cast<int>(it - mTabTitles.begin()))) {
        return false;
    }
    if (!mContentComponents.empty()) {
        mContentComponents.front()->focus();
    }
    return true;
}

void SettingsWindow::build_tools(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Controlled);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    RowBuilder rows(*this, left, right);
    auto& s = settings();
    const bool prelaunch = mPrelaunch;

    rows.section("Saves");
    rows.external_action(
        {
            .label = "Save Manager",
            .help = "Import, export, back up and delete your Pikmin 2 saves.",
            .isDisabled = [prelaunch] { return !prelaunch; },
            .disabledNote = "Return to the start menu to manage saves.",
        },
        SettingsAction::SaveManager);

    rows.section("Maintenance");
    add_data_folder_row(rows);
    rows.add_action(
        {
            .label = "Reset All Settings",
            .help = "Put every setting back to its default. Your saves, disc and controller layouts are kept.",
        },
        [] {
            const auto dismiss = [](Modal& modal) {
                play_sound(kSoundWindowClose);
                modal.pop();
            };
            push_document(std::make_unique<Modal>(Modal::Props{
                .title = "Reset All Settings",
                .bodyText = fmt::format("Reset every {} setting to its default? Your saves, disc and "
                                        "controller layouts are kept.",
                    product()),
                .actions =
                    {
                        {.label = "Cancel", .onPressed = dismiss},
                        {.label = "Reset",
                            .onPressed =
                                [](Modal& modal) {
                                    auto& s = settings();
                                    const VarBase* kept[] = {&s.backend.discPath, &s.backend.discVerification,
                                        &s.backend.discFingerprint, &s.interface.presetChosen};
                                    for (const Meta& m : settings_meta()) {
                                        if (std::ranges::find(kept, m.var) == std::end(kept)) {
                                            m.var->reset();
                                        }
                                    }
                                    play_sound(kSoundItemChange);
                                    modal.pop();
                                }},
                    },
                .onDismiss = dismiss,
                .variant = "danger",
                .icon = "warning",
            }));
            if (auto* doc = top_document()) {
                doc->focus();
            }
        });
    rows.external_action(
        {
            .label = "Reset All Controls",
            .help = "Put every controller and keyboard binding back to its default.",
        },
        SettingsAction::ResetAllControls);
    rows.add_action(
        {
            .label = "Choose Preset",
            .help = "Pick Modern or Original again. Only the picture settings of the preset change; "
                    "cheats and gameplay options stay as they are.",
        },
        [] {
            push_document(std::make_unique<PresetWindow>(false));
            if (auto* doc = top_document()) {
                doc->focus();
            }
        });

    rows.section("Developer");
    const auto adv = Tier::Advanced;
    rows.toggle(s.tools.perfOverlay,
        {.label = "Performance Overlay", .help = "Show draw calls, shader counts and frame timing.", .tier = adv});
    rows.choices(s.tools.logLevel,
        {{LogLevel::Debug, "Debug"}, {LogLevel::Info, "Info"}, {LogLevel::Warning, "Warnings"}, {LogLevel::Error, "Errors"}},
        {.label = "Log Detail", .help = fmt::format("How much {} writes to its log.", product()), .tier = adv});
    rows.toggle(s.tools.panicScreen,
        {
            .label = "Show Crash Screen",
            .help = "When the game hits a fatal error, show its original error screen instead of closing.",
            .tier = adv,
        });
    rows.toggle(s.tools.logFps,
        {.label = "Log Frame Rate", .help = "Print the frame rate to the terminal once a second.", .tier = adv});
    rows.toggle(s.tools.schedDebug,
        {
            .label = "Scheduler Log",
            .help = "Every two seconds, print the state of the game's threads to the terminal. Useful "
                    "when tracking down hangs.",
            .tier = adv,
        });
    rows.toggle(s.tools.audioDebug,
        {
            .label = "Audio Log",
            .help = "Print the sound system's activity to the terminal. Useful when music or sound "
                    "effects go missing or cut out.",
            .tier = adv,
        });
    rows.toggle(s.tools.audioVoiceLog,
        {
            .label = "Audio Voice Log",
            .help = "Print every sound voice as it starts and stops to the terminal. Very noisy.",
            .tier = adv,
        });
}

// ---- Entry points ----

bool settings_backend_is_running(std::string_view id) {
    return backend_id_is_running(id);
}

SettingsWindow& open_settings_window(bool prelaunch, Document* opener) {
    auto window = std::make_unique<SettingsWindow>(prelaunch);
    SettingsWindow& ref = *window;
    if (opener != nullptr) {
        opener->push(std::move(window));
    } else {
        push_document(std::move(window));
    }
    ref.focus();
    return ref;
}

void set_settings_action(SettingsAction action, std::function<void()> handler, std::function<std::string()> status) {
    if (action < SettingsAction::Count) {
        action_handlers()[static_cast<size_t>(action)] = {std::move(handler), std::move(status)};
    }
}

}  // namespace mppm::ui
