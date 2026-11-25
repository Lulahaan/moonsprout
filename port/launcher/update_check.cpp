#include "update_check.hpp"

#include "branding.hpp"
#include "settings.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>
#include <borealis/update.hpp>
#include <fmt/format.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>

namespace mppm::launcher::update_check {
namespace {

const borealis::AppInfo kAppInfo{
    .orgName     = "",
    .appName     = branding::kProductName,
    .githubOwner = branding::kGitHubOwner,
    .githubRepo  = branding::kGitHubRepo,
};

State sState = State::Idle;
bool sStarted = false; // once per process unless forced
unsigned sGeneration = 0;
std::string sLatestVersion;
std::string sReleaseUrl;
borealis::Task<borealis::update::Result> sTask;

void set_state(State state) {
    if (sState != state) {
        sState = state;
        sGeneration++;
    }
}

std::string strip_v(std::string_view tag) {
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
        tag.remove_prefix(1);
    }
    return std::string(tag);
}

// GitHub answers 404 for a private (or not yet created) repository and for a repository without
// releases. Neither is worth a message. borealis only reports the status in its message text.
bool is_not_found(const borealis::update::Result& result) {
    return result.message.find("HTTP 404") != std::string::npos;
}

void apply(const borealis::update::Result& result) {
    using borealis::update::Status;
    switch (result.status) {
    case Status::UpdateAvailable:
        sLatestVersion = strip_v(result.latest.tagName);
        sReleaseUrl    = result.latest.htmlUrl;
        set_state(State::Available);
        return;
    case Status::UpToDate:
        set_state(State::UpToDate);
        return;
    case Status::Disabled:
        set_state(State::Quiet);
        return;
    case Status::Failed:
        if (is_not_found(result)) {
            set_state(State::Quiet);
            return;
        }
        std::fprintf(stderr, "mppm: update check: %s\n", result.message.c_str());
        set_state(State::Failed);
        return;
    }
}

// MPPM_UPDATE_MOCK (see the header). Returns true when the mock handled the check.
bool apply_mock() {
    const char* mock = std::getenv("MPPM_UPDATE_MOCK");
    if (mock == nullptr || mock[0] == '\0') {
        return false;
    }
    const std::string_view value = mock;
    borealis::update::Result result;
    if (value == "checking") {
        set_state(State::Checking);
        return true;
    }
    if (value == "uptodate") {
        result.status = borealis::update::Status::UpToDate;
    } else if (value.starts_with("available")) {
        result.status         = borealis::update::Status::UpdateAvailable;
        result.latest.tagName = value.size() > 10 ? std::string(value.substr(10)) : "v0.4.0";
        result.latest.htmlUrl = std::string(branding::kReleasesUrl) + "/latest";
    } else if (value == "404") {
        result.message = "GitHub returned HTTP 404";
    } else {
        result.message = "mocked failure";
    }
    apply(result);
    return true;
}

} // namespace

void begin(bool force) {
    if (sTask) {
        return; // one request at a time
    }
    // The first-run screen asks for consent (Check for Updates): until it has been answered
    // (interface.presetChosen), only an explicit "Check Now" may contact GitHub.
    if (!force && (sStarted || !settings().interface.checkForUpdates.get() ||
                      !settings().interface.presetChosen.get()))
    {
        return;
    }
    sStarted = true;
    sLatestVersion.clear();
    sReleaseUrl.clear();
    if (apply_mock()) {
        return;
    }
    borealis::update::Options options;
    options.currentVersion = branding::kVersion;
    sTask = borealis::update::check_latest_github_release(kAppInfo, options);
    set_state(State::Checking);
}

void poll() {
    if (!sTask || !sTask.ready()) {
        return;
    }
    std::optional<borealis::update::Result> result;
    try {
        result = sTask.try_take();
    } catch (const std::exception& e) {
        result.emplace();
        result->message = e.what();
    } catch (...) {
        result.emplace();
        result->message = "unknown error";
    }
    sTask = {};
    if (result) {
        apply(*result);
    } else {
        set_state(State::Failed);
    }
}

State state() {
    return sState;
}

const std::string& latest_version() {
    return sLatestVersion;
}

std::string release_url() {
    if (!sReleaseUrl.empty()) {
        return sReleaseUrl;
    }
    return std::string(branding::kReleasesUrl) + "/latest";
}

bool open_release() {
    const std::string url = release_url();
    if (!SDL_OpenURL(url.c_str())) {
        std::fprintf(stderr, "mppm: could not open %s: %s\n", url.c_str(), SDL_GetError());
        return false;
    }
    return true;
}

std::string summary() {
    switch (sState) {
    case State::Idle:
        return {};
    case State::Checking:
        return "Checking for updates...";
    case State::UpToDate:
        return fmt::format("You're up to date (v{}).", strip_v(branding::kVersion));
    case State::Available:
        return fmt::format("{} v{} is available.", branding::kProductName, sLatestVersion);
    case State::Failed:
    case State::Quiet:
        // Quiet stays silent in the start menu; asked explicitly, the honest answer is this.
        return "Couldn't reach GitHub. Try again later.";
    }
    return {};
}

unsigned generation() {
    return sGeneration;
}

} // namespace mppm::launcher::update_check
