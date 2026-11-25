#include "prelaunch.hpp"

#include "branding.hpp"
#include "live_apply.hpp"
#include "menu_registry.hpp"
#include "restart.hpp"
#include "scene_bridge.hpp"
#include "settings.hpp"
#include "update_check.hpp"

#include <aurora/lib/window.hpp>
#include <borealis/file_select.hpp>
#include <mppm/launcher.hpp>
#include <p2_mods.h>
#include "ui/input.hpp"
#include "ui/preset.hpp"
#include "ui/settings_window.hpp"
#include "ui/ui.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace mppm::ui {
namespace scene = launcher::scene;

namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/prelaunch.rcss" />
</head>
<body>
    <prelaunch-background>
        <bd-sky />
        <bd-stars class="far" />
        <bd-stars class="near" />
        <bd-stars class="twinkle" />
        <bd-sun />
        <bd-planet />
        <bd-atmosphere />
        <bd-ship>
            <bd-bob>
                <bd-tilt>
                    <bd-spark class="b" />
                    <bd-spark class="y" />
                    <bd-spark class="r" />
                    <bd-flame />
                    <img src="res/img/ship.png" />
                </bd-tilt>
            </bd-bob>
        </bd-ship>
        <bd-glow />
    </prelaunch-background>
    <intro-veil />
    <prelaunch-vignette />
    <content id="root">
        <scrims>
            <scrim-extend />
            <prelaunch-gradient />
            <corner-scrim />
        </scrims>
        <menu>
            <hero class="intro-item">
                <eyebrow>A native port for your own disc</eyebrow>
                <wordmark-box>
                    <img src="res/img/wordmark.png" />
                    <wordmark-leaf><img src="res/img/wordmark-leaf.png" /></wordmark-leaf>
                </wordmark-box>
            </hero>
            <menu-list id="menu-list" />
        </menu>
        <disc-info class="intro-item">
            <disc-status id="disc-status">
                <status-orb><icon /></status-orb>
                <disc-status-label id="disc-status-label" />
            </disc-status>
            <disc-line id="disc-line"><disc-version id="disc-version" /></disc-line>
            <disc-hint id="disc-hint" />
        </disc-info>
        <version-info class="intro-item">
            <update-status id="update-status">
                <update-checking><update-text>Checking for updates...</update-text><beads><bead /><bead /><bead /></beads></update-checking>
                <update-failed><update-text>Couldn't check for updates</update-text></update-failed>
            </update-status>
            <version-row>
                <prompts>
                    <prompt><glyph id="prompt-confirm">Enter</glyph><prompt-label>Select</prompt-label></prompt>
                </prompts>
                <version-label id="version-label" />
            </version-row>
        </version-info>
    </content>
    <launch-bloom id="launch-bloom" />
    <letterbox class="top" />
    <letterbox class="bottom" />
    <skip-hint id="skip-hint">Press any button to skip</skip-hint>
    <drop-target><drop-card><icon /><drop-text>Drop to use this disc image</drop-text></drop-card></drop-target>
    <launch-flash><flash-core /><beads><bead /><bead /><bead /></beads></launch-flash>
</body>
</rml>
)RML";

// DESIGN 6.8: the entrance is over 1020 ms after the first frame.
constexpr double kEntranceSeconds = 1.02;
// DESIGN 6.9: the landing flight and its glow; the backdrop fades no earlier than this.
constexpr double kLandingSeconds = 1.3;
// DESIGN 6.7: "Couldn't check for updates" fades after 6 s.
constexpr double kFailedNoticeSeconds = 6.0;
// README 6: any button skips the take-off. Not in its first 600 ms, so a double press or double
// click on Play cannot skip it (scene::arm_skip also waits for the Play press to be released).
constexpr double kSkipFromMs = 600.0;
// The "Press any button to skip" pill (SCENE.md / DESIGN 7.2): in at 1.2 s, dimmed after 4 s.
constexpr double kSkipHintShowMs = 1200.0;
constexpr double kSkipHintDimMs  = 4000.0;
// The checking row (beads) only shows when the check is still running this long after the
// entrance: a quick answer never flashes it (DESIGN 6.7).
constexpr double kCheckingShowSeconds = 0.8;

// Nominal cue times in ms from the Play press (DESIGN 7.1, SCENE 11). Index = Prelaunch::Cue:
// launch_begin, hop, board, ignite, liftoff, exit_frame, handoff. Negative = not sent.
// kFullTimes is the live scene's take-off (used only if the scene hangs or fails mid-way);
// kBackdropTimes keeps the still backdrop's ship and flash at their own pace when no scene is live
// at Play.
constexpr double kFullTimes[]     = {0, 3300, 7640, 12590, 12990, 16890, 17660};
constexpr double kBackdropTimes[] = {0, 600, 1400, 2800, 3070, 5600, 6350};
constexpr double kOffTimes[]      = {0, -1, -1, -1, -1, 0, 400};
// With a live scene the timeline follows its cues only, however slowly the game runs. The nominal
// times take over only when there is no scene, when it failed, or when it hung: it polls its
// command once per frame (scene::heartbeat_age_ms), so 3 s without a poll is a hang, and a
// take-off still running 60 s after Play is one too.
constexpr double kHeartbeatStallMs = 3000.0;
constexpr int kHeartbeatStallFrames = 30; // launcher frames without a poll, besides the 3 s
constexpr double kSceneMaxMs       = 60000.0;
// No hand-over this long after the flash became opaque: navy and the loader (7.2).
constexpr double kHandoffWaitMs = 2000.0;
// The flash holds this long once opaque before it reveals the title (7.2: handoff + 150).
constexpr double kHandoffHoldMs = 150.0;
// The scene's own end fade lasts 0.75 s (SCENE 11). The flash veil starts closing (0.4 s, rcss)
// this long into it, so it is opaque 50 ms before the scene section ends and the game frame under
// it goes black. A skip has no veil: the scene's skip fade is 0.3 s and the flash is opaque in 0.25.
constexpr double kSceneFadeMs = 300.0;
// A launch that starts with no live scene (parked or failed) and Take-off Off: the title section
// only starts loading at Play, so the flash holds at least this long before it reveals it.
constexpr double kColdRevealMs = 1500.0;
// The warm flash dissolves this long into the title, which fades up from black by itself
// (prelaunch.rcss body.launch-reveal launch-flash).
constexpr double kRevealMs = 1600.0;
// Set across a silent restart (the first-run preset): the menu that comes back skips its entrance.
constexpr const char* kSeamlessEnv = "MPPM_SEAMLESS_RESTART";

// The SDL file dialog answers on another thread.
std::mutex sPickMutex;
std::optional<std::string> sPickedPath;
bool sPickFailed = false; // no portal and no zenity (or they failed): not the same as a cancel

// The entrance plays once per process (header comment).
bool sShownBefore = false;
Prelaunch* sCurrent = nullptr;

bool take_seamless_flag() {
    const char* v = std::getenv(kSeamlessEnv);
    if (v == nullptr || v[0] == '\0') {
        return false;
    }
    SDL_unsetenv_unsafe(kSeamlessEnv);
    return true;
}

const char* phase_name(int phase) {
    static constexpr const char* kNames[] = {"idle", "launching", "flash", "wait", "handoff", "reveal", "done"};
    return phase >= 0 && phase < 7 ? kNames[phase] : "?";
}

std::string file_name(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

double now_seconds() {
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// "GPVE01" -> "USA". Pikmin 2 shipped in these regions; anything else shows the raw id only.
const char* region_name(const std::string& gameId) {
    if (gameId.size() < 4) {
        return nullptr;
    }
    switch (gameId[3]) {
    case 'E':
        return "USA";
    case 'P':
        return "EUR";
    case 'J':
        return "JPN";
    case 'K':
        return "KOR";
    case 'U':
        return "AUS";
    default:
        return nullptr;
    }
}

} // namespace

void Prelaunch::game_started() {
    // Booted in place behind this menu (launcher.cpp 3.6): from now on it is the menu over the game,
    // with the scene loading behind the opaque backdrop.
    mGameRunning = true;
    mDiscPath    = launcher::startup_disc();
    refresh_disc();
    mSceneReady  = false;
    mSceneSwap   = false;
    set_body_class("scene-ready", false);
    set_body_class("scene-swap", false);
    mWantLanding = scene::live();
    mLanding     = false;
    mLanded      = false;
    set_body_class("landing", false);
    set_body_class("landed", false);
    refresh_version_label(); // the mods were mounted after this menu was built
}

void Prelaunch::refresh_version_label() {
    // " · N mods" only when the boot mounted data mods (docs/online/M5_2_MODS.md; unchanged otherwise).
    const int mods = p2_mods_count();
    set_text_content(mDocument->GetElementById("version-label"),
        mods > 0 ? fmt::format("{} v{} \xC2\xB7 {} mod{}", branding::kProductName, branding::kVersion, mods,
                       mods == 1 ? "" : "s")
                 : fmt::format("{} v{}", branding::kProductName, branding::kVersion));
}

Prelaunch* Prelaunch::current() {
    return sCurrent;
}

Prelaunch::Prelaunch(bool gameRunning)
    : Document(kDocumentSource, false, DocumentScope::Prelaunch)
    , mGameRunning(gameRunning)
    , mContinuation(sShownBefore) {
    // A first-run restart comes back without the entrance; take the flag either way so it does
    // not leak into a later restart.
    mContinuation = take_seamless_flag() || mContinuation;
    sShownBefore = true;
    sCurrent     = this;
    // Over the game the menu shows the disc that booted (a command-line disc may differ from the
    // remembered one); standalone it shows the remembered one.
    mDiscPath = mGameRunning ? launcher::startup_disc() : settings().backend.discPath.get();
    if (mDocument == nullptr) {
        return;
    }
    mBloom = mDocument->GetElementById("launch-bloom");
    build_menu();

    if (auto* line = mDocument->GetElementById("disc-line")) {
        mChangeDisc = std::make_unique<Button>(line, "Change");
        mChangeDisc->root()->SetId("disc-change");
        mChangeDisc->on_pressed([this] { pick_disc(); });
        mChangeDisc->listen(Rml::EventId::Mousemove,
            [this](Rml::Event&) { hover_focus(*mChangeDisc); });
    }
    if (auto* status = mDocument->GetElementById("update-status")) {
        mUpdatePill = std::make_unique<Button>(status, "");
        Rml::Element* pill = mUpdatePill->root();
        pill->SetId("update-pill");
        append(append(pill, "pill-orb"), "icon");
        append(pill, "pill-label")->SetId("update-pill-label");
        append(pill, "pill-open");
        mUpdatePill->on_pressed([this] {
            if (!yield_input()) {
                launcher::update_check::open_release();
            }
        });
        mUpdatePill->listen(Rml::EventId::Mousemove, [this](Rml::Event&) { hover_focus(*mUpdatePill); });
    }
    refresh_version_label();

    // Mouse users get no button prompts; any key brings them back (DESIGN 5.17). The pointer is
    // polled in update() (track_input_device), so a window mapping under a resting pointer does not
    // count as mouse use, and keys pressed on the first-run card count too. A key or pad button on
    // this document is caught here at once.
    listen(
        Rml::EventId::Keydown, [this](Rml::Event&) { set_pointer_input(false); }, true);
    // During the take-off a click anywhere skips, like any other button (scene::consume_any_press
    // sees it too; this catches a click the poll missed).
    listen(Rml::EventId::Mousedown, [this](Rml::Event&) {
        if (mPhase == Phase::Launching) {
            request_skip();
        }
    });

    mCueListener = scene::on_cue([this](scene::Cue cue) { on_scene_cue(static_cast<int>(cue)); });
    // The update check starts from update(), once the first-run question has been answered.
    refresh_disc();
    refresh_prompts();
    // A menu created again (DESIGN 6.8) is in its end state before its first frame is drawn: that
    // frame can stay on screen while the game boots, and must not be the entrance's start state.
    if (mContinuation) {
        finish_entrance();
    }
}

Prelaunch::~Prelaunch() {
    if (mCueListener != 0) {
        scene::remove_cue_listener(mCueListener);
    }
    if (sCurrent == this) {
        sCurrent = nullptr;
    }
}

// ---------------------------------------------------------------------------------------- menu

Prelaunch::MenuItem& Prelaunch::add_item(const Rml::String& text, ButtonCallback onPressed) {
    auto* list = mDocument->GetElementById("menu-list");
    MenuItem item;
    // The label lives in its own element: Button::set_text would replace the decoration children.
    item.button = std::make_unique<Button>(list, "");
    Rml::Element* root = item.button->root();
    auto* sprout = append(root, "cursor-sprout");
    append(sprout, "leaf")->SetClass("l", true);
    append(sprout, "leaf")->SetClass("r", true);
    item.label = append_text_element(root, "menu-label", text);
    item.button->on_pressed([this, onPressed = std::move(onPressed)] {
        if (mPhase == Phase::Idle && yield_input()) {
            return;
        }
        if (mPhase != Phase::Idle) {
            // During the take-off a press on the focused item skips it, like any other button
            // (README 6); it must not open that item's window (an online start leaves the focus on
            // Multiplayer).
            request_skip();
            return;
        }
        onPressed();
    });
    Button* button = item.button.get();
    item.button->listen(Rml::EventId::Mousemove, [this, button](Rml::Event&) { hover_focus(*button); });
    mItems.push_back(std::move(item));
    return mItems.back();
}

void Prelaunch::build_menu() {
    // Play first and Quit last; everything registered in between, by order (DESIGN 5.3).
    // Settings opens the settings window with the PRELAUNCH tab (SETTINGS 9).
    if (std::ranges::none_of(launcher::all_menu_entries(launcher::Menu::StartMenu),
            [](const launcher::MenuEntry* e) { return e->id == "settings"; }))
    {
        launcher::register_menu_entry(launcher::Menu::StartMenu,
            {.id = "settings", .label = "Settings", .order = start_order::kSettings, .visible = nullptr,
                .open = [] { open_settings_window(true, sCurrent); }});
    }

    mItems.reserve(8);
    MenuItem& play = add_item("Play", [this] { on_play(); });
    play.button->root()->SetClass("play", true);
    mPlayHalo = append(play.button->root(), "play-halo");
    play.icon = append(play.button->root(), "menu-icon");
    append(append(play.icon, "beads"), "bead");
    // Three beads for "Landing...": the first append made one.
    append(play.icon->GetFirstChild(), "bead");
    append(play.icon->GetFirstChild(), "bead");

    for (const launcher::MenuEntry* entry : launcher::menu_entries(launcher::Menu::StartMenu)) {
        const int index = static_cast<int>(mItems.size());
        add_item(entry->label, [this, index, open = entry->open] {
            // The window it opens gives the focus back to this item when it closes (also when it
            // was clicked, which focuses it only in the same event).
            mReturnItem  = index;
            mReturnExtra = nullptr;
            if (open) {
                open();
            }
        });
    }
    add_item("Quit", [this] {
        if (mPhase != Phase::Idle) {
            return;
        }
        mQuitRequested = true; // standalone: the boot flow returns
        if (mGameRunning) {
            // Over the game nobody polls that flag: leave the way the window's close button does
            // (AURORA_EXIT; launcher.cpp flushes the settings as the event is queued).
            SDL_Event event{};
            event.type           = SDL_EVENT_QUIT;
            event.quit.timestamp = SDL_GetTicksNS();
            SDL_PushEvent(&event);
        }
    });
    mPlay = &mItems.front();

    // Entrance delay ladder (n, top to bottom) and take-off ladder (k, bottom to top, Play excluded).
    const int count = static_cast<int>(mItems.size());
    for (int i = 0; i < count; i++) {
        Rml::Element* root = mItems[i].button->root();
        root->SetClass("intro-item", true);
        root->SetClass("n" + std::to_string(std::min(i, 5)), true);
        if (i > 0) {
            root->SetClass("k" + std::to_string(std::min(count - 1 - i, 4)), true);
        }
    }
}

void Prelaunch::focus_item(int index, bool sound) {
    if (index < 0 || index >= static_cast<int>(mItems.size())) {
        return;
    }
    if (mItems[index].button->focus() && sound) {
        play_sound(kSoundItemFocus);
    }
}

int Prelaunch::focused_item() const {
    auto* context = mDocument != nullptr ? mDocument->GetContext() : nullptr;
    auto* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    for (int i = 0; i < static_cast<int>(mItems.size()); i++) {
        if (mItems[i].button->contains(focused)) {
            return i;
        }
    }
    return -1;
}

bool Prelaunch::focus_extra(Rml::Element* element) {
    if (element == nullptr || !element->IsVisible(true)) {
        return false;
    }
    if (element->Focus(true)) {
        play_sound(kSoundItemFocus);
        return true;
    }
    return false;
}

bool Prelaunch::focus() {
    if (mPhase != Phase::Idle || mPlay == nullptr) {
        return false;
    }
    return mPlay->button->focus();
}

void Prelaunch::remember_focus() {
    if (mCovered || mDocument == nullptr) {
        return;
    }
    const int index = focused_item();
    if (index >= 0) {
        mReturnItem  = index;
        mReturnExtra = nullptr;
        return;
    }
    auto* context = mDocument->GetContext();
    auto* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    for (Button* extra : {mChangeDisc.get(), mUpdatePill.get()}) {
        if (extra != nullptr && extra->contains(focused)) {
            mReturnItem  = -1;
            mReturnExtra = extra;
            return;
        }
    }
}

bool Prelaunch::restore_focus() {
    if (mPhase != Phase::Idle) {
        return false;
    }
    if (mReturnExtra != nullptr && mReturnExtra->root()->IsVisible(true) && mReturnExtra->root()->Focus(true)) {
        return true;
    }
    if (mReturnItem >= 0 && mReturnItem < static_cast<int>(mItems.size()) && mItems[mReturnItem].button->focus()) {
        return true;
    }
    return focus();
}

void Prelaunch::cover() {
    // Stay visible: the backdrop (or the live scene) shows through the settings glass; only the
    // menu steps back. Focus moves to the window on top.
    mWasVisible = visible();
    mCovered    = true;
    set_body_class("covered", true);
}

void Prelaunch::uncover() {
    mCovered    = false;
    mUncoveredAt = SDL_GetTicks();
    set_body_class("covered", false);
    if (!visible()) {
        show();
    }
    // DESIGN 9.4: closing a window returns focus to the control that opened it.
    restore_focus();
}

bool Prelaunch::yield_input() {
    // The first-run card is pushed over the menu without covering it: while it (or any window) is
    // on top, a key that still reaches the menu gives the focus to that document instead.
    if (Document* top = top_document(); top != nullptr && top != this && top->visible()) {
        top->focus();
        return true;
    }
    // SETTINGS 12: the card comes before anything else can be used. A press during the entrance
    // ends it, and the card follows on the next frame.
    if (!mPresetChecked && !settings().interface.presetChosen.get()) {
        finish_entrance();
        return true;
    }
    return false;
}

void Prelaunch::hover_focus(Button& button) {
    if (mPhase != Phase::Idle || !mEntranceDone || mCovered || mPicking || mDocument == nullptr ||
        button.disabled() || !button.root()->IsVisible(true) ||
        (top_document() != this && top_document() != nullptr && top_document()->visible()))
    {
        return;
    }
    auto* context = mDocument->GetContext();
    if (context != nullptr && button.contains(context->GetFocusElement())) {
        return;
    }
    if (button.focus()) {
        play_sound(kSoundItemFocus);
    }
}

void Prelaunch::start_landing(double now) {
    mLanding   = true;
    mLandingAt = now;
    set_body_class("landing", true);
}

bool Prelaunch::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Menu) {
        return true; // the start menu can't be dismissed with the menu key
    }
    if (mPhase != Phase::Idle) {
        // Any button skips the take-off (README 6), but not a modifier key: DESIGN 7.3 keeps them
        // out, yet Left Shift is pad B on the keyboard and reaches here as the pad's Escape. A
        // command that arrives while a modifier is held is that key, not a button.
        const bool modifier = (SDL_GetModState() & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
        if (!modifier) {
            request_skip();
        }
        return true;
    }
    if (yield_input()) {
        return true;
    }
    if (!mEntranceDone && cmd == NavCommand::Confirm) {
        finish_entrance(); // the first press snaps the entrance; it does not activate anything
        return true;
    }
    if (mItems.empty()) {
        return Document::handle_nav_command(event, cmd);
    }
    if (mPicking) {
        return true; // the file dialog is up: the menu waits for it
    }
    if (cmd == NavCommand::Confirm && focused_item() < 0) {
        // Nothing focused (a click on the background): the press brings the focus back instead of
        // doing nothing; Change and the pill handle their own confirm.
        auto* ctx = mDocument->GetContext();
        auto* el  = ctx != nullptr ? ctx->GetFocusElement() : nullptr;
        const bool onExtra = (mChangeDisc && mChangeDisc->contains(el)) || (mUpdatePill && mUpdatePill->contains(el));
        if (!onExtra) {
            if (restore_focus()) {
                play_sound(kSoundItemFocus);
            }
            return true;
        }
    }
    // Spatial: the menu is a column; "Change" and the update pill share the bottom row. Up and
    // Down run through the column and wrap through the bottom row (Change, else the pill); Right
    // from the column or from Change reaches the pill, Left from the pill reaches Change.
    auto* context = mDocument->GetContext();
    auto* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    const auto shown = [](const std::unique_ptr<Button>& b) {
        return b != nullptr && b->root()->IsVisible(true) ? b.get() : nullptr;
    };
    Button* change = shown(mChangeDisc);
    Button* pill   = shown(mUpdatePill);
    const int count = static_cast<int>(mItems.size());
    const int index = focused_item();
    const bool onChange = change != nullptr && change->contains(focused);
    const bool onPill   = pill != nullptr && pill->contains(focused);
    Button* bottom = change != nullptr ? change : pill;
    const auto go = [this](Button* b) {
        if (b != nullptr) {
            focus_extra(b->root());
        }
    };
    if (onChange || onPill) {
        switch (cmd) {
        case NavCommand::Up:
            focus_item(count - 1, true);
            return true;
        case NavCommand::Down:
            focus_item(0, true);
            return true;
        case NavCommand::Right:
            if (onChange) {
                go(pill);
            }
            return true;
        case NavCommand::Left:
            if (onPill) {
                go(change);
            }
            return true;
        default:
            return Document::handle_nav_command(event, cmd);
        }
    }
    switch (cmd) {
    case NavCommand::Up:
        if (index > 0) {
            focus_item(index - 1, true);
        } else if (bottom != nullptr) {
            go(bottom);
        } else {
            focus_item(count - 1, true);
        }
        return true;
    case NavCommand::Down:
        if (index >= 0 && index < count - 1) {
            focus_item(index + 1, true);
        } else if (index == count - 1 && bottom != nullptr) {
            go(bottom);
        } else {
            focus_item(0, true);
        }
        return true;
    case NavCommand::Right:
        go(pill);
        return true;
    case NavCommand::Left:
        return true;
    default:
        break;
    }
    return Document::handle_nav_command(event, cmd);
}

void Prelaunch::finish_entrance() {
    if (mEntranceDone || mDocument == nullptr) {
        return;
    }
    mEntranceDone = true;
    const bool snapped = now_seconds() - mOpenedAt < kEntranceSeconds;
    if (snapped || !mEntranceStarted) {
        set_body_class("intro-skip", true);
        set_body_class("animate-in", true);
    }
    // The end states equal the plain styles, so dropping .intro-item changes nothing on screen; it
    // lets the take-off rules apply at their normal specificity.
    Rml::ElementList items;
    mDocument->GetElementsByClassName(items, "intro-item");
    for (Rml::Element* element : items) {
        element->SetClass("intro-item", false);
    }
    for (auto& item : mItems) {
        item.button->root()->SetClass("anim-done", true);
    }
}

// ---------------------------------------------------------------------------------------- disc

void Prelaunch::refresh_disc() {
    const std::string& setting = settings().backend.discPath.get();
    if (setting != mShownDiscSetting) {
        mRejectedPath.clear(); // a new disc replaces the note about a dropped file
        mPickFailed = false;   // ... and the one about the file dialog
    }
    mShownDiscSetting = setting;
    mSwitchShown      = false;
    if (!mGameRunning) {
        mDiscPath = setting;
    }
    mDisc = disc::inspect(mDiscPath);
    if (mDocument == nullptr) {
        return;
    }
    // A dropped file that can't be used is shown (with its name) but never saved.
    const bool rejected = !mRejectedPath.empty();
    const disc::Info& shown = rejected ? mRejected : mDisc;
    const std::string& shownPath = rejected ? mRejectedPath : mDiscPath;
    // Over the game, a different disc chosen in the menu only applies after a restart.
    const bool pending = !rejected && mGameRunning && !setting.empty() && setting != mDiscPath;
    const char* status = "none";
    std::string label  = disc::state_label(disc::State::None);
    const char* hint   = "";
    std::string hintText;
    const disc::Info chosen = pending ? disc::inspect(setting) : disc::Info{};
    if (pending && chosen.state == disc::State::Ready) {
        status = "pending";
        // On the menu a usable different disc always relaunches (live_apply): say so from the first
        // frame, before the applier (which runs after the menu in the frame) has picked it up.
        mSwitchShown = launcher::live::pending_relaunch();
        label        = "Switching disc...";
    } else if (pending) {
        // The chosen file cannot be used: its own state and hint; the booted disc stays.
        label    = disc::state_label(chosen.state);
        status   = chosen.state == disc::State::Missing || chosen.state == disc::State::Invalid ? "bad" : "mismatch";
        hint     = status;
        hintText = chosen.state == disc::State::Missing   ? "It may have been moved or renamed. Choose it again."
                   : chosen.state == disc::State::Invalid ? fmt::format("Choose a GameCube disc image: {}.", disc::kFormatListNoBreak)
                                                          : fmt::format("{} needs the USA GameCube release (GPVE01).", branding::kProductName);
        hintText += " The game keeps using the current disc.";
    } else {
        label = disc::state_label(shown.state);
        switch (shown.state) {
        case disc::State::Ready:
            status = "good";
            break;
        case disc::State::None:
            break;
        case disc::State::Unsupported:
        case disc::State::WrongGame:
            status   = "mismatch";
            hint     = "mismatch";
            hintText = fmt::format("{} needs the USA GameCube release (GPVE01).", branding::kProductName);
            break;
        case disc::State::Missing:
            status   = "bad";
            hint     = "bad";
            hintText = "It may have been moved or renamed. Choose it again.";
            break;
        case disc::State::Invalid:
            status   = "bad";
            hint     = "bad";
            // No lone "TGC." on a line of its own.
            hintText = fmt::format("Choose a GameCube disc image: {}.", disc::kFormatListNoBreak);
            break;
        }
        if (rejected && mDisc.state == disc::State::Ready) {
            hintText += " Your disc is unchanged.";
        }
    }
    // Drag and drop works on the pre-disc screen: say so where a disc is wanted.
    if (!mGameRunning && !pending && hint[0] == '\0' && shown.state == disc::State::None) {
        hint     = "tip";
        hintText = "You can also drop a disc image onto this window.";
    }
    if (mPickFailed && !rejected) {
        hint     = "mismatch";
        hintText = "Couldn't open a file dialog. Drop a disc image onto this window instead.";
    }
    if (auto* el = mDocument->GetElementById("disc-status")) {
        el->SetAttribute("status", status);
    }
    set_text_content(mDocument->GetElementById("disc-status-label"), label);
    if (auto* el = mDocument->GetElementById("disc-hint")) {
        el->SetClass("mismatch", hint == std::string_view("mismatch"));
        el->SetClass("bad", hint == std::string_view("bad"));
        el->SetClass("tip", hint == std::string_view("tip"));
        set_text_content(el, hintText);
    }

    // "GameCube • USA • GPVE01" for a disc that reads, else the file's name, then "Change".
    // Hidden when no disc is chosen: Play picks one.
    std::string detail;
    if (shown.state == disc::State::Ready && !shown.gameId.empty()) {
        detail = "GameCube \xE2\x80\xA2 ";
        if (const char* region = region_name(shown.gameId)) {
            detail += region;
            detail += " \xE2\x80\xA2 ";
        }
        detail += shown.gameId;
    } else if (!shownPath.empty()) {
        detail = file_name(shownPath);
    }
    if (auto* el = mDocument->GetElementById("disc-version")) {
        set_text_content(el, detail);
        el->SetClass("empty", detail.empty());
    }
    if (auto* line = mDocument->GetElementById("disc-line")) {
        line->SetClass("empty", shownPath.empty() && setting.empty());
    }
}

void Prelaunch::refresh_picking() {
    const bool picking = borealis::file_select::busy();
    if (picking == mPicking || mDocument == nullptr) {
        return;
    }
    mPicking = picking;
    set_body_class("picking", picking);
    if (picking) {
        mPickFailed = false; // a dialog did open this time
        if (auto* el = mDocument->GetElementById("disc-status")) {
            el->SetAttribute("status", "picking");
        }
        set_text_content(mDocument->GetElementById("disc-status-label"), "Waiting for the file dialog...");
    } else {
        refresh_disc();
    }
}

void Prelaunch::drop_hover(bool on) {
    set_body_class("drop-hover", on && mPhase == Phase::Idle && !mPicking);
}

void Prelaunch::drop_file(const std::string& path) {
    drop_hover(false);
    if (mPhase != Phase::Idle || mPicking || path.empty()) {
        return;
    }
    const disc::Info info = disc::inspect(path);
    if (info.state == disc::State::Ready) {
        std::fprintf(stderr, "mppm: dropped disc image: %s\n", path.c_str());
        mRejectedPath.clear();
        if (settings().backend.discPath.get() == path) {
            refresh_disc();
        } else {
            settings().backend.discPath.set(path); // update() refreshes, like a picked file
        }
        return;
    }
    std::fprintf(stderr, "mppm: dropped file is not a usable disc: %s\n", path.c_str());
    mPickFailed   = false;
    mRejectedPath = path;
    mRejected     = info;
    refresh_disc();
}

void Prelaunch::pick_disc() {
    if (borealis::file_select::busy() || mPhase != Phase::Idle || yield_input()) {
        return;
    }
    // Confirm mashed through the first-run card lands here once the card closes: a press within
    // 0.6 s of a window closing, or within 0.5 s of such a press (for up to 6 s), is ignored.
    if (mUncoveredAt != 0) {
        const uint64_t now = SDL_GetTicks();
        const bool justClosed = now - mUncoveredAt < 600;
        const bool mashing    = mLastPickGuard != 0 && now - mLastPickGuard < 500 && now - mUncoveredAt < 6000;
        if (justClosed || mashing) {
            mLastPickGuard = now;
            return;
        }
    }
    borealis::file_select::FileOptions options;
    options.parentWindow = aurora::window::get_sdl_window();
    options.filters      = {{"GameCube Disc Images", disc::kImageFilter}, {"All Files", "*"}};
    borealis::file_select::open_file(std::move(options), [](borealis::file_select::Result result) {
        using borealis::file_select::Status;
        // SDL's zenity backend reports a cancel as one empty location: that is not a choice, and
        // must never clear the remembered disc.
        if (result.status == Status::Selected && !result.locations.empty() && !result.locations.front().empty()) {
            std::lock_guard lk(sPickMutex);
            sPickedPath = result.locations.front();
        } else if (result.status == Status::Failed || result.status == Status::Unsupported) {
            // No file dialog could open at all (no portal, no zenity): say so, or the main button
            // of the pre-disc screen looks dead. A cancel stays silent.
            std::fprintf(stderr, "mppm: start menu: the file dialog could not open: %s\n",
                result.message.empty() ? "no backend" : result.message.c_str());
            std::lock_guard lk(sPickMutex);
            sPickFailed = true;
        }
    });
}

// ---------------------------------------------------------------------------------------- status

void Prelaunch::refresh_update_status() {
    namespace upd = launcher::update_check;
    upd::poll();
    auto* status = mDocument->GetElementById("update-status");
    if (status == nullptr) {
        return;
    }
    if (upd::state() == upd::State::Checking) {
        if (mCheckingSince <= 0.0) {
            mCheckingSince = now_seconds();
        }
        // Only a slow check shows its row, and never during the entrance (DESIGN 6.7, 6.8).
        if (mEntranceDone && now_seconds() - std::max(mCheckingSince, mOpenedAt + kEntranceSeconds) >= kCheckingShowSeconds &&
            !status->HasAttribute("state"))
        {
            status->SetAttribute("state", "checking");
        }
    } else {
        mCheckingSince = 0.0;
    }
    if (upd::generation() != mUpdateGeneration) {
        mUpdateGeneration = upd::generation();
        switch (upd::state()) {
        case upd::State::Checking:
            break; // shown above once it has run for a while
        case upd::State::Available:
            status->SetAttribute("state", "available");
            set_text_content(mDocument->GetElementById("update-pill-label"),
                fmt::format("Update available: v{}", upd::latest_version()));
            break;
        case upd::State::Failed:
            status->SetAttribute("state", "failed");
            status->SetClass("faded", false);
            mFailedAt = now_seconds();
            break;
        case upd::State::Idle:
        case upd::State::UpToDate:
        case upd::State::Quiet:
            status->RemoveAttribute("state");
            break;
        }
    }
    if (upd::state() == upd::State::Failed && !status->IsClassSet("faded") &&
        now_seconds() - mFailedAt > kFailedNoticeSeconds)
    {
        status->SetClass("faded", true);
    }
}

void Prelaunch::set_pointer_input(bool on) {
    if (mPointerInput != on) {
        mPointerInput = on;
        set_body_class("pointer-input", on);
    }
}

void Prelaunch::track_input_device() {
    float x = 0.0f;
    float y = 0.0f;
    const uint32_t buttons = SDL_GetMouseState(&x, &y);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    bool keyPressed = false;
    if (keys != nullptr && count > 0) {
        if (mKeysDown.size() != static_cast<size_t>(count)) {
            mKeysDown.assign(static_cast<size_t>(count), 0);
        }
        for (int i = 0; i < count; i++) {
            const uint8_t down = keys[i] ? 1 : 0;
            keyPressed = keyPressed || (down != 0 && mKeysDown[i] == 0);
            mKeysDown[i] = down;
        }
    }
    const bool pad = input::last_input_gamepad();
    const bool padPressed = pad && !mPadInput;
    mPadInput = pad;
    // The first frames only take a baseline: the pointer's position settles as the window maps.
    bool pointer = false;
    if (mPointerBaseline && mEntranceStarted) {
        const float moved = std::abs(x - mMouseX) + std::abs(y - mMouseY);
        pointer = moved > 2.0f || (buttons & ~mMouseButtons) != 0;
    }
    mPointerBaseline = mEntranceStarted;
    mMouseX = x;
    mMouseY = y;
    mMouseButtons = buttons;
    if (keyPressed || padPressed) {
        set_pointer_input(false);
    } else if (pointer) {
        set_pointer_input(true);
    }
}

void Prelaunch::refresh_prompts() {
    // A pad shows its face button, a keyboard the key (plain letters, no platform button art).
    auto* glyph = mDocument != nullptr ? mDocument->GetElementById("prompt-confirm") : nullptr;
    if (glyph == nullptr) {
        return;
    }
    const bool pad = input::last_input_gamepad();
    if (glyph->IsClassSet("pad") != pad) {
        glyph->SetClass("pad", pad);
        set_text_content(glyph, pad ? "A" : "Enter");
    }
}

// ---------------------------------------------------------------------------------------- update

void Prelaunch::update() {
    if (mDocument == nullptr) {
        return;
    }
    const double now = now_seconds();
    const scene::State sceneState = scene::state();
    // The first frame is presented on its own, before a slow first event pump (launcher.cpp
    // present_first_frame): the entrance starts from the second, so none of it is lost to that gap.
    if (!mEntranceStarted && ++mUpdates >= 2) {
        // Class changes after the first layout start the entrance transitions (6.8).
        mEntranceStarted = true;
        mOpenedAt        = now;
        // A live scene is loading behind the backdrop: the backdrop's ship lands on the horizon
        // before the cross-fade, so the cartoon ship never hangs over the real one (6.9).
        mWantLanding = mGameRunning && sceneState == scene::State::Loading && scene::live();
        if (mContinuation) {
            finish_entrance();
        } else {
            set_body_class("animate-in", true);
        }
    }
    if (mEntranceStarted && !mEntranceDone && now - mOpenedAt >= kEntranceSeconds) {
        finish_entrance();
    }
    if (mEntranceDone && !mPresetChecked) {
        // SETTINGS 12: the first-run card comes on top of the menu once its entrance is over, so
        // the two entrances never play over each other.
        mPresetChecked = true;
        show_first_run_preset_if_needed();
    }
    remember_focus();
    // Consent first: nothing contacts GitHub before the first-run question is answered.
    if (settings().interface.presetChosen.get()) {
        launcher::update_check::begin();
    }

    std::optional<std::string> picked;
    bool pickFailed = false;
    {
        std::lock_guard lk(sPickMutex);
        picked.swap(sPickedPath);
        pickFailed = std::exchange(sPickFailed, false);
    }
    bool clearedNote = false;
    if (pickFailed && !mPickFailed) {
        mPickFailed = true;
        clearedNote = true; // refresh below
    }
    if (picked && !picked->empty()) {
        clearedNote = !mRejectedPath.empty();
        mRejectedPath.clear();
        settings().backend.discPath.set(*picked);
    }
    if (settings().backend.discPath.get() != mShownDiscSetting || clearedNote ||
        (mGameRunning && launcher::live::pending_relaunch() != mSwitchShown))
    {
        refresh_disc();
    }
    refresh_picking();

    // Backdrop to live scene (6.9): the ship lands (once the entrance is over), then the backdrop
    // fades when the scene has drawn and the landing is done.
    const bool sceneDrawn = sceneState == scene::State::Ready || sceneState == scene::State::Takeoff ||
                            sceneState == scene::State::Fading || sceneState == scene::State::Done;
    if ((sceneState == scene::State::Failed || (sceneState == scene::State::Parked && !scene::live())) &&
        (mWantLanding || mLanding || mLanded))
    {
        // No scene on screen (failed, or parked by Live Start Menu): the backdrop stays, with its
        // ship (the backdrop take-off uses it). Parked with Live back on is the scene returning (the
        // unpark can wait out a music swap): the landing below is armed once, not reset each frame.
        mWantLanding = false;
        mLanding     = false;
        mLanded      = false;
        set_body_class("landing", false);
        set_body_class("landed", false);
    }
    // The scene is coming back (Live Start Menu turned on over a parked scene): the ship lands again.
    if (mGameRunning && mEntranceStarted && !mWantLanding && !mLanded && !sceneDrawn && scene::live() &&
        mPhase == Phase::Idle && (sceneState == scene::State::Loading || sceneState == scene::State::Parked))
    {
        mWantLanding = true;
    }
    if (mWantLanding && !mLanding && !mLanded && mEntranceDone && !mCovered) {
        start_landing(now);
    }
    if (mWantLanding && !mLanding && !mLanded && sceneDrawn && mCovered) {
        // Drawn while Settings covers the menu: no flourish nobody sees, or the scene would stay
        // hidden until Settings closes.
        mLanded = true;
        set_body_class("landed", true);
    }
    if (mLanding && now - mLandingAt >= kLandingSeconds) {
        mLanding = false;
        mLanded  = true;
        set_body_class("landed", true);
    }
    // Two-way (live_options 3.4): the backdrop also comes back over a shown scene while a structural
    // change (area, Live Start Menu) is applied behind it. Fixed from Play on: a parked or failed
    // scene ends at once (DONE) and must not uncover the title loading under the backdrop take-off.
    const bool show = sceneDrawn && !scene::cover_wanted() && (!mWantLanding || mLanded);
    if (show != mSceneReady && mPhase == Phase::Idle) {
        if (!show && !mSceneSwap) {
            mSceneSwap = true; // the next fade-out is the quick one
            set_body_class("scene-swap", true);
        }
        mSceneReady = show;
        set_body_class("scene-ready", show);
    }

    // Play's face: "Choose Disc Image" without a disc, "Landing..." until the scene shows or while a
    // change is applied behind the backdrop.
    if (mPlay != nullptr && mPhase == Phase::Idle) {
        const bool waiting = mGameRunning && (scene::busy() || (mWantLanding && sceneDrawn && !mSceneReady));
        const bool haveDisc = mGameRunning || mDisc.state == disc::State::Ready;
        set_text_content(mPlay->label, waiting ? "Landing..." : haveDisc ? "Play" : "Choose Disc Image");
        mPlay->icon->SetClass("rocket", !waiting && haveDisc && !mPicking);
        mPlay->icon->SetClass("album", !waiting && !haveDisc && !mPicking);
        mPlay->icon->SetClass("loading", waiting || mPicking);
        mPlay->button->root()->SetClass("long", !waiting && !haveDisc);
        mPlay->button->set_disabled(waiting);
        // The halo pulses while Play (or nothing else) has focus.
        const int focusedIndex = focused_item();
        mPlay->button->root()->SetClass("halo", focusedIndex <= 0 && !waiting);
    }

    refresh_update_status();
    track_input_device();
    refresh_prompts();
    for (auto& item : mItems) {
        item.button->update();
    }
    if (mChangeDisc != nullptr) {
        mChangeDisc->update();
    }
    if (mUpdatePill != nullptr) {
        mUpdatePill->update();
    }
    if (mPhase != Phase::Idle) {
        step_launch();
    }
}

// ---------------------------------------------------------------------------------------- take-off

void Prelaunch::on_play() {
    if (mPhase != Phase::Idle) {
        request_skip();
        return;
    }
    if (!mEntranceDone) {
        finish_entrance();
        return;
    }
    if (mPicking) {
        return;
    }
    if (!mGameRunning && mDisc.state != disc::State::Ready) {
        pick_disc();
        return;
    }
    if (mPlay != nullptr && mPlay->button->disabled()) {
        return;
    }
    // Restart-only changes are waiting: restart first, straight into the game (SETTINGS 11).
    if ((mGameRunning || mDisc.state == disc::State::Ready) && launcher::restart_pending()) {
        launcher::request_restart(launcher::RestartTarget::Play);
    }
    play_sound(kSoundPlay);
    begin_launch();
}

bool Prelaunch::start_online() {
    if (mPhase != Phase::Idle || mPicking) {
        return false;
    }
    if (!mGameRunning && mDisc.state != disc::State::Ready) {
        std::fprintf(stderr, "mppm: start menu: an online match started without a usable disc\n");
        return false;
    }
    // Close what is open over the menu, top first: the Multiplayer window closes itself on
    // Starting, but a member may have Settings or a modal up when the host starts.
    std::vector<Document*> above;
    bool reached = false;
    for_each_visible_document([&](Document& doc) {
        if (&doc == this) {
            reached = true;
        } else if (reached && !doc.permanent()) {
            above.push_back(&doc);
        }
    });
    for (auto it = above.rbegin(); it != above.rend(); ++it) {
        (*it)->pop();
    }
    finish_entrance();
    mPresetChecked = true; // the first-run card must not come up over the take-off
    std::fprintf(stderr, "mppm: start menu: online match starting\n");
    play_sound(kSoundPlay);
    begin_launch();
    return true;
}

bool Prelaunch::live_scene() const {
    const scene::State s = scene::state();
    return s == scene::State::Loading || s == scene::State::Ready || s == scene::State::Takeoff ||
           s == scene::State::Fading || s == scene::State::Done;
}

Prelaunch::Ms Prelaunch::launch_ms() const {
    return (now_seconds() - mLaunchAt) * 1000.0;
}

void Prelaunch::set_body_class(const char* name, bool on) {
    if (mDocument != nullptr) {
        mDocument->SetClass(name, on);
    }
}

void Prelaunch::begin_launch() {
    finish_entrance();
    mLaunchAt = now_seconds();
    mCueAt.fill(std::nullopt);
    mSkipped     = false;
    mSkipSent    = false;
    mSceneFade   = false;
    mFlashClosedAt.reset();
    scene::arm_skip();
    if (mGameRunning) {
        mMode      = effective_launch_animation() == LaunchAnimation::Off ? Mode::Off : Mode::Full;
        mNavyFlash = mMode == Mode::Off || settings().interface.reduceMotion.get();
        mScenePlay = live_scene();
        mSceneLive = mScenePlay;
        mBeatSeen     = scene::heartbeat_count();
        mFramesNoBeat = 0;
        scene::send(scene::Command::Play);
    } else {
        // Standalone: no scene, so no choreography. The menu fades to navy and the game boots.
        mMode      = Mode::Off;
        mNavyFlash = true;
        mScenePlay = false;
        mSceneLive = false;
    }
    // Hover transitions and the entrance's skip switch would outrank the take-off rules. Play keeps
    // its focused face (padding and sprout) through the press, so the label does not snap back
    // under the sprout (launch-focused, rcss).
    set_body_class("intro-skip", false);
    if (mPlay != nullptr) {
        auto* ctx = mDocument->GetContext();
        Rml::Element* root = mPlay->button->root();
        const bool focused = focused_item() == 0 || (ctx != nullptr && ctx->GetHoverElement() != nullptr &&
                                                        mPlay->button->contains(ctx->GetHoverElement()));
        root->SetClass("launch-focused", focused);
    }
    for (auto& item : mItems) {
        item.button->root()->SetClass("anim-done", false);
    }
    set_body_class("launching", true);
    mNominal = mMode == Mode::Off ? kOffTimes : mScenePlay ? kFullTimes : kBackdropTimes;
    // Reduce Motion keeps the full take-off but the plain-fade UI (no letterbox, no bloom).
    const bool calm = mMode == Mode::Full && settings().interface.reduceMotion.get();
    set_body_class(mMode == Mode::Off ? "launch-off" : calm ? "launch-short" : "launch-full", true);
    set_body_class("launch-navy", mNavyFlash);
    set_body_class("launch-backdrop", !mSceneReady);
    mPhase   = Phase::Launching;
    mPhaseAt = 0.0;
    step_launch();
}

void Prelaunch::request_skip() {
    if (mPhase != Phase::Launching || mSkipped || launch_ms() < kSkipFromMs || !scene::skip_armed()) {
        return;
    }
    std::fprintf(stderr, "mppm: start menu: skip at %.0f ms\n", launch_ms());
    mSkipped = true;
    if (mScenePlay) {
        scene::send(scene::Command::Skip);
        mSkipSent = true;
    }
    set_body_class("launch-skip", true);
    on_scene_cue(kExitFrame);
}

void Prelaunch::resend_skip() {
    // The scene reads a command once and may still refuse a skip (its take-off ignores one in its
    // first half second of scene time, which can be well over 0.5 s of wall time on a slow frame):
    // repeat it every frame until the scene starts its end fade, so an early skip is never lost.
    if (!mSkipSent || !mScenePlay) {
        return;
    }
    if (scene::state() == scene::State::Takeoff) {
        scene::send(scene::Command::Skip);
    } else if (scene::state() != scene::State::Ready && scene::state() != scene::State::Loading) {
        mSkipSent = false; // fading, done or gone: nothing more to tell it
    }
}

void Prelaunch::on_scene_cue(int cue) {
    if (mPhase == Phase::Idle || cue < 0 || cue >= kCueCount || mCueAt[cue].has_value()) {
        return;
    }
    mCueAt[cue] = launch_ms();
    switch (cue) {
    case kIgnite:
        set_body_class("launch-ignite", !mNavyFlash);
        break;
    case kLiftoff:
        set_body_class("launch-liftoff", !mNavyFlash);
        break;
    case kExitFrame:
        if (mPhase == Phase::Launching) {
            enter_phase(Phase::Flash);
        }
        break;
    default:
        break;
    }
}

void Prelaunch::enter_phase(Phase phase) {
    mPhase   = phase;
    mPhaseAt = launch_ms();
    std::fprintf(stderr, "mppm: start menu: %s at %.0f ms\n", phase_name(static_cast<int>(phase)), mPhaseAt);
    switch (phase) {
    case Phase::Flash:
        // With the live scene fading itself to the flash colour underneath, the flash only veils
        // it at first, so the lift-off dissolves into the colour instead of being cut by a flat
        // card; it closes once the scene's fade is nearly done (kSceneFadeMs) or at the hand-over.
        mSceneFade = mScenePlay && !mNavyFlash && !mSkipped;
        set_body_class("launch-scene-fade", mSceneFade);
        set_body_class("launch-flash", true);
        break;
    case Phase::Wait:
        set_body_class("launch-wait", true);
        break;
    case Phase::Handoff:
        set_body_class("launch-scene-fade", false);
        // Everything but the flash goes while it is opaque. The menu stops counting as open, which
        // hands the window shape back to the user's aspect setting (ASPECT 5) and unblocks input.
        set_body_class("launch-handoff", true);
        mPendingClose = true;
        // The title takes input from here on, while the flash still dissolves over it: unblock the
        // pad now, not at the next key event (which would itself be lost to the block).
        input::sync_input_block();
        break;
    case Phase::Reveal:
        set_body_class("launch-reveal", true);
        break;
    case Phase::Done:
        if (mGameRunning) {
            Document::hide(true);
        } else {
            mLaunchRequested = true; // the boot flow closes the document and boots the game
        }
        break;
    case Phase::Idle:
    case Phase::Launching:
        break;
    }
}

void Prelaunch::step_launch() {
    const Ms t = launch_ms();
    resend_skip();
    const double* nominal = mNominal != nullptr ? mNominal : kOffTimes;
    if (mMode == Mode::Full && mPhase == Phase::Launching && scene::consume_any_press()) {
        request_skip();
    }

    // While the live scene runs, its cues alone drive the timeline (7.1), at whatever speed the
    // game runs. It stops counting when it fails, disappears or hangs; the nominal times take
    // over from there.
    const scene::State sceneState = scene::state();
    if (mScenePlay) {
        if (sceneState == scene::State::Failed || sceneState == scene::State::None) {
            mScenePlay = false;
            std::fprintf(stderr, "mppm: start menu: scene gone at %.0f ms, using the nominal times\n", t);
        } else if (sceneState != scene::State::Done) {
            // Hung only if the launcher itself kept drawing frames meanwhile: after a whole-process
            // pause the age is large but no frame went by without a poll.
            const uint64_t beat = scene::heartbeat_count();
            mFramesNoBeat       = beat != mBeatSeen ? 0 : mFramesNoBeat + 1;
            mBeatSeen           = beat;
            const double age    = scene::heartbeat_age_ms();
            if ((age >= kHeartbeatStallMs && mFramesNoBeat >= kHeartbeatStallFrames) || t >= kSceneMaxMs) {
                mScenePlay = false;
                std::fprintf(stderr, "mppm: start menu: scene hung (no frame for %.0f ms) at %.0f ms, using the nominal times\n",
                    age, t);
            }
        }
    }
    // A parked or failed scene ends at once on Play, before the title has even started to load: its
    // DONE is not a hand-over, and the launch runs on the nominal times (H2).
    if (sceneState == scene::State::Done && mSceneLive) {
        on_scene_cue(kHandoff);
    }
    if (mScenePlay && mBloom != nullptr && mCueAt[kIgnite].has_value()) {
        float x = 0.0f;
        float y = 0.0f;
        const bool onScreen = scene::ship_screen(x, y);
        if (onScreen) {
            mBloom->SetProperty(Rml::PropertyId::Left, Rml::Property(x * 100.0f, Rml::Unit::PERCENT));
            mBloom->SetProperty(Rml::PropertyId::Top, Rml::Property(y * 100.0f, Rml::Unit::PERCENT));
        }
        // The glow leaves with the ship (0.12 s) instead of hanging where it left the frame.
        set_body_class("launch-bloom-off", !onScreen);
    }
    // The skip hint: Full only, in at 1.2 s, dimmed after 4 s; the flash (exit_frame or a skip)
    // hides it (rcss).
    const bool hint = mMode == Mode::Full && mPhase == Phase::Launching && !mSkipped;
    set_body_class("skip-hint-on", hint && t >= kSkipHintShowMs);
    set_body_class("skip-hint-dim", hint && t >= kSkipHintDimMs);

    switch (mPhase) {
    case Phase::Launching: {
        if (!mScenePlay) {
            // No scene to follow: the nominal times (lift-off keeps its distance to an ignite
            // that did arrive).
            for (int cue : {kIgnite, kLiftoff, kExitFrame}) {
                if (nominal[cue] < 0.0 || mCueAt[cue].has_value()) {
                    continue;
                }
                double due = nominal[cue];
                if (cue == kLiftoff && mCueAt[kIgnite].has_value()) {
                    due = *mCueAt[kIgnite] + (nominal[kLiftoff] - nominal[kIgnite]);
                }
                if (t >= due) {
                    on_scene_cue(cue);
                }
            }
        }
        // The game ended the scene early (a failed scene ends at once on Play): hurry to the flash.
        if (mCueAt[kHandoff].has_value() && mPhase == Phase::Launching) {
            mSkipped = true;
            set_body_class("launch-skip", true);
            on_scene_cue(kExitFrame);
        }
        break;
    }
    case Phase::Flash: {
        if (mGameRunning && mCueAt[kHandoff].has_value()) {
            // The scene section ended: its last frame was the flash colour, and the frames after it
            // are black until the title draws. The flash turns opaque now (launch-handoff, no
            // transition), whether it was still a veil or partway through closing.
            mSceneFade = false;
            enter_phase(Phase::Handoff);
            break;
        }
        if (mSceneFade) {
            if (mScenePlay && t - mPhaseAt < kSceneFadeMs) {
                break;
            }
            mSceneFade = false; // close the veil: 0.4 s to opaque (rcss)
            set_body_class("launch-scene-fade", false);
            mFlashClosedAt = t;
        }
        const Ms flashMs  = mSkipped ? 250.0 : (mNavyFlash && mMode != Mode::Off) ? 300.0 : 400.0;
        const Ms opaqueAt = mFlashClosedAt ? *mFlashClosedAt + flashMs : mPhaseAt + flashMs;
        if (t < opaqueAt) {
            break;
        }
        if (!mGameRunning) {
            enter_phase(Phase::Done);
            break;
        }
        bool handoff = mCueAt[kHandoff].has_value();
        if (!mScenePlay) {
            // No scene to wait for: the nominal hand-over, never before the flash is opaque.
            double due = mSkipped ? opaqueAt + 50.0 : std::max(nominal[kHandoff], opaqueAt);
            if (!mSceneLive && mGameRunning && mMode == Mode::Off) {
                due = std::max(due, kColdRevealMs); // the title only began loading at Play
            }
            handoff = handoff || t >= due;
        }
        if (handoff) {
            enter_phase(Phase::Handoff);
        } else if (t - opaqueAt >= kHandoffWaitMs) {
            enter_phase(Phase::Wait);
        }
        break;
    }
    case Phase::Wait:
        if (mCueAt[kHandoff].has_value() || !mScenePlay) {
            enter_phase(Phase::Handoff);
        }
        break;
    case Phase::Handoff:
        if (t - mPhaseAt >= kHandoffHoldMs) {
            enter_phase(Phase::Reveal);
        }
        break;
    case Phase::Reveal:
        if (t - mPhaseAt >= (mNavyFlash ? 400.0 : kRevealMs)) {
            enter_phase(Phase::Done);
        }
        break;
    case Phase::Idle:
    case Phase::Done:
        break;
    }
}

} // namespace mppm::ui
