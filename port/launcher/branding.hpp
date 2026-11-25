#pragma once
// Product identity. Every user-facing string that names the app takes it from here; no string in
// code spells the name (docs/launcher/README.md 2 and 6).
//
// The product is "Moonsprout". The repository is "moonsprout"; the binary and the data folder stay "mppm", so
// existing settings and saves keep their place.

#include <mppm_version.h> // generated at configure time (CMakeLists.txt)

#include <string_view>

namespace mppm::branding {

// Window title, wordmark, release notes, every "Moonsprout" in UI copy.
inline constexpr const char* kProductName = "Moonsprout";
// Folder name under the platform's user-data directory (SDL_GetPrefPath). Never changes.
inline constexpr const char* kDataDirName = "mppm";
// Version shown in the start menu and compared by the update check: "1.2.0" for a tagged
// release, "0.1.0-dev+<commit>" otherwise.
inline constexpr const char* kVersion = MPPM_VERSION_STRING;
// Where releases are published; "View Release" and the update check point here.
inline constexpr const char* kReleasesUrl = "https://github.com/owallzz/moonsprout/releases";
// GitHub repository for borealis::update::check_latest_github_release.
inline constexpr std::string_view kGitHubOwner = "owallzz";
inline constexpr std::string_view kGitHubRepo  = "moonsprout";

} // namespace mppm::branding
