// Frame pacing policy (mppm/frame_policy.hpp): turns video.frameRate, video.smoothMotion,
// video.vsync and the display's refresh rate into the numbers the host loops use. The derivation
// is fps_design section 4 (docs/launcher/SETTINGS.md 3, "Frame Rate").
#include "mppm/frame_policy.hpp"

#include "settings.hpp"
#include "ui/ui.hpp"
#include "video.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace mppm::launcher {
namespace {

constexpr double kNsPerSecond = 1e9;
// With VSync on and a known refresh, host-made frames sleep to 1.05 R: a blocking FIFO always
// wins against it, and a swapchain that does not block (Xvfb) is still bounded.
constexpr float kVsyncSlack = 1.05f;
// UI-only loops stay bounded even at Unlimited, so an open menu does not run the GPU flat out.
constexpr float kUnlimitedUiHz = 240.0f;
constexpr float kUnknownRefreshUiHz = 60.0f;

int64_t period_ns(float hz) {
    return hz > 0.0f ? static_cast<int64_t>(kNsPerSecond / hz + 0.5) : 0;
}

struct Inputs {
    std::string rate;
    bool smooth = false;
    bool vsync = true;
    float refreshHz = 0.0f;
    bool operator==(const Inputs&) const = default;
};

FramePolicy derive(const Inputs& in) {
    FramePolicy p;
    const float r = in.refreshHz;
    const bool vr = in.vsync && r > 0.0f; // VSync on and the refresh known
    p.refreshHz = r;
    p.original = in.rate == "original";
    p.smoothMotion = in.smooth && !p.original;

    // sleepNs of the `display` row; the UI slot for `original` and `display`.
    int64_t displaySleep = 0;
    float displayCap = 0.0f;
    if (vr) {
        displayCap = r;
        displaySleep = period_ns(kVsyncSlack * r);
    } else if (!in.vsync) {
        displayCap = r > 0.0f ? r : 60.0f;
        displaySleep = period_ns(displayCap);
    }

    if (p.original) {
        // Skipping only: the game keeps its own retrace pacing; game frames never sleep.
        p.capHz = vr ? r : 0.0f;
        p.sleepNs = 0;
        p.uiSleepNs = displaySleep;
    } else if (in.rate == "display") {
        p.capHz = displayCap;
        p.sleepNs = displaySleep;
        p.uiSleepNs = displaySleep;
    } else if (in.rate == "unlimited") {
        p.capHz = vr ? r : 0.0f;
        p.sleepNs = vr ? period_ns(kVsyncSlack * r) : 0;
        p.uiSleepNs = period_ns(std::max(r, kUnlimitedUiHz));
    } else {
        const int n = std::max(frame_rate_number(in.rate), 30); // sanitized: 30-1000
        const float nf = static_cast<float>(n);
        if (vr) {
            p.capHz = std::min(nf, r);
            p.sleepNs = nf < r ? period_ns(nf) : period_ns(kVsyncSlack * r);
        } else {
            p.capHz = nf;
            p.sleepNs = period_ns(nf);
        }
        p.uiSleepNs = period_ns(nf);
    }
    if (p.uiSleepNs == 0) {
        // VSync on with the refresh unknown: a swapchain that does not block (Xvfb, some
        // compositors) would let the hold loop and the start menu spin, so they keep 60.
        p.uiSleepNs = period_ns(kUnknownRefreshUiHz);
    }
    p.presentNs = period_ns(p.capHz);
    return p;
}

void log_policy(const Inputs& in, const FramePolicy& p) {
    char display[32];
    if (p.refreshHz > 0.0f) {
        std::snprintf(display, sizeof(display), "%.2f Hz", p.refreshHz);
    } else {
        std::snprintf(display, sizeof(display), "unknown");
    }
    char cap[32];
    if (p.capHz > 0.0f) {
        std::snprintf(cap, sizeof(cap), "%.1f Hz", p.capHz);
    } else {
        std::snprintf(cap, sizeof(cap), "none");
    }
    std::fprintf(stderr,
        "mppm: display %s, vsync %s, frame rate %s -> cap %s, sleep %.2f ms, ui sleep %.2f ms%s\n", display,
        in.vsync ? "on" : "off", in.rate.c_str(), cap, static_cast<double>(p.sleepNs) / 1e6,
        static_cast<double>(p.uiSleepNs) / 1e6, p.smoothMotion ? ", smooth motion" : "");
}

} // namespace

const FramePolicy& frame_policy() {
    static FramePolicy sPolicy{};
    static Inputs sInputs{};
    static bool sValid = false;
    static unsigned sDisplayGen = ~0u;
    // Cheap to re-check every frame: recompute only when an input changed (a live settings edit,
    // VSync, or the window moving to a display with another refresh rate).
    const auto& v = settings().video;
    const unsigned gen = video::display_generation();
    if (sValid && gen == sDisplayGen && v.frameRate.get() == sInputs.rate && v.smoothMotion.get() == sInputs.smooth &&
        v.vsync.get() == sInputs.vsync)
    {
        return sPolicy;
    }
    Inputs in{v.frameRate.get(), v.smoothMotion.get(), v.vsync.get(), video::display_refresh_hz()};
    sDisplayGen = gen;
    if (sValid && in == sInputs) {
        return sPolicy;
    }
    sInputs = std::move(in);
    sPolicy = derive(sInputs);
    sValid = true;
    if (settings().tools.logFps.get()) {
        log_policy(sInputs, sPolicy);
    }
    return sPolicy;
}

bool ui_visible() {
    return ui::any_document_visible();
}

} // namespace mppm::launcher
