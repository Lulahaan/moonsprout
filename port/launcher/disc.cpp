#include "disc.hpp"

#include <borealis/io.hpp>
#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

namespace mppm::disc {
namespace {

using namespace std::string_view_literals;

// The decomp is built for the North American release (GPVE01, revision 0). Reference: a 1459978240-
// byte image with SHA-1 63a68c656d654388e096d27a768e85823dcf3327 / CRC32 3541416b (the published
// GPVE01 dump, GameTDB), whose XXH3-128 over the decompressed disc is below.
constexpr std::array kAccepted{
    borealis::disc::AcceptedDisc{.gameId       = "GPVE01"sv,
                                 .discNumber   = 0,
                                 .revision     = 0,
                                 .expectedHash = borealis::disc::parse_xxh3_128("406caed0849fc17cda833f7b322a6ee5")},
};
// Other Pikmin 2 releases: recognised, but not playable with this build.
constexpr std::array kRecognized{"GPVP01"sv, "GPVJ01"sv, "R92E01"sv, "R92P01"sv, "R92J01"sv};

} // namespace

const borealis::disc::Catalog& catalog() {
    static const borealis::disc::Catalog sCatalog{
        .acceptedDiscs = kAccepted,
        .recognizedGameIds = kRecognized,
    };
    return sCatalog;
}

Info inspect(const std::string& path) {
    Info info;
    if (path.empty()) {
        return info;
    }
    std::error_code ec;
    // Gone for sure (no error asking): say so; anything else is for the reader to report.
    if (!std::filesystem::exists(borealis::io::fs_path_from_utf8(path), ec) && !ec) {
        info.state = State::Missing;
        return info;
    }
    const auto result = borealis::disc::inspect(path, catalog());
    info.gameId  = result.metadata.gameId;
    info.title   = result.metadata.title;
    info.message = result.message;
    switch (result.status) {
    case borealis::disc::Status::Success:
    case borealis::disc::Status::HashMismatch:
        info.state = State::Ready;
        break;
    case borealis::disc::Status::UnsupportedVersion:
        info.state = State::Unsupported;
        break;
    case borealis::disc::Status::UnknownGame:
        info.state = State::WrongGame;
        break;
    default:
        info.state = State::Invalid;
        break;
    }
    return info;
}

const char* state_label(State state) {
    switch (state) {
    case State::None:
        return "No disc image chosen";
    case State::Ready:
        return "Disc ready";
    case State::Unsupported:
        return "Not a supported release";
    case State::WrongGame:
        return "Not a Pikmin 2 disc";
    case State::Invalid:
        return "Not a readable disc image";
    case State::Missing:
        return "File not found";
    }
    return "";
}

// ---- Remembered verification ----

std::string fingerprint(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const auto file = borealis::io::fs_path_from_utf8(path);
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return {};
    }
    const auto mtime = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return {};
    }
    // Unix seconds, so the stored text reads the same on every platform and library.
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::file_clock::to_sys(mtime).time_since_epoch())
                             .count();
    return fmt::format("{}:{}", size, seconds);
}

DiscVerification remembered_verification(const std::string& path) {
    const auto& backend = settings().backend;
    const DiscVerification stored = backend.discVerification.get();
    if (stored == DiscVerification::Unknown || backend.discFingerprint.get().empty()) {
        return DiscVerification::Unknown;
    }
    return fingerprint(path) == backend.discFingerprint.get() ? stored : DiscVerification::Unknown;
}

bool needs_reverification(const std::string& path) {
    const auto& backend = settings().backend;
    return backend.discVerification.get() != DiscVerification::Unknown && !path.empty() &&
           remembered_verification(path) == DiscVerification::Unknown;
}

void remember_verification(const std::string& path, DiscVerification result) {
    auto& backend = settings().backend;
    const std::string print = result == DiscVerification::Unknown ? std::string{} : fingerprint(path);
    if (print.empty()) {
        result = DiscVerification::Unknown; // nothing to tie the result to
    }
    backend.discVerification.set(result);
    backend.discFingerprint.set(print);
}

void forget_verification() {
    remember_verification({}, DiscVerification::Unknown);
}

const char* verification_label(DiscVerification verification) {
    switch (verification) {
    case DiscVerification::Unknown:
        return "Not verified";
    case DiscVerification::Verified:
        return "Verified";
    case DiscVerification::MismatchAccepted:
        return "Doesn't match the known good copy";
    }
    return "";
}

// ---- Full verification on a worker thread ----

Verification::Verification(std::string path)
    : mPath(std::move(path))
    , mWorker([this] { run(); }) {}

Verification::~Verification() {
    cancel();
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void Verification::run() {
    Outcome outcome = Outcome::ReadError;
    try {
        const auto result = borealis::disc::verify(mPath, catalog(), &mProgress);
        switch (result.status) {
        case borealis::disc::Status::Success:
            outcome = Outcome::Verified;
            break;
        case borealis::disc::Status::HashMismatch:
            outcome = Outcome::Mismatch;
            break;
        case borealis::disc::Status::Canceled:
            outcome = Outcome::Canceled;
            break;
        case borealis::disc::Status::UnknownGame:
        case borealis::disc::Status::UnsupportedVersion:
            outcome = Outcome::WrongDisc;
            break;
        case borealis::disc::Status::IOError:
        case borealis::disc::Status::InvalidImage:
        case borealis::disc::Status::Failed:
            outcome = Outcome::ReadError;
            break;
        }
        if (outcome != Outcome::Verified && outcome != Outcome::Canceled) {
            std::fprintf(stderr, "mppm: disc verification of '%s': %s\n", mPath.c_str(),
                         !result.message.empty()          ? result.message.c_str()
                         : outcome == Outcome::Mismatch ? "XXH3-128 differs from the GPVE01 reference"
                                                        : "failed");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mppm: disc verification of '%s' threw: %s\n", mPath.c_str(), e.what());
    } catch (...) {
        std::fprintf(stderr, "mppm: disc verification of '%s' threw\n", mPath.c_str());
    }
    // After a cancel request, a failure is the cancel's doing; a result that completed still counts.
    if (cancel_requested() && outcome != Outcome::Verified && outcome != Outcome::Mismatch) {
        outcome = Outcome::Canceled;
    }
    mOutcome = outcome;
    mDone.store(true, std::memory_order_release);
}

const char* outcome_message(Outcome outcome) {
    switch (outcome) {
    case Outcome::Running:
        return "Checking the disc image...";
    case Outcome::Verified:
        return "This is a good copy of Pikmin 2 (USA). Every byte matches the known good dump.";
    case Outcome::Mismatch:
        return "This disc image doesn't match the known good copy of Pikmin 2 (USA). It may be "
               "damaged or modified. You can keep playing, but crashes may be caused by the image "
               "rather than by the port.";
    case Outcome::Canceled:
        return "Verification was cancelled. Nothing was changed.";
    case Outcome::WrongDisc:
        return "This is not the North American Pikmin 2 disc (GPVE01).";
    case Outcome::ReadError:
        return "The disc image couldn't be read to the end. Check that the file is complete and "
               "still in place.";
    }
    return "";
}

} // namespace mppm::disc
