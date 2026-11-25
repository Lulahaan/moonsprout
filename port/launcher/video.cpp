#include "video.hpp"

#include "settings.hpp"

#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <p2_wide.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace mppm::video {
namespace {

bool sInitialized       = false;
bool sStartMenuOverride = false;
// Last values handed to Aurora, so apply() only touches what changed (vsync and fullscreen are
// visible operations; the startup values came from AuroraConfig).
int sAppliedVsync       = -1;
int sAppliedAnisotropy  = -1;
int sAppliedMsaa        = -1;
std::vector<borealis::config::Connection> sConnections;
SDL_Window* sWindow     = nullptr;
float sRefreshHz        = 0.0f;
unsigned sDisplayGen    = 0;

// fps_design 4: the current mode of the window's display. A missing mode or a rate of 0 or less
// is unknown (Xvfb reports none on some setups).
float read_refresh_hz() {
    // Test hook (raw environment, like MPPM_UPDATE_MOCK): MPPM_TEST_REFRESH=<Hz> stands in for the
    // display, so headless runs (Xvfb reports no refresh rate) can exercise the frame policy.
    if (const char* test = std::getenv("MPPM_TEST_REFRESH"); test != nullptr && test[0] != '\0') {
        const float hz = std::strtof(test, nullptr);
        return hz > 0.0f ? hz : 0.0f;
    }
    if (sWindow == nullptr) {
        return 0.0f;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(sWindow);
    const SDL_DisplayMode* mode = display != 0 ? SDL_GetCurrentDisplayMode(display) : nullptr;
    if (mode == nullptr) {
        return 0.0f;
    }
    // The exact ratio when SDL has it (59.94 rather than 60).
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
        return static_cast<float>(mode->refresh_rate_numerator) / static_cast<float>(mode->refresh_rate_denominator);
    }
    return mode->refresh_rate > 0.0f ? mode->refresh_rate : 0.0f;
}

float hud_max_aspect(HudMaxAspect v) {
    switch (v) {
    case HudMaxAspect::Ratio16x9:
        return 16.0f / 9.0f;
    case HudMaxAspect::Ratio21x9:
        return 64.0f / 27.0f;
    case HudMaxAspect::Unlimited:
        break;
    }
    return 0.0f;
}

int hud_layout(HudLayout v) {
    switch (v) {
    case HudLayout::Centered:
        return 0;
    case HudLayout::Edges:
        return 1;
    case HudLayout::Stretched:
        return 2;
    }
    return 0;
}

// The game's frame: 608x448 logical pixels, 19:14 (ASPECT.md 1.6), and the widest content shape
// the wide model handles (k <= 3.6, port/sdk/wide.cpp).
constexpr int kGameWidth  = 608;
constexpr int kGameHeight = 448;
constexpr float kGameAspect = static_cast<float>(kGameWidth) / kGameHeight;
constexpr float kMaxAspect  = kGameAspect * 3.6f;
// Auto fills windows down to 4:3 without bars: the console put its 19:14 frame on a 4:3 screen the
// same way (a 1.8% squeeze nobody sees), and a 19:14 lock left thin black bars above and below the
// title in a 4:3 window right after the start menu's full-window scene (the hand-off).
constexpr float kNarrowestUnbarred = 4.0f / 3.0f - 0.001f;

// Shape of the window in pixels (not of the content framebuffer, which a lock changes).
float sWindowAspect = 0.0f;

// ASPECT.md 4 and 5. Aurora always runs the STRETCH policy: the logical 608x448 frame maps onto a
// content framebuffer of whatever shape the lock gives, and the game compensates with k
// (port/sdk/wide.cpp). Original is a 19:14 lock (k = 1, bars on a wide window); Auto follows the
// window; the fixed ratios lock that shape; Stretch follows the window with k forced to 1.
void apply_aspect() {
    const auto& s = settings();
    const AspectRatio mode = s.video.aspectRatio.get();
    // The start menu shows its scene across the whole window whatever the setting; the scene is
    // correct at any k. In game the UI covers the whole window over the picture as it is (Aurora
    // patch 0007), so the F1 bar and its windows change nothing here.
    const bool menu = sStartMenuOverride;
    // A scene not verified wide (Piklopedia, Treasure Hoard) asks for Original while it runs.
    const bool sceneOriginal = !menu && mode != AspectRatio::Stretch && p2_wide_scene_original_active() != 0;

    AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    if (menu) {
        VIUnlockAspectRatio();
    } else if (sceneOriginal) {
        VILockAspectRatio(kGameWidth, kGameHeight);
    } else {
        switch (mode) {
        case AspectRatio::Auto:
            // Narrower than 4:3 gets bars above and below rather than a squeezed picture; past
            // the widest k the picture gets bars at the sides.
            if (sWindowAspect > 0.0f && sWindowAspect < kNarrowestUnbarred) {
                VILockAspectRatio(kGameWidth, kGameHeight);
            } else if (sWindowAspect > kMaxAspect) {
                VILockAspectRatio(static_cast<int>(kGameWidth * 3.6f), kGameHeight);
            } else {
                VIUnlockAspectRatio();
            }
            break;
        case AspectRatio::Original:
            VILockAspectRatio(kGameWidth, kGameHeight);
            break;
        case AspectRatio::Ratio16x10:
            VILockAspectRatio(16, 10);
            break;
        case AspectRatio::Ratio16x9:
            VILockAspectRatio(16, 9);
            break;
        case AspectRatio::Ratio21x9:
            VILockAspectRatio(64, 27); // the "21:9" of monitors is 64:27 (2.370)
            break;
        case AspectRatio::Stretch:
            VIUnlockAspectRatio();
            break;
        }
    }
    p2_wide_configure(!menu && mode == AspectRatio::Stretch, hud_layout(s.interface.hudLayout.get()),
        hud_max_aspect(s.interface.hudMaxAspect.get()));
}

// Remembers the window's pixel shape; Auto depends on it.
void note_window_size(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (aspect != sWindowAspect) {
        sWindowAspect = aspect;
        if (sInitialized) {
            apply_aspect();
        }
    }
}

} // namespace

void init(SDL_Window* window) {
    if (sInitialized) {
        return;
    }
    sInitialized  = true;
    sWindow       = window;
    sRefreshHz    = read_refresh_hz();
    sDisplayGen++;
    auto& s       = settings();
    sAppliedVsync = s.video.vsync.get() ? 1 : 0; // AuroraConfig.vsync
    sAppliedAnisotropy = s.video.anisotropy.get(); // AuroraConfig.maxTextureAnisotropy
    sAppliedMsaa  = s.video.msaa.get();            // AuroraConfig.msaa
    // Nothing has locked or scaled the content framebuffer yet, so it still has the window's shape.
    uint32_t width = 0, height = 0;
    AuroraGetRenderSize(&width, &height);
    note_window_size(width, height);
    // Scenes switch to Original and back through wide.cpp; re-lock at the next frame start.
    p2_wide_set_change_hook([] { apply_aspect(); });
    // A draw whose shader is not built yet waits for it (Aurora patch 0008) instead of being left
    // out of the frame: on a cold pipeline cache the title otherwise showed bare branches and a flat
    // grey square for a moment. Cached and prewarmed shaders never wait. MPPM_ASYNC_PIPELINES=1
    // (raw environment, a diagnostic) restores Aurora's skip-until-ready behaviour.
    const char* async = std::getenv("MPPM_ASYNC_PIPELINES");
    aurora_set_blocking_pipelines(async == nullptr || async[0] == '\0' || async[0] == '0');
    const auto reapply = [] { apply(); };
    sConnections.push_back(s.video.fullscreen.on_change(reapply));
    sConnections.push_back(s.video.vsync.on_change(reapply));
    sConnections.push_back(s.video.anisotropy.on_change(reapply));
    sConnections.push_back(s.video.aspectRatio.on_change(reapply));
    sConnections.push_back(s.video.internalResolution.on_change(reapply));
    sConnections.push_back(s.video.resampler.on_change(reapply));
    sConnections.push_back(s.interface.hudLayout.on_change(reapply));
    sConnections.push_back(s.interface.hudMaxAspect.on_change(reapply));
    apply();
}

void apply() {
    if (!sInitialized) {
        return;
    }
    const auto& s = settings();
    const bool fullscreen = s.video.fullscreen.get();
    if (VIGetWindowFullscreen() != fullscreen) {
        VISetWindowFullscreen(fullscreen);
    }
    const int vsync = s.video.vsync.get() ? 1 : 0;
    if (vsync != sAppliedVsync) {
        aurora_enable_vsync(vsync != 0);
        sAppliedVsync = vsync;
    }
    // Texture Filtering (SETTINGS 3): samplers built from the next frame on use the new value.
    const int anisotropy = s.video.anisotropy.get();
    if (anisotropy != sAppliedAnisotropy) {
        aurora_set_texture_anisotropy(static_cast<uint16_t>(anisotropy));
        sAppliedAnisotropy = anisotropy;
    }
    aurora_set_resampler(s.video.resampler.get() == Resampler::Area ? SAMPLER_AREA : SAMPLER_BILINEAR);
    // 0 = follow the window; n = n times the game's 448-line frame.
    VISetFrameBufferScale(static_cast<float>(s.video.internalResolution.get()));
    apply_aspect();
}

float display_refresh_hz() {
    return sRefreshHz;
}

void refresh_display() {
    const float hz = read_refresh_hz();
    if (hz != sRefreshHz) {
        sRefreshHz = hz;
        sDisplayGen++;
    }
}

unsigned display_generation() {
    return sDisplayGen;
}

void set_msaa(int samples) {
    samples = samples >= 4 ? 4 : 1;
    if (!sInitialized || samples == sAppliedMsaa) {
        return;
    }
    aurora_set_msaa(static_cast<uint32_t>(samples));
    sAppliedMsaa = samples;
}

void set_start_menu_override(bool on) {
    if (sStartMenuOverride != on) {
        sStartMenuOverride = on;
        apply();
    }
}

void toggle_fullscreen() {
    auto& var = settings().video.fullscreen;
    if (!var.locked()) {
        var.set(!var.get());
    }
}

void reset_window_size() {
    auto& s = settings();
    s.video.fullscreen.set(false);
    VISetWindowFullscreen(false);
    VISetWindowSize(1280, 960);
    VICenterWindow();
    s.video.windowWidth.reset();
    s.video.windowHeight.reset();
}

void on_window_resized(const AuroraWindowSize& size) {
    note_window_size(size.native_fb_width, size.native_fb_height);
    auto& s = settings();
    if (VIGetWindowFullscreen() || size.width == 0 || size.height == 0) {
        return;
    }
    // A size forced by MPPM_WINDOW or --cvar is not the user's; don't save over their own.
    if (s.video.windowWidth.overlay() != nullptr || s.video.windowHeight.overlay() != nullptr) {
        return;
    }
    s.video.windowWidth.set(static_cast<int>(size.width));
    s.video.windowHeight.set(static_cast<int>(size.height));
}

} // namespace mppm::video
