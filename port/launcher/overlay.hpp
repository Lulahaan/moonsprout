#pragma once
// The passive overlay above the game (docs/launcher/DESIGN.md 5.14, 8.2, 8.3; SETTINGS.md 7, 9.1,
// 13): never focused, never blocks input.
//
//   - toasts from ui::push_toast, up to three stacked under the top-right corner;
//   - the FPS pill (video.fpsCounter picks its corner) with the "Cheats on" badge beside it while
//     any cheat.* setting differs from its default;
//   - the shader-compile notice while Aurora has pipelines queued (interface.shaderIndicator);
//   - the "Press F1 for the menu" hint, once per session, after the start menu has closed and the
//     game is on screen (interface.menuHint), and a warning when Player 1 has no input device;
//   - the scrim behind the in-game bar and its windows: 30% navy under the bar, 50% under a window.

#include "ui/document.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace mppm::ui {

class Overlay : public Document {
public:
    Overlay();
    ~Overlay() override;

    void show() override;
    void update() override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    struct LiveToast {
        Rml::Element* element = nullptr;
        clock::time_point start;
        clock::duration duration{};
        bool closing = false;
        float top = -1.0f;
    };

    void update_corner();
    bool corner_covered() const;
    void update_toasts();
    void update_pipeline_progress();
    void update_menu_hint();
    void update_controller_warning();

    Rml::Element* mCorner = nullptr;
    Rml::Element* mFps = nullptr;
    Rml::Element* mCheats = nullptr;
    Rml::Element* mPipeline = nullptr;
    Rml::Element* mPipelineLabel = nullptr;
    Rml::Element* mPipelineBar = nullptr;
    Rml::Element* mMenuHint = nullptr;
    Rml::Element* mControllerWarning = nullptr;
    std::vector<LiveToast> mToasts;
    clock::time_point mMenuHintStart;
    clock::time_point mPipelineStart;
    clock::time_point mPrelaunchSeen; // last frame the start menu was up (no toast in its closing flash)
    clock::time_point mPrelaunchClosedAt;
    std::string mFpsText;
    uint32_t mPipelineBase = 0;
    uint32_t mLastQueued = 0;
    bool mPipelineActive = false;
    bool mBarOpen = false;
    bool mCornerUnderBar = false;
    bool mCornerCovered = false; // a window's box reaches the corner pills
    bool mHintArmed = false;
    bool mHintDone = false;
    int mFrames = 0;
};

// "NN FPS", sampled at most twice a second (a number that changes every frame is unreadable).
// The overlay pill and the in-game bar both show this one sample, so they always agree.
const std::string& fps_text();

// Full-frame navy scrim behind the in-game bar and the windows opened from it (DESIGN 8.2). It
// has no blur: the game stays readable and the darkening is the "menu is up" signal.
class Scrim : public Document {
public:
    Scrim();
    void show() override;
    void update() override;

protected:
    bool handle_nav_command(Rml::Event&, NavCommand) override { return false; }

private:
    int mLevel = -1;
};

} // namespace mppm::ui
