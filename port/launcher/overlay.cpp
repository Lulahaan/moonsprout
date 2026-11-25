#include "overlay.hpp"

#include "menu_bar.hpp"
#include "restart.hpp"
#include "settings.hpp"

#include "ui/ui.hpp"
#include "ui/window.hpp"

#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <fmt/format.h>
#include <mppm/launcher.hpp>
#include <p2_host.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <cstdlib>

namespace mppm::ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/overlay.rcss" />
    <link type="text/rcss" href="res/rml/ingame.rcss" />
</head>
<body>
    <hud-corner id="corner" corner="tl">
        <fps id="fps" />
        <cheats-badge id="cheats"><icon class="warning" /><span>Cheats on</span></cheats-badge>
    </hud-corner>
    <pipeline-progress id="pipeline-progress">
        <status>
            <icon />
            <status-label id="pipeline-progress-label" />
        </status>
        <progress id="pipeline-progress-bar" />
    </pipeline-progress>
</body>
</rml>
)RML";

const Rml::String kScrimSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/ingame.rcss" />
</head>
<body class="scrim"></body>
</rml>
)RML";

constexpr int kMaxVisibleToasts = 3;
constexpr float kToastTopDp = 32.0f;
constexpr float kToastTopCompactDp = 16.0f;
constexpr float kToastTopUnderBarDp = 96.0f;
constexpr float kToastGapDp = 8.0f;
constexpr auto kMenuHintDuration = std::chrono::seconds(5);
constexpr auto kMenuHintDelay = std::chrono::milliseconds(1200);
constexpr auto kPipelineOpenDelay = std::chrono::milliseconds(250);
// The take-off's cream flash dissolves into the title after the start menu hands over: a shader
// pill popping up in it reads as a glitch, so it waits until the dissolve is done.
constexpr auto kPipelineAfterMenu = std::chrono::milliseconds(2500);
// The exit transition is 0.18s; remove the element a little after it even if no transitionend
// arrives (a document hidden mid-transition never sends one).
constexpr auto kToastExitGrace = std::chrono::milliseconds(400);

constexpr std::array<const char*, 5> kCorners = {"", "tl", "tr", "bl", "br"};

// The heading icon that repeats each toast type, so the type is never told by colour alone.
const char* toast_icon(const Rml::String& type) {
    if (type == "success") {
        return "success";
    }
    if (type == "warning") {
        return "warning";
    }
    if (type == "error") {
        return "error";
    }
    if (type == "achievement") {
        return "trophy";
    }
    if (type == "controller") {
        return "controller";
    }
    return "info";
}

Rml::Element* create_toast(Rml::Element* parent, const Toast& toast) {
    auto* elem = append(parent, "toast");
    const Rml::String type = toast.type.empty() ? "info" : toast.type;
    elem->SetClass(type, true);
    if (!toast.modId.empty()) {
        elem->SetAttribute("mod-id", toast.modId);
    }
    auto* heading = append(elem, "heading");
    if (toast.title.starts_with("<")) {
        heading->SetInnerRML(toast.title);
    } else {
        append_text(append(heading, "toast-title"), toast.title);
    }
    append(heading, "icon")->SetClass(toast_icon(type), true);
    if (!toast.content.empty()) {
        auto* message = append(elem, "message");
        if (toast.content.starts_with("<")) {
            message->SetInnerRML(toast.content);
        } else {
            append_text(append(message, "toast-message-text"), toast.content);
        }
    }
    append(elem, "progress")->SetAttribute("value", 1.0f);
    return elem;
}

SDL_Gamepad* gamepad_for_port(u32 port) noexcept {
    const s32 index = PADGetIndexForPort(port);
    if (index < 0) {
        return nullptr;
    }
    return PADGetSDLGamepadForIndex(static_cast<u32>(index));
}

// What the controller's menu button is called on Player 1's pad (SETTINGS 13: Back, or R + Start
// on a GameCube adapter). Empty without a pad.
Rml::String menu_button_name() {
    SDL_Gamepad* gamepad = gamepad_for_port(PAD_CHAN0);
    if (gamepad == nullptr) {
        return {};
    }
    switch (SDL_GetGamepadType(gamepad)) {
    case SDL_GAMEPAD_TYPE_PS3:
        return "Select";
    case SDL_GAMEPAD_TYPE_PS4:
        return "Share";
    case SDL_GAMEPAD_TYPE_PS5:
        return "Create";
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return "View";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return "\xe2\x88\x92"; // the minus button
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return "R + Start";
    default:
        return "Back";
    }
}

Rml::Element* create_menu_hint(Rml::Element* parent) {
    auto* elem = append(parent, "toast");
    elem->SetClass("menu-notification", true);
    auto* message = append(elem, "message");
    auto* row = append(message, "row");
    append_text_element(row, "span", "Press");
    append_text_element(row, "key-cap", "F1");
    if (const Rml::String pad = menu_button_name(); !pad.empty()) {
        append_text_element(row, "span", "or");
        append(row, "icon")->SetClass("controller", true);
        append_text_element(row, "key-cap", pad)->SetClass("pad", true);
    }
    append_text_element(row, "span", "to open the menu");
    return elem;
}

Rml::Element* create_controller_warning(Rml::Element* parent) {
    auto* elem = append(parent, "toast");
    elem->SetClass("controller-warning", true);
    auto* heading = append(elem, "heading");
    append_text(append(heading, "toast-title"), "No controller for Player 1");
    append(heading, "icon")->SetClass("warning", true);
    auto* message = append(elem, "message");
    append_text(append(message, "toast-message-text"),
        "Press F1 and choose Controllers to pick a device.");
    return elem;
}

void remove_element(Rml::Element*& elem) noexcept {
    if (elem == nullptr) {
        return;
    }
    if (auto* parent = elem->GetParentNode()) {
        parent->RemoveChild(elem);
    }
    elem = nullptr;
}

// Any cheat.* setting away from its default (SETTINGS 7).
bool cheats_active() {
    const auto& c = settings().cheat;
    return c.unlockChallenge.modified() || c.daySpeed.modified() || c.sunsetOnDayOne.modified() ||
           c.invincibleCaptains.modified() || c.infiniteSprays.modified() || c.pikminLimit.modified();
}

bool in_game_menu_open() {
    const auto* bar = MenuBar::current();
    return bar != nullptr && bar->visible();
}

// The open F1 bar shows the FPS reading itself (its status block drops it from fit level 4 on).
bool bar_shows_fps(Rml::ElementDocument* overlay) {
    const auto* bar = MenuBar::current();
    auto* context = overlay != nullptr ? overlay->GetContext() : nullptr;
    if (bar == nullptr || !bar->visible() || context == nullptr) {
        return false;
    }
    for (int i = 0; i < context->GetNumDocuments(); i++) {
        Rml::ElementDocument* doc = context->GetDocument(i);
        if (doc != nullptr && doc->IsVisible() && doc->IsClassSet("in-game-bar")) {
            return !doc->IsClassSet("fit-4");
        }
    }
    return false;
}

// Inherited by the process a Reset Game / restart executes (execv / CreateProcess).
constexpr const char* kMenuHintEnv = "MPPM_SESSION_MENU_HINT_SHOWN";

bool menu_hint_shown_in_session() {
    const char* v = std::getenv(kMenuHintEnv);
    return v != nullptr && v[0] == '1';
}

void mark_menu_hint_shown_in_session() {
#ifdef _WIN32
    _putenv_s(kMenuHintEnv, "1");
#else
    setenv(kMenuHintEnv, "1", 1);
#endif
}

float dp_ratio(Rml::ElementDocument* doc) {
    auto* context = doc != nullptr ? doc->GetContext() : nullptr;
    return context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0f;
}

} // namespace

const std::string& fps_text() {
    static std::string text;
    static Uint64 last = 0;
    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();
    // While the menu holds the game, presents are UI-only frames: keep the last gameplay reading
    // instead of the menu's redraw rate, and after the hold wait until Aurora's one-second present
    // window holds game frames only.
    if (launcher::hold_game()) {
        if (text.empty()) {
            text = "Paused";
        }
        last = now + freq / 2;
        return text;
    }
    if (last == 0 || freq == 0 || (now > last && (now - last) * 2 >= freq)) {
        last = now;
        // Once a game runs, count new pictures (shown ticks plus in-between passes) rather than
        // presents: with present skipping or Smooth Motion the two differ (fps_design 5.6).
        text = fmt::format("{:.0f} FPS", launcher::game_booted() ? p2_host_picture_fps() : aurora_get_fps());
    }
    return text;
}

// ------------------------------------------------------------------------------------ overlay

Overlay::Overlay() : Document(kDocumentSource, true, DocumentScope::Overlay) {
    mCorner = mDocument->GetElementById("corner");
    mFps = mDocument->GetElementById("fps");
    mCheats = mDocument->GetElementById("cheats");
    mPipeline = mDocument->GetElementById("pipeline-progress");
    mPipelineLabel = mDocument->GetElementById("pipeline-progress-label");
    mPipelineBar = mDocument->GetElementById("pipeline-progress-bar");

    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        Rml::Element* target = event.GetTargetElement();
        for (auto& toast : mToasts) {
            if (toast.element == target && toast.closing) {
                target->SetPseudoClass("done", true);
            }
        }
        if (target != nullptr && (target == mMenuHint || target == mControllerWarning) &&
            !target->HasAttribute("open"))
        {
            target->SetPseudoClass("done", true);
        }
    });
}

Overlay::~Overlay() = default;

void Overlay::show() {
    if (mDocument != nullptr) {
        // Never takes focus: the game and the focused document keep the keyboard.
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

void Overlay::update() {
    Document::update();
    if (mDocument == nullptr) {
        return;
    }
    ++mFrames;
    // While the bar is open the top corners move below it (it floats 16dp down and is 64dp tall),
    // but only when the bar reaches that corner: on a wide window it ends well before it.
    mBarOpen = in_game_menu_open();
    update_corner();
    const auto* bar = MenuBar::current();
    const float cornerLeft = mCorner->GetAbsoluteLeft();
    const bool underBar = mBarOpen && bar != nullptr &&
                          bar->overlaps_columns(cornerLeft, cornerLeft + mCorner->GetOffsetWidth());
    if (underBar != mCornerUnderBar) {
        mCornerUnderBar = underBar;
        mDocument->SetClass("bar-open", underBar);
    }
    // A window that reaches the corner (large Menu Size, small window) would sit under the pills:
    // fade them out while it is open rather than draw over its tab bar or close button.
    const bool covered = corner_covered();
    if (covered != mCornerCovered) {
        mCornerCovered = covered;
        mDocument->SetClass("corner-covered", covered);
    }
    update_pipeline_progress();
    update_menu_hint();
    update_controller_warning();
    update_toasts();
}

bool Overlay::corner_covered() const {
    auto* context = mDocument->GetContext();
    if (context == nullptr || (!mFps->HasAttribute("open") && !mCheats->HasAttribute("open"))) {
        return false;
    }
    // The pills' box, a little enlarged so they don't touch a window's edge either.
    const float margin = 4.0f * dp_ratio(mDocument);
    const Rml::Vector2f pos = mCorner->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = mCorner->GetBox().GetSize(Rml::BoxArea::Border);
    const float l = pos.x - margin, t = pos.y - margin;
    const float r = pos.x + size.x + margin, b = pos.y + size.y + margin;
    for (int i = 0; i < context->GetNumDocuments(); i++) {
        Rml::ElementDocument* doc = context->GetDocument(i);
        if (doc == nullptr || doc == mDocument || !doc->IsVisible()) {
            continue;
        }
        Rml::Element* window = doc->GetElementById("window");
        if (window == nullptr || !window->IsVisible()) {
            continue;
        }
        const Rml::Vector2f wpos = window->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f wsize = window->GetBox().GetSize(Rml::BoxArea::Border);
        if (wpos.x < r && wpos.x + wsize.x > l && wpos.y < b && wpos.y + wsize.y > t) {
            return true;
        }
    }
    return false;
}

void Overlay::update_corner() {
    const FpsCounter fpsCorner = settings().video.fpsCounter.get();
    const bool showFps = fpsCorner != FpsCounter::Off;
    const bool showCheats = cheats_active();
    // The badge sits beside the FPS pill; with the counter off it takes the top-left corner.
    const char* corner = showFps ? kCorners[static_cast<size_t>(fpsCorner)] : "tl";
    if (mCorner->GetAttribute<Rml::String>("corner", "") != corner) {
        mCorner->SetAttribute("corner", corner);
    }

    // The F1 bar shows the same reading in its status block while it is open: one is enough. The
    // pill stays when the bar's narrowest layouts (fit-4 and up) drop their FPS readout.
    if (showFps && !bar_shows_fps(mDocument)) {
        if (!mFps->HasAttribute("open")) {
            mFps->SetAttribute("open", "");
        }
        if (const std::string& text = fps_text(); text != mFpsText) {
            mFpsText = text;
            set_text_content(mFps, mFpsText);
        }
    } else if (mFps->HasAttribute("open")) {
        mFps->RemoveAttribute("open");
    }

    if (showCheats != mCheats->HasAttribute("open")) {
        if (showCheats) {
            mCheats->SetAttribute("open", "");
        } else {
            mCheats->RemoveAttribute("open");
        }
    }
}

void Overlay::update_toasts() {
    auto& queue = get_toasts();
    const auto now = clock::now();

    // Take queued toasts while fewer than three are on screen (DESIGN 5.14).
    int showing = static_cast<int>(std::ranges::count_if(mToasts, [](const LiveToast& t) { return !t.closing; }));
    while (!queue.empty() && showing < kMaxVisibleToasts) {
        Toast toast = std::move(queue.front());
        queue.pop_front();
        if (toast.duration <= clock::duration::zero()) {
            // 4s by default; warnings and errors stay 6s.
            const bool serious = toast.type == "warning" || toast.type == "error";
            toast.duration = serious ? std::chrono::seconds(6) : std::chrono::seconds(4);
        }
        LiveToast live;
        live.element = create_toast(mDocument, toast);
        live.start = now;
        live.duration = toast.duration;
        mToasts.push_back(live);
        ++showing;
    }

    // Lay the stack out top to bottom; a closing toast keeps its place until it has faded.
    const float ratio = dp_ratio(mDocument);
    const auto* context = mDocument->GetContext();
    const bool compact = context != nullptr && static_cast<float>(context->GetDimensions().y) / ratio <= 640.0f;
    float y = (mBarOpen ? kToastTopUnderBarDp : compact ? kToastTopCompactDp : kToastTopDp) * ratio;
    // Keep clear of the FPS pill / cheats badge when they share the top-right corner.
    if (mCorner->GetAttribute<Rml::String>("corner", "") == "tr" &&
        (mFps->HasAttribute("open") || mCheats->HasAttribute("open")))
    {
        y = std::max(y, ((mCornerUnderBar ? 92.0f : 12.0f) + 26.0f + 10.0f) * ratio);
    }
    for (auto& toast : mToasts) {
        if (std::abs(toast.top - y) > 0.5f) {
            toast.top = y;
            toast.element->SetProperty(Rml::PropertyId::Top, Rml::Property(y, Rml::Unit::PX));
        }
        y += toast.element->GetOffsetHeight() + kToastGapDp * ratio;
    }

    for (auto& toast : mToasts) {
        const float total = std::chrono::duration<float>(toast.duration).count();
        const float elapsed = std::chrono::duration<float>(now - toast.start).count();
        const float remaining = total > 0.0f ? std::clamp(1.0f - elapsed / total, 0.0f, 1.0f) : 0.0f;
        if (auto* progress = toast.element->QuerySelector("progress")) {
            progress->SetAttribute("value", remaining);
        }
        if (!toast.closing) {
            if (remaining > 0.0f) {
                // Opened on its second frame so the entrance transition runs.
                if (!toast.element->HasAttribute("open") && now > toast.start) {
                    toast.element->SetAttribute("open", "");
                }
            } else {
                toast.closing = true;
                toast.element->RemoveAttribute("open");
            }
        }
    }
    std::erase_if(mToasts, [now](LiveToast& toast) {
        const bool gone = toast.closing && (toast.element->IsPseudoClassSet("done") ||
                                               now >= toast.start + toast.duration + kToastExitGrace);
        if (gone) {
            remove_element(toast.element);
        }
        return gone;
    });
}

void Overlay::update_pipeline_progress() {
    // During the take-off and its closing flash the pill would read as a glitch over the scene.
    if (is_prelaunch_open() || is_prelaunch_taking_off()) {
        mPrelaunchSeen = clock::now();
    }
    const auto* stats = aurora_get_stats();
    const uint32_t queued = stats != nullptr ? stats->queuedPipelines : 0;
    if (queued == 0) {
        if (mPipeline->HasAttribute("open")) {
            mPipeline->RemoveAttribute("open");
        }
        mPipelineActive = false;
        mPipelineBase = 0;
        mLastQueued = 0;
        return;
    }

    const uint32_t created = stats->createdPipelines;
    if (!mPipelineActive || created < mPipelineBase) {
        mPipelineActive = true;
        mPipelineBase = created;
        mPipelineStart = clock::now();
        mLastQueued = 0;
    }
    const uint32_t built = created - mPipelineBase;
    const uint32_t total = queued + built;
    const float progress = total > 0 ? static_cast<float>(built) / static_cast<float>(total) : 0.0f;
    if (queued != mLastQueued) {
        mLastQueued = queued;
        set_text_content(mPipelineLabel, fmt::format("Preparing {} {}", queued, queued == 1 ? "shader" : "shaders"));
    }
    mPipelineBar->SetAttribute("value", progress);

    // The start menu's footer (disc line, prompts, version) fills the bottom band: over the menu
    // the pill sits at the top right. Elsewhere it sits above a bottom-left FPS pill, not on it.
    const bool overMenu = is_prelaunch_open();
    if (mPipeline->IsClassSet("over-menu") != overMenu) {
        mPipeline->SetClass("over-menu", overMenu);
    }
    if (overMenu) {
        mPipeline->RemoveProperty(Rml::PropertyId::Bottom);
    } else {
        const float ratio = dp_ratio(mDocument);
        const bool sharesCorner = mCorner->GetAttribute<Rml::String>("corner", "") == "bl" &&
                                  (mFps->HasAttribute("open") || mCheats->HasAttribute("open"));
        mPipeline->SetProperty(Rml::PropertyId::Bottom,
            Rml::Property((sharesCorner ? 16.0f + 26.0f + 8.0f : 16.0f) * ratio, Rml::Unit::PX));
    }

    const bool show = settings().interface.shaderIndicator.get() &&
                      clock::now() >= mPipelineStart + kPipelineOpenDelay &&
                      !is_prelaunch_taking_off() &&
                      (overMenu || mPrelaunchSeen == clock::time_point{} || clock::now() >= mPrelaunchSeen + kPipelineAfterMenu);
    if (show != mPipeline->HasAttribute("open")) {
        if (show) {
            mPipeline->SetAttribute("open", "");
        } else {
            mPipeline->RemoveAttribute("open");
        }
    }
}

void Overlay::update_menu_hint() {
    // Once per session, after the start menu has gone and the game is on screen (DESIGN 8.3). A
    // direct boot (disc on the command line) has no start menu: the hint follows the first frames.
    // Reset Game and restarts re-execute the binary, which is still the same session to the user:
    // the environment carries "already shown" into the new process.
    const auto now = clock::now();
    if (!mHintDone && (menu_hint_shown_in_session() || in_game_menu_open())) {
        mHintDone = true; // an earlier process showed it, or the player found the menu first
        mark_menu_hint_shown_in_session();
    }
    if (!mHintDone && !mHintArmed) {
        if (!is_prelaunch_open() && mFrames > 2) {
            mHintArmed = true;
            mPrelaunchClosedAt = now;
        }
    }
    if (mHintArmed && !mHintDone && now >= mPrelaunchClosedAt + kMenuHintDelay) {
        mHintDone = true;
        mark_menu_hint_shown_in_session();
        if (settings().interface.menuHint.get() && !in_game_menu_open()) {
            show_menu_notification();
        }
    }
    if (consume_menu_notification_request()) {
        if (mMenuHint == nullptr) {
            mMenuHint = create_menu_hint(mDocument);
        }
        mMenuHintStart = now;
    }
    if (mMenuHint == nullptr) {
        return;
    }
    // Gone after 5s, or as soon as the player has found the menu.
    const bool expired = now >= mMenuHintStart + kMenuHintDuration || in_game_menu_open() ||
                         !settings().interface.menuHint.get();
    if (expired) {
        if (mMenuHint->IsPseudoClassSet("done") || !mMenuHint->IsPseudoClassSet("opened") ||
            now >= mMenuHintStart + kMenuHintDuration + kToastExitGrace)
        {
            remove_element(mMenuHint);
        } else {
            mMenuHint->RemoveAttribute("open");
        }
    } else if (now > mMenuHintStart) {
        mMenuHint->SetAttribute("open", "");
        mMenuHint->SetPseudoClass("opened", true);
    }
}

void Overlay::update_controller_warning() {
    // Player 1 has neither a controller nor the keyboard: say how to fix it, unless a window that
    // could fix it is already open.
    u32 count = 0;
    const bool noInput = PADGetIndexForPort(PAD_CHAN0) < 0 && PADGetKeyButtonBindings(PAD_CHAN0, &count) == nullptr;
    bool windowOpen = false;
    for_each_visible_document([&windowOpen](Document& doc) {
        windowOpen = windowOpen || doc.scope() == DocumentScope::Window;
    });
    const bool show = noInput && !windowOpen && !is_prelaunch_open() && mFrames > 2;
    if (show) {
        if (mControllerWarning == nullptr) {
            mControllerWarning = create_controller_warning(mDocument);
        } else {
            mControllerWarning->SetAttribute("open", "");
            mControllerWarning->SetPseudoClass("opened", true);
            mControllerWarning->SetPseudoClass("done", false);
        }
    } else if (mControllerWarning != nullptr) {
        if (mControllerWarning->IsPseudoClassSet("done") || !mControllerWarning->IsPseudoClassSet("opened")) {
            remove_element(mControllerWarning);
        } else {
            mControllerWarning->RemoveAttribute("open");
        }
    }
}

bool Overlay::handle_nav_command(Rml::Event&, NavCommand) {
    return false;
}

// -------------------------------------------------------------------------------------- scrim

Scrim::Scrim() : Document(kScrimSource, true, DocumentScope::Overlay) {}

void Scrim::show() {
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

void Scrim::update() {
    Document::update();
    if (mDocument == nullptr) {
        return;
    }
    // 30% under the bar (and its small dialogs), 50% under a window (DESIGN 8.2). The start menu
    // has its own backdrop and scrims, so nothing while it is up.
    int level = 0;
    if (!is_prelaunch_open()) {
        for_each_visible_document([&level](Document& doc) {
            if (dynamic_cast<Window*>(&doc) != nullptr) {
                level = 2;
            } else if (level == 0) {
                level = 1;
            }
        });
    }
    if (level != mLevel) {
        mLevel = level;
        mDocument->SetClass("level-1", level == 1);
        mDocument->SetClass("level-2", level == 2);
    }
}

} // namespace mppm::ui
