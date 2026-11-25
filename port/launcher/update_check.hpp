#pragma once
// The update check (docs/launcher/SETTINGS.md 9.1 "Check for Updates", DESIGN.md 6.7).
//
// Notify only: asks GitHub for the latest release of branding::kGitHubOwner/kGitHubRepo, compares it
// with branding::kVersion and reports the result. It never downloads or installs anything. The
// request runs on borealis' worker pool; everything here is called on the main thread.
//
// While the repository is private GitHub answers 404. That is not an error the user can act on, so
// it reports State::Quiet and the start menu shows nothing (README 7, question 8).
//
// Test hook (raw environment, like MPPM_FORCE_LAUNCHER): MPPM_UPDATE_MOCK replaces the network
// result with `checking` (never finishes), `uptodate`, `available[:vX.Y.Z]`, `failed` or `404`.

#include <string>

namespace mppm::launcher::update_check {

enum class State {
    Idle,      // not started (checks off, or the start menu has not asked yet)
    Checking,  // request in flight
    UpToDate,
    Available, // latest_version() / release_url() are set
    Failed,    // network or parse error: "Couldn't check for updates"
    Quiet,     // nothing to report: no HTTP backend, or the repository is not public (404)
};

// Starts a check unless one already ran this process, interface.checkForUpdates is off, or the
// first-run consent question has not been answered yet (interface.presetChosen is false).
// `force` ("Check Now" in Settings) ignores both and checks again.
void begin(bool force = false);

// Collects a finished request. Cheap; call once per frame while anything shows the status.
void poll();

State state();
// "0.4.0" (tag without the leading v) when an update is available, else empty.
const std::string& latest_version();
// The release page of the latest version (falls back to branding::kReleasesUrl).
std::string release_url();
// Opens release_url() in the browser. Returns false if the system refused.
bool open_release();

// One sentence for "Check Now" (SETTINGS 9.1): "You're up to date (v0.3.1).",
// "Moonsprout v0.4.0 is available.", "Couldn't reach GitHub. Try again later." or
// "Checking for updates..."; empty while Idle.
std::string summary();

// Increments whenever state() changes, so a view can refresh only when needed.
unsigned generation();

} // namespace mppm::launcher::update_check
