#pragma once
// Frame pacing policy: the contract between the settings (video.frameRate, video.smoothMotion,
// video.vsync, the display's refresh rate) and the host loops in port/main.cpp. Derivation rules:
// fps_design section 4. Package 0 returns the default policy (Original, nothing capped).
#include <cstdint>

namespace mppm::launcher {

struct FramePolicy {
    bool original = true;      // video.frameRate == "original"
    bool smoothMotion = false; // video.smoothMotion && !original
    float refreshHz = 0.0f;    // current display; 0 = unknown
    float capHz = 0.0f;        // most presents per second; 0 = none
    int64_t presentNs = 0;     // 1e9 / capHz; 0 = none        (FramePacer::due)
    int64_t sleepNs = 0;       // slot for host-made game frames; 0 = no sleep (FramePacer::wait)
    int64_t uiSleepNs = 0;     // slot for UI-only loops
};

const FramePolicy& frame_policy(); // main thread; cached, recomputed on change
bool ui_visible();                 // a launcher document is visible

} // namespace mppm::launcher
