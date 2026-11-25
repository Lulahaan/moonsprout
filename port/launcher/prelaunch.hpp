#pragma once
// The start menu (docs/launcher/DESIGN.md 5.3, 6, 7): wordmark, menu, disc status, version and
// update notice over the pre-disc backdrop or the live scene, the entrance, the backdrop-to-scene
// hand-over and the UI half of the take-off.
//
// Two modes:
//   - standalone (gameRunning = false): no game yet. Play picks a disc, or, with a disc, fades the
//     menu to navy and reports launch_requested(); the boot flow then boots the game.
//   - over the game (gameRunning = true): the game boots behind the opaque backdrop. The backdrop
//     fades once the live scene is ready (body class `scene-ready`). Play runs the take-off
//     timeline (section 7), driven by scene cues with fallback timers, and the document closes
//     itself after the hand-over flash. Without a scene (the W9 stub) the timeline runs entirely
//     on its nominal times and reveals whatever the game shows underneath: the title.
//
// The entrance plays once per process. A start menu created again in the same run (after a disc
// was picked on the pre-disc screen, DESIGN 6.9) starts with `intro-skip` and an opaque backdrop,
// and plays the ship's landing instead.

#include "disc.hpp"

#include "ui/button.hpp"
#include "ui/document.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mppm::ui {

// Orders for start-menu items in launcher::Menu::StartMenu (DESIGN 5.3). Play is always first and
// Quit always last; registered entries sort between them.
namespace start_order {
inline constexpr int kMultiplayer = 20;
inline constexpr int kMods        = 30;
inline constexpr int kSettings    = 50;
} // namespace start_order

class Prelaunch : public Document {
public:
    // gameRunning: shown over the booting game (Play runs the take-off) rather than before a disc
    // exists (Play picks a disc / starts the boot).
    explicit Prelaunch(bool gameRunning);
    ~Prelaunch() override;

    void update() override;
    bool focus() override;
    bool obscures_game() const override { return true; }
    // A window pushed over the menu (Settings) leaves the backdrop or scene visible behind its
    // glass: the menu only steps back (body class `covered`) instead of hiding.
    void cover() override;
    void uncover() override;

    // A file dropped on the window (SDL_EVENT_DROP_*), forwarded by the launcher while the menu is
    // up. drop_hover shows or clears the drop target; drop_file checks the image and, if it is a
    // disc this build can run, makes it the disc, like the file dialog.
    void drop_hover(bool on);
    void drop_file(const std::string& path);

    // Standalone mode: the player started the game. Set once the exit fade has finished.
    bool launch_requested() const { return mLaunchRequested; }
    bool quit_requested() const { return mQuitRequested; }

    // The game was booted behind this standalone menu (live_options 3.4, 3.6): it carries on as the
    // menu over the game instead of being replaced.
    void game_started();
    // The menu is at rest: no take-off running and no file dialog open (live_options 3.5).
    bool idle() const { return mPhase == Phase::Idle && !mPicking; }
    bool taking_off() const { return mPhase != Phase::Idle; } // Play pressed: the take-off runs

    // An online match is starting (p2_launcher_online_started, M5.3b): leave the way Play does, with
    // no disc picking and no restart for pending changes (a relaunch would drop the session).
    // Windows over the menu (the Multiplayer window, Settings, a modal) close first. Standalone
    // mode fades to navy and reports launch_requested(); over the game the take-off runs. False
    // when the menu cannot leave now (already taking off, or standalone without a usable disc).
    bool start_online();

    // The start menu currently open, if any: registered menu entries use it to push their window
    // over the menu (e.g. SettingsWindow).
    static Prelaunch* current();

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void refresh_version_label(); // "<product> v<version>", plus " · N mods" once mods are mounted
    // Take-off phases (DESIGN 7.4).
    enum class Phase {
        Idle,      // menu up
        Launching, // UI leaves, scene takes off; cues and fallbacks drive the classes
        Flash,     // the end flash ramps to opaque
        Wait,      // flash opaque, still no hand-over: navy and the bead loader
        Handoff,   // hidden under the flash: launcher elements removed, aspect override released
        Reveal,    // the flash fades over the title
        Done,
    };
    // Full: the take-off (Reduce Motion keeps its cameras still and uses the plain-fade UI rules,
    // body class launch-short). Off: a short fade to navy.
    enum class Mode { Full, Off };
    // Scene cues as the timeline sees them (DESIGN 7.1). Handoff is P2_SCENE_DONE.
    enum Cue : int { kLaunchBegin, kHop, kBoard, kIgnite, kLiftoff, kExitFrame, kHandoff, kCueCount };
    using Ms = double;

    struct MenuItem {
        std::unique_ptr<Button> button;
        Rml::Element* label = nullptr;
        Rml::Element* icon  = nullptr;
    };

    void build_menu();
    MenuItem& add_item(const Rml::String& text, ButtonCallback onPressed);
    void build_backdrop();
    void refresh_disc();
    void refresh_picking();
    void start_landing(double now);
    void refresh_update_status();
    void refresh_prompts();
    void track_input_device();
    void set_pointer_input(bool on);
    void on_play();
    void pick_disc();
    void finish_entrance();
    void focus_item(int index, bool sound);
    int focused_item() const;
    bool focus_extra(Rml::Element* element);
    void remember_focus();
    bool restore_focus();
    void resend_skip();
    // True when the menu must not act on input: a document is on top of it (it gets the focus), or
    // the first-run card is still to come (the press hurries the entrance so the card comes now).
    bool yield_input();
    // The pointer moved over a menu control: it takes the focus, so hover and keyboard share one
    // selection (Dusklight).
    void hover_focus(Button& button);

    // Take-off.
    bool live_scene() const;
    void begin_launch();
    void request_skip();
    void step_launch();
    void enter_phase(Phase phase);
    void on_scene_cue(int cue);
    Ms launch_ms() const;
    void set_body_class(const char* name, bool on);

    bool mGameRunning;
    bool mContinuation;   // a start menu was already shown this run: no entrance
    bool mLaunchRequested = false;
    bool mQuitRequested   = false;

    // Entrance.
    bool mEntranceStarted = false;
    bool mEntranceDone    = false;
    int mUpdates          = 0;
    double mOpenedAt      = 0.0; // seconds, steady clock

    // Disc.
    std::string mDiscPath;       // the disc this menu shows (the booted one over the game)
    disc::Info mDisc;
    std::string mShownDiscSetting;
    bool mPicking = false;          // the file dialog is open
    std::string mRejectedPath;      // a dropped file that is not a usable disc (shown, not saved)
    disc::Info mRejected;

    // Backdrop / scene hand-over (DESIGN 6.9).
    bool mSceneReady   = false;
    bool mLanding      = false; // ship flying to the horizon after a disc was picked
    bool mLanded       = false;
    bool mWantLanding  = false; // a live scene is coming: the ship lands before the cross-fade
    double mLandingAt  = 0.0;
    bool mCovered      = false;
    // A window on top (the first-run card) closed at this SDL tick; presses of a mash that carry on
    // past it must not open the file dialog (pick_disc).
    uint64_t mUncoveredAt   = 0;
    uint64_t mLastPickGuard = 0;
    bool mSceneSwap    = false; // the backdrop came back over a shown scene (body class scene-swap)
    bool mSwitchShown  = false; // the disc line says "Switching disc..." (live::pending_relaunch)

    // Update notice.
    unsigned mUpdateGeneration = ~0u;
    double mFailedAt = 0.0;

    // Menu.
    std::vector<MenuItem> mItems;
    MenuItem* mPlay = nullptr;
    std::unique_ptr<Button> mChangeDisc;
    std::unique_ptr<Button> mUpdatePill;
    Rml::Element* mPlayHalo = nullptr;
    bool mPointerInput = false;
    // Polled across documents (the first-run card and Settings get the events while they are on
    // top): pointer position and buttons, keyboard state and the pad flag, last frame.
    bool mPointerBaseline = false;
    float mMouseX = 0.0f;
    float mMouseY = 0.0f;
    uint32_t mMouseButtons = 0;
    std::vector<uint8_t> mKeysDown;
    bool mPadInput = false;
    bool mPickFailed = false;         // the file dialog could not open: the hint says so
    bool mPresetChecked = false;      // the first-run card was offered (after the entrance)
    int mReturnItem = 0;              // focus to give back when a window over the menu closes
    Button* mReturnExtra = nullptr;   // ... or Change / the update pill
    double mCheckingSince = 0.0;      // the update check has been running since (0 = not)

    // Take-off state.
    Phase mPhase     = Phase::Idle;
    Mode mMode       = Mode::Full;
    bool mNavyFlash  = false;
    bool mSkipped    = false; // the flash was hurried (skip, or the scene ended early)
    bool mSkipSent    = false; // Skip went to the scene; repeated until its fade starts
    bool mSceneFade   = false; // the flash only veils the scene's own end fade (step_launch)
    std::optional<Ms> mFlashClosedAt; // when that veil started closing to opaque
    bool mScenePlay  = false; // a live scene received the Play command
    bool mSceneLive  = false; // ... latched at Play: only then does the scene's DONE hand over (H2)
    uint64_t mBeatSeen   = 0;     // scene::heartbeat_count at the last check
    int mFramesNoBeat    = 0;     // launcher frames since the scene last polled
    double mLaunchAt = 0.0;
    Ms mPhaseAt      = 0.0;   // launch_ms() when mPhase was entered
    std::array<std::optional<Ms>, kCueCount> mCueAt{}; // when each cue took effect
    const double* mNominal = nullptr; // fallback cue times: kFullTimes, kBackdropTimes or kOffTimes
    int mCueListener = 0;
    Rml::Element* mBloom = nullptr;
};

} // namespace mppm::ui
