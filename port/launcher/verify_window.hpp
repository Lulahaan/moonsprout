#pragma once
// The "Verify Disc" dialog (docs/launcher/SETTINGS.md 2): reads the whole disc image on a worker
// thread with a progress bar and Cancel, then shows the result in place. A match is remembered
// (backend.discVerification = verified); on a mismatch "Continue anyway" remembers
// mismatch-accepted. Both are tied to the file's fingerprint (disc::remember_verification).
//
// Opened from the PRELAUNCH tab of the settings window (package W4), and by the start menu when
// disc::needs_reverification() says a verified file has changed.

#include "disc.hpp"

#include <functional>
#include <string>

namespace mppm::ui {

// Pushes the dialog over the top document and starts verifying `path` (empty: the configured
// backend.discPath). `onFinished` runs on the UI thread once the run ends (also when canceled).
// Returns false, and opens nothing, when there is no disc or a verification is already running.
bool open_verify_disc(std::string path = {}, std::function<void(disc::Outcome)> onFinished = {});

// True while a Verify Disc dialog is open.
bool disc_verification_running();

} // namespace mppm::ui
