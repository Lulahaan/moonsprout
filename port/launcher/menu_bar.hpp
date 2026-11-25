#pragma once
// The in-game menu (docs/launcher/SETTINGS.md 13, DESIGN.md 8): a floating glass bar opened with
// F1, the controller's Back button or R + Start (all arrive as KI_F1, ui/input.cpp), built from
// the in-game bar registry (menu_registry.hpp). While it or anything opened from it is visible,
// ui::update keeps PADBlockInput on, so the game sees no input; held buttons stay suppressed
// until released when it closes (Aurora's PADBlockInput).
//
// The bar is a permanent document at the bottom of the document stack: hidden, it still takes the
// menu key (the passive overlay forwards keys to the top document), and the start menu, pushed
// above it, swallows the key while it is up. Windows opened from the bar cover it and uncover it
// when they close; the bar itself stays up above them (DESIGN 8.2).
//
// Entries registered here (in-game bar registry ids): "resume", "settings", "controllers",
// "start-menu", "quit", plus hidden reservations for "mods" and "online". Other files add their
// own with launcher::register_menu_entry(Menu::InGameBar, ...); the bar asks the registry every
// time it opens, so nothing here changes.

#include "ui/document.hpp"
#include "ui/tab_bar.hpp"

#include <memory>
#include <string>

namespace mppm::ui {

class MenuBar : public Document {
public:
    MenuBar();
    ~MenuBar() override;

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;
    bool permanent() const override { return true; }
    // A window opened from the bar does not hide it (DESIGN 8.2): the bar stays up, the window
    // opens below it, and focus comes back to the entry that opened it when the window closes.
    void cover() override;
    void uncover() override;

    // Pushes a window over the bar; the bar slides away and comes back when the window closes.
    void open_over(std::unique_ptr<Document> document);
    // Hides the bar (Resume).
    void close();
    // Rebuilds the tabs from the registry now (they are rebuilt on every open anyway).
    void refresh_entries();

    // Whether the open bar spans any of the columns [left, right) (context pixels): the overlay's
    // top-corner pill moves below the bar only when the bar actually reaches its corner.
    bool overlaps_columns(float left, float right) const;

    // The bar, once install_in_game_ui() has run.
    static MenuBar* current();

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void build_tabs();
    void update_status();
    void refresh_prompt();
    void fit_to_width();
    void apply_fit_level();
    void update_paused_tag();
    void update_window_inset();
    void mark_open_entry(bool on);

    Rml::Element* mRoot = nullptr;
    Rml::Element* mStatus = nullptr;
    Rml::Element* mFps = nullptr;
    Rml::Element* mClock = nullptr;
    Rml::Element* mPromptGlyph = nullptr;
    Rml::Element* mPaused = nullptr;
    std::unique_ptr<TabBar> mTabBar;
    std::string mFocusedId;            // registry id of the tab focused when the bar last closed
    std::vector<std::string> mTabIds;  // registry ids, in tab order
    std::string mClockText;
    std::string mFpsText;
    Rml::Vector2i mFitDims;
    float mFitRatio = 0.0f;
    int mFitLevel = -1; // compact level in use (fit_to_width), -1 = measure again
    int mFitBase = 0;   // level the last measurement settled on for mFitDims
    bool mFitSettled = false;
    bool mShowFps = true;
    bool mPromptPad = false;
    bool mCovered = false; // a window opened from the bar is on screen below it
    bool mPausedOnline = false; // the paused tag reads "The game is still running" (online match)
};

// Asks before an action that loses unsaved progress (Start Menu, Quit): a danger modal with
// `confirmLabel` and Cancel, or the action straight away when nothing can be lost (title screen,
// file select). `over` is covered while the modal is up.
void confirm_progress_loss(const std::string& title, const std::string& confirmLabel,
    std::function<void()> action, Document* over = nullptr);

// Creates the passive overlay, the scrim and the in-game bar. Called once by launcher::init,
// before any start menu is pushed (the start menu must sit above the bar).
void install_in_game_ui();

} // namespace mppm::ui
