#include "menu_bar.hpp"

#include "branding.hpp"
#include "controller_config.hpp"
#include "menu_registry.hpp"
#include "overlay.hpp"
#include "restart.hpp"
#include "settings.hpp"

#include "ui/input.hpp"
#include "ui/modal.hpp"
#include "ui/settings_window.hpp"
#include "ui/ui.hpp"

#include <borealis/config.hpp>
#include <fmt/format.h>
#include <mppm/launcher.hpp>
#include <p2_host.h>
#include <p2_online.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <string_view>

namespace mppm::ui {
namespace {

using launcher::Menu;
using launcher::MenuEntry;
namespace bar_order = launcher::bar_order;

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/tabbing.rcss" />
    <link type="text/rcss" href="res/rml/popup.rcss" />
    <link type="text/rcss" href="res/rml/ingame.rcss" />
</head>
<body class="in-game-bar">
    <popup id="popup">
        <mark class="orb"><leaf class="l"/><leaf class="r"/></mark>
    </popup>
    <paused-tag id="paused">Paused</paused-tag>
</body>
</rml>
)RML";

MenuBar* sMenuBar = nullptr;

// How far the bar gives way when it does not fit the viewport (Original aspect on a wide window
// confines the UI to the 4:3 picture; small windows): each level keeps the ones before it.
//   1: tighter tabs   2: no clock   3: tighter caps and divider   4: no FPS (the overlay pill
//   still has it)   5: no sprout mark
constexpr int kMaxFitLevel = 5;

// A danger modal whose confirming (last) button is red too (DESIGN 5.11).
class DangerModal final : public Modal {
public:
    explicit DangerModal(Props props) : Modal(std::move(props)) {
        Rml::ElementList buttons;
        mDocument->QuerySelectorAll(buttons, "modal-actions button");
        if (!buttons.empty()) {
            buttons.back()->SetClass("danger", true);
        }
    }
};

// Posts the same shutdown as the window's close button: Aurora turns SDL_EVENT_QUIT into
// AURORA_EXIT, which port/main.cpp handles on its next event pump.
void request_quit() {
    if (const auto status = borealis::config::Registry::global().flush(); !status) {
        std::fprintf(stderr, "mppm: settings: %s\n", status.message.c_str());
    }
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    event.quit.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&event);
}

bool has_entry(const char* id) {
    const auto entries = launcher::all_menu_entries(Menu::InGameBar);
    return std::ranges::any_of(entries, [id](const MenuEntry* entry) { return entry->id == id; });
}

// The v1 entries of SETTINGS 13, registered unless another file already registered the same id
// (which then wins). Mods and Online only reserve their slot: hidden until those features exist
// and register themselves.
void register_default_entries() {
    const auto add = [](MenuEntry entry) {
        if (!has_entry(entry.id.c_str())) {
            launcher::register_menu_entry(Menu::InGameBar, std::move(entry));
        }
    };
    add({.id = "resume", .label = "Resume", .order = bar_order::kResume, .open = [] {
        if (auto* bar = MenuBar::current()) {
            play_sound(kSoundMenuClose);
            bar->close();
        }
    }});
    add({.id = "settings", .label = "Settings", .order = bar_order::kSettings, .open = [] {
        open_settings_window(false, MenuBar::current());
    }});
    add({.id = "controllers", .label = "Controllers", .order = bar_order::kControllers, .open = [] {
        // Opens below the bar (pop_to_or_push covers the top document: MenuBar::cover).
        open_controller_config(0);
    }});
    add({.id = "mods", .label = "Mods", .order = bar_order::kMods, .visible = [] { return false; }});
    add({.id = "online", .label = "Online", .order = bar_order::kOnline, .visible = [] { return false; }});
    add({.id = "start-menu", .label = "Start Menu", .order = bar_order::kStartMenu, .open = [] {
        confirm_progress_loss("Return to the start menu?", "Return",
            [] { launcher::request_restart(launcher::RestartTarget::StartMenu); }, MenuBar::current());
    }});
    add({.id = "quit", .label = "Quit", .order = bar_order::kQuit, .open = [] {
        confirm_progress_loss(fmt::format("Quit {}?", branding::kProductName), "Quit",
            [] { request_quit(); }, MenuBar::current());
    }});
}

} // namespace

// ------------------------------------------------------------------------------------------ bar

MenuBar::MenuBar() : Document(kDocumentSource, false, DocumentScope::MenuBar) {
    mRoot = mDocument->GetElementById("popup");
    mPaused = mDocument->GetElementById("paused");
    mTabBar = std::make_unique<TabBar>(mRoot, TabBar::Props{.autoSelect = false});

    append(mRoot, "divider");
    mStatus = append(mRoot, "bar-status");
    mFps = append(mStatus, "span");
    mFps->SetClass("fps", true);
    mClock = append(mStatus, "span");
    mClock->SetClass("clock", true);
    auto* prompt = append(mStatus, "prompt");
    mPromptGlyph = append_text_element(prompt, "glyph", "Esc");
    mPromptGlyph->SetClass("key", true);
    append_text_element(prompt, "label", "Close");
    // The prompt is also a close button for the mouse.
    listen(prompt, Rml::EventId::Click, [this](Rml::Event&) {
        if (visible()) {
            play_sound(kSoundMenuClose);
            close();
        }
    });

    // Hide the document once the slide-out has finished.
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") && Document::visible()) {
            Document::hide(mPendingClose);
        }
    });
    // A click on the scrim area (outside the bar) resumes, like Escape.
    listen(Rml::EventId::Click, [this](Rml::Event& event) {
        if (visible() && event.GetTargetElement() == mDocument) {
            play_sound(kSoundMenuClose);
            close();
        }
    });

    build_tabs();
    refresh_prompt();
    sMenuBar = this;
}

MenuBar::~MenuBar() {
    if (sMenuBar == this) {
        sMenuBar = nullptr;
    }
}

MenuBar* MenuBar::current() {
    return sMenuBar;
}

void MenuBar::build_tabs() {
    mTabBar->clear_tabs();
    mTabIds.clear();
    for (const MenuEntry* entry : launcher::menu_entries(Menu::InGameBar)) {
        auto& button = mTabBar->add_tab(entry->label, [this, id = entry->id, open = entry->open] {
            // Entries act; they are not pages, so no tab stays selected. Focus comes back to this
            // entry when whatever it opens (a window, a confirmation) closes.
            mFocusedId = id;
            mTabBar->set_active_tab(-1);
            play_sound(kSoundClick);
            if (open) {
                open();
            }
        });
        button.root()->SetAttribute("entry", entry->id);
        mTabIds.push_back(entry->id);
    }
    mFitLevel = -1; // the entries changed: measure again
    if (mCovered) {
        mark_open_entry(true); // rebuilt under a window: keep its entry marked
    }
}

// While a window opened from the bar (Settings, Controllers) is on screen, its entry keeps the
// selected look of DESIGN.md 5.9 (text, glow, sprout), as Dusklight keeps SETTINGS lit. Actions
// and their confirmation modals (Resume, Reset Game, Start Menu, Quit) mark nothing. The pseudo
// class is set directly: TabBar::set_active_tab would focus the tab and run its action again.
void MenuBar::mark_open_entry(bool on) {
    static constexpr std::string_view kActions[] = {"resume", "reset", "start-menu", "quit"};
    const bool window = !mFocusedId.empty() && std::ranges::find(kActions, mFocusedId) == std::end(kActions);
    for (const auto& id : mTabIds) {
        Rml::Element* tab = mDocument->QuerySelector("tab[entry=\"" + id + "\"]");
        if (tab != nullptr) {
            tab->SetPseudoClass("selected", on && window && id == mFocusedId);
        }
    }
}

void MenuBar::refresh_entries() {
    build_tabs();
    if (visible()) {
        focus();
    }
}

void MenuBar::show() {
    if (mCovered) { // closed under a window by other means (Start Menu, close_all_documents)
        mCovered = false;
        mDocument->SetClass("covered", false);
        set_window_top_inset(0.0f);
    }
    // The entries are asked again on every open: visibility can change while the game runs.
    build_tabs();
    Document::show();
    mRoot->SetAttribute("open", "");
    mPendingClose = false;
    mTabBar->set_active_tab(-1);
    update_status();
    fit_to_width();
    focus();
}

void MenuBar::hide(bool close) {
    const auto title = mTabBar->focused_tab_title();
    for (const MenuEntry* entry : launcher::menu_entries(Menu::InGameBar)) {
        if (entry->label == title) {
            mFocusedId = entry->id;
        }
    }
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
    }
}

void MenuBar::cover() {
    mWasVisible = visible();
    if (!mWasVisible) {
        return;
    }
    // Keep the bar open and remember the entry the window came from; large windows keep clear of
    // it (body padding 96dp at the default size: the bar's 16dp + 64dp and a 16dp gap).
    const auto title = mTabBar->focused_tab_title();
    for (const MenuEntry* entry : launcher::menu_entries(Menu::InGameBar)) {
        if (entry->label == title) {
            mFocusedId = entry->id;
        }
    }
    mCovered = true;
    mDocument->SetClass("covered", true);
    mark_open_entry(true);
    update_window_inset();
}

void MenuBar::uncover() {
    if (!mCovered) {
        // No window of the bar's covered it (a modal over the game closed, e.g. the wait pill's Drop
        // confirmation): leave the bar as it is. A "was visible" left from an earlier window must
        // not open a bar the player has closed since.
        mWasVisible = false;
        if (visible()) {
            focus();
        }
        return;
    }
    mCovered = false;
    mDocument->SetClass("covered", false);
    mark_open_entry(false);
    set_window_top_inset(0.0f);
    Document::uncover();
}

void MenuBar::update_window_inset() {
    auto* context = mDocument->GetContext();
    if (!mCovered || context == nullptr) {
        return;
    }
    const float gap = 16.0f * context->GetDensityIndependentPixelRatio();
    set_window_top_inset(std::round(mRoot->GetAbsoluteTop() + mRoot->GetOffsetHeight() + gap));
}

void MenuBar::close() {
    hide(false);
    input::sync_input_block();
}

void MenuBar::open_over(std::unique_ptr<Document> document) {
    push(std::move(document));
}

bool MenuBar::focus() {
    if (!mFocusedId.empty()) {
        const auto it = std::ranges::find(mTabIds, mFocusedId);
        if (it != mTabIds.end() && mTabBar->focus_tab(static_cast<int>(it - mTabIds.begin()))) {
            return true;
        }
    }
    return mTabBar->focus();
}

bool MenuBar::visible() const {
    return mRoot != nullptr && mRoot->HasAttribute("open");
}

void MenuBar::update() {
    if (mCovered) {
        // The window above can be hidden and shown again without closing (the menu key toggles
        // it): the bar goes and comes back with it, so it never sits there without focus.
        bool above = false;
        for_each_visible_document([this, &above](Document& doc) {
            if (&doc != this) {
                above = true;
            }
        });
        if (above != visible()) {
            if (above) {
                Document::show();
                mRoot->SetAttribute("open", "");
                mPendingClose = false;
            } else {
                mRoot->RemoveAttribute("open");
            }
            input::sync_input_block();
        }
    }
    if (visible()) {
        update_status();
        refresh_prompt();
        fit_to_width();
        update_window_inset(); // follows the bar through resizes and fit levels
    }
    update_paused_tag();
    mTabBar->update();
    Document::update();
}

void MenuBar::update_status() {
    // FPS (when the counter is on): the same sample as the overlay's pill, so both agree.
    const bool showFps = settings().video.fpsCounter.get() != FpsCounter::Off;
    if (showFps != mShowFps) {
        mShowFps = showFps;
        mFps->SetClass("off", !showFps);
        mFitLevel = -1;
        mFitBase = 0; // the status block changed width: measure from the roomiest layout
    }
    if (showFps) {
        if (const std::string& text = fps_text(); text != mFpsText) {
            mFpsText = text;
            set_text_content(mFps, mFpsText);
        }
    }

    // The local wall clock, as a game console's menu shows it.
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%H:%M", &local) > 0 && mClockText != buf) {
        mClockText = buf;
        set_text_content(mClock, mClockText);
    }
}

void MenuBar::fit_to_width() {
    auto* context = mDocument->GetContext();
    if (context == nullptr) {
        return;
    }
    const Rml::Vector2i dims = context->GetDimensions();
    const float ratio = context->GetDensityIndependentPixelRatio();
    if (mFitLevel < 0 || dims != mFitDims || ratio != mFitRatio) {
        // Start from the level the last measurement settled on for this viewport (0 after a
        // resize), so a reopen does not flash the roomy layout; a wider entry set steps up again.
        if (dims != mFitDims || ratio != mFitRatio) {
            mFitBase = 0;
        }
        mFitDims = dims;
        mFitRatio = ratio;
        mFitLevel = mFitBase;
        mFitSettled = false;
        apply_fit_level();
        return;
    }
    if (mFitSettled) {
        return;
    }
    // One step per frame, measured on the layout the context made for the previous frame (the
    // first open settles within the slide-in). Fits when the strip does not scroll and the status block ends inside the bar (the tab strip
    // does not always shrink, so the content can run out past the bar's edge).
    Rml::Element* strip = mTabBar->root();
    const bool stripFits = strip->GetScrollWidth() <= strip->GetClientWidth() + 0.5f;
    const float barRight = mRoot->GetAbsoluteLeft() + mRoot->GetOffsetWidth();
    const float statusRight = mStatus->GetAbsoluteLeft() + mStatus->GetOffsetWidth();
    const bool barFits = statusRight <= barRight + 0.5f && barRight <= static_cast<float>(dims.x) + 0.5f;
    if ((stripFits && barFits) || mFitLevel >= kMaxFitLevel) {
        mFitSettled = true;
        mFitBase = mFitLevel;
        return;
    }
    ++mFitLevel;
    apply_fit_level();
}

void MenuBar::apply_fit_level() {
    for (int i = 1; i <= kMaxFitLevel; ++i) {
        mDocument->SetClass(fmt::format("fit-{}", i), i <= mFitLevel);
    }
}

void MenuBar::update_paused_tag() {
    // DESIGN 8.2: the tag says the game is actually held (Pause While Menu Is Open), not merely
    // that a menu is up.
    // Not over the game's own pause menu: it already says so, and the tag would sit on its title.
    // Nor under a window opened from the bar (the window starts where the tag would be).
    // Online the game never pauses (SETTINGS 13): the tag says so instead.
    const bool online = mppm::online::in_game();
    const bool show = visible() && !mCovered && p2_game_pause_menu_open() == 0 && (online || launcher::hold_game());
    if (online != mPausedOnline) {
        mPausedOnline = online;
        set_text_content(mPaused, online ? "The game is still running" : "Paused");
        mPaused->SetClass("online", online);
    }
    if (show != mPaused->HasAttribute("open")) {
        if (show) {
            mPaused->SetAttribute("open", "");
        } else {
            mPaused->RemoveAttribute("open");
        }
    }
}

bool MenuBar::overlaps_columns(float left, float right) const {
    if (!visible() || mRoot == nullptr) {
        return false;
    }
    const float barLeft = mRoot->GetAbsoluteLeft();
    const float barRight = barLeft + mRoot->GetOffsetWidth();
    return left < barRight && right > barLeft;
}

void MenuBar::refresh_prompt() {
    // A pad shows its face button, a keyboard the key (DESIGN 5.17; plain letters, no platform art).
    const bool pad = input::last_input_gamepad();
    if (pad != mPromptPad || mPromptGlyph->GetNumChildren() == 0) {
        mPromptPad = pad;
        mPromptGlyph->SetClass("key", !pad);
        set_text_content(mPromptGlyph, pad ? "B" : "Esc");
    }
}

bool MenuBar::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    // Not available while the start menu is up (it sits above the bar and normally takes the key
    // itself; this covers the frames where it is closing).
    if (is_prelaunch_open()) {
        return cmd == NavCommand::Menu;
    }
    if (cmd == NavCommand::Cancel && visible()) {
        play_sound(kSoundMenuClose);
        close();
        return true;
    }
    if (cmd == NavCommand::Menu) {
        if (game_obscured_below(*this)) {
            return true;
        }
        play_sound(visible() ? kSoundMenuClose : kSoundMenuOpen);
        if (visible()) {
            close();
        } else {
            // A fresh open always starts on Resume; only a window or confirmation closing over the
            // bar brings focus back to the entry that opened it.
            mFocusedId.clear();
            show();
        }
        input::sync_input_block();
        return true;
    }
    if (!visible()) {
        return false;
    }
    // Down/Up have nowhere to go; keep them from scrolling anything.
    if (cmd == NavCommand::Up || cmd == NavCommand::Down) {
        return true;
    }
    return Document::handle_nav_command(event, cmd);
}

// ------------------------------------------------------------------------------- confirmation

void confirm_progress_loss(const std::string& title, const std::string& confirmLabel,
    std::function<void()> action, Document* over) {
    // Nothing to lose on the title screen or file select (SETTINGS 13): act at once.
    if (p2_game_progress_at_risk() == 0) {
        action();
        return;
    }
    const auto dismiss = [](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
    };
    auto modal = std::make_unique<DangerModal>(Modal::Props{
        .title = title,
        .bodyText = "Progress since your last save will be lost.",
        .actions =
            {
                {.label = "Cancel", .onPressed = dismiss},
                {.label = confirmLabel,
                    .onPressed =
                        [action = std::move(action)](Modal& modal) {
                            play_sound(kSoundClick);
                            modal.pop();
                            action();
                        }},
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "warning",
    });
    Document& ref = *modal;
    if (over != nullptr) {
        over->push(std::move(modal));
    } else {
        push_document(std::move(modal));
    }
    ref.focus();
}

// ----------------------------------------------------------------------------------- install

void install_in_game_ui() {
    if (MenuBar::current() != nullptr) {
        return;
    }
    register_default_entries();
    // INPUT > Configure Controllers opens the same window as the bar's Controllers entry.
    set_settings_action(SettingsAction::ConfigureControllers, [] { open_controller_config(0); });
    // Order: the scrim lowest (z-index in ingame.rcss), the overlay above everything, the bar at
    // the bottom of the focus stack so start menus and windows open above it.
    push_document(std::make_unique<Scrim>(), true, true);
    push_document(std::make_unique<Overlay>(), true, true);
    push_document(std::make_unique<MenuBar>(), false);
}

} // namespace mppm::ui
