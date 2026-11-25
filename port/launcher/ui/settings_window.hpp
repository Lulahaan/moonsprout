#pragma once
// The settings window (docs/launcher/SETTINGS.md 1.5, 2 to 7, 9, 11; DESIGN.md 5.10).
//
// Nine tabs: PRELAUNCH (only when opened from the start menu), VIDEO, INPUT, AUDIO, GAMEPLAY,
// CHEATS, INTERFACE, ONLINE (M5.3b), TOOLS. Every row is bound to its var in port/launcher/settings.hpp: the left
// pane holds the controls, the right pane explains the focused row (help text, lock and restart
// notes, the default value and a Reset button). Rows carry badges for "set by the environment",
// "restart required", "next launch" and "experimental". Runtime settings apply as they change
// (their hooks observe the vars); restart-only ones raise a banner with "Restart now".

#include "window.hpp"

#include <borealis/config.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mppm::ui {

class SettingsWindow : public Window {
public:
    // prelaunch: opened from the start menu (adds the PRELAUNCH tab; Restart now needs no
    // confirmation because nobody is playing).
    explicit SettingsWindow(bool prelaunch);
    ~SettingsWindow() override;

    void update() override;
    void hide(bool close) override;
    void uncover() override;

    // Where the window is (live_options_design 3.8): the visible tab's title, the focused row's
    // settings key ("" for a button row or none) and the controls pane's scroll position.
    struct Location {
        std::string tab;
        std::string key;
        float scroll = 0.0f;
    };
    Location location() const;
    // Puts the window back at a Location after a relaunch: that tab, that row focused, that
    // scroll, with no entrance animation and no open sound. Call right after opening.
    void restore(const std::string& tab, const std::string& key, float scroll);
    // Switches to the tab with this title ("Online") and focuses its first row. False if none.
    bool show_tab(const std::string& title);

    // Per-row state of the visible tab (badges, help). Defined in settings_window.cpp.
    struct Row;
    struct TabState;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;
    bool consume_close_request() override;

private:
    friend class RowBuilder;

    void build_prelaunch(Rml::Element* content);
    void build_video(Rml::Element* content);
    void build_input(Rml::Element* content);
    void build_audio(Rml::Element* content);
    void build_gameplay(Rml::Element* content);
    void build_cheats(Rml::Element* content);
    void build_interface(Rml::Element* content);
    void build_online(Rml::Element* content);
    void build_tools(Rml::Element* content);

    void update_rows();
    void update_banner();
    void restart_now();
    void request_rebuild();
    // Hover follows the pointer only once it has moved (open, tab change, restore arm the hold).
    void set_hover_hold(bool hold);
    // Leaving the restart banner (Later, or it hides): focus goes back to the list.
    void focus_from_banner();
    // R / Y: resets the row holding `target` when it differs from its default. True if it did.
    bool reset_row_of(Rml::Element* target);

    bool mPrelaunch;
    std::vector<std::string> mTabTitles; // in tab order
    std::string mActiveTab;              // the tab whose rows are built
    int mResumeFrames = 0;               // restore(): frames left with the `resume` class
    bool mHoverHold = false;             // hover leaves rows and the help pane alone until the pointer moves
    bool mHoverAnchorSet = false;
    Rml::Vector2f mHoverAnchor{0.0f, 0.0f};
    std::unique_ptr<TabState> mTab;
    // Restart banner (DESIGN 5.10), pinned under the content.
    Rml::Element* mBanner = nullptr;
    Rml::Element* mBannerText = nullptr;
    std::vector<std::unique_ptr<Button>> mBannerButtons;
    bool mBannerDismissed = false;   // "Later": hidden until the pending set changes
    size_t mBannerPendingCount = 0;
    bool mCloseAsked = false;        // the restart question on close is asked once per window
    bool mRebuildPending = false;
    // The control that had focus last, given back when a modal on top closes (DESIGN 9.4).
    Rml::ObserverPtr<Rml::Element> mLastFocus;
    float mRestoreScrollTop = 0.0f; // the controls pane's scroll position across a rebuild
    int mRestoreScrollFrames = 0;
    // A row focused by a rebuild that did not keep the old one: scrolled into view once laid out.
    Rml::ObserverPtr<Rml::Element> mScrollTo;
    int mScrollToFrames = 0;
    bool mScrollToNearest = false; // scroll mScrollTo just into view (the banner appeared), not to the centre
    // The row that "Reset to Default" in its help pane belongs to: focused again after the press
    // (the button hides itself, and the help pane is rebuilt, so this waits for update()).
    const Row* mPendingRowFocus = nullptr;
    // The control Down left for the restart banner, given back by Up.
    Rml::ObserverPtr<Rml::Element> mBannerReturn;
    std::vector<borealis::config::Connection> mConnections;
};

// Whether this graphics backend id resolves to the backend running now (no restart needed).
bool settings_backend_is_running(std::string_view id);

// Opens the settings window on top of everything. `opener` (the start menu) is covered while it is
// open and uncovered when it closes.
SettingsWindow& open_settings_window(bool prelaunch, Document* opener = nullptr);

// Buttons whose feature belongs to another package (README 4). A row stays disabled with "Not
// available yet" until its owner registers a handler, so the settings window never has to change
// when the feature lands.
enum class SettingsAction {
    VerifyDisc,           // W7: PRELAUNCH > Verify Disc
    SaveManager,          // W7: PRELAUNCH and TOOLS > Save Manager
    ConfigureControllers, // W6: INPUT > Configure Controllers
    ResetAllControls,     // W6: TOOLS > Reset All Controls (asks for confirmation itself)
    CheckForUpdates,      // W3: INTERFACE > Check Now (reports its own result)
    Count,
};
// `status` (optional) is the row's value text, e.g. "Verified" for Verify Disc; asked every frame.
void set_settings_action(SettingsAction action, std::function<void()> handler,
    std::function<std::string()> status = {});

}  // namespace mppm::ui
