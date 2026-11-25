#pragma once
// Disc image checks: a quick header inspection (game id / revision, through nod so compressed
// images work), the catalog of discs this build can run, and the full verification behind the
// "Verify Disc" button (docs/launcher/SETTINGS.md 2).
//
// Verification reads the whole decompressed disc and compares its XXH3-128 with the reference
// GPVE01 dump. It takes about a minute, so it runs on a worker thread (Verification) and its
// result is remembered in backend.discVerification, tied to the file by backend.discFingerprint
// ("<size>:<mtime>"): when the file changes, the remembered result no longer counts.

#include "settings.hpp"

#include <borealis/disc.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace mppm::disc {

enum class State {
    None,        // no disc chosen
    Ready,       // Pikmin 2 (USA) revision 0
    Unsupported, // Pikmin 2, but another region/revision
    WrongGame,   // another GameCube/Wii game
    Invalid,     // not a disc image / unreadable
    Missing,     // the file is not there (moved, renamed, unmounted drive)
};

// The image formats the file dialog offers, one list for every place that names them.
inline constexpr const char* kImageFilter = "iso;gcm;ciso;gcz;rvz;wia;nfs;tgc";
inline constexpr const char* kFormatList  = "ISO, GCM, CISO, GCZ, RVZ, WIA, NFS or TGC";
// The same with a no-break space before the last one, for wrapped text (no lone "TGC." line).
inline constexpr const char* kFormatListNoBreak = "ISO, GCM, CISO, GCZ, RVZ, WIA, NFS or\u00A0TGC";

struct Info {
    State state = State::None;
    std::string gameId;
    std::string title;
    std::string message;
};

const borealis::disc::Catalog& catalog();

// Fast: reads the disc header only.
Info inspect(const std::string& path);

// Short status, the same words on the start menu and in Settings: "Disc ready",
// "Not a readable disc image", "File not found", ...
const char* state_label(State state);

// ---- Remembered verification ----

// "<size>:<mtime>" of a disc file (mtime in seconds of the filesystem clock), or "" when the file
// can't be read. Two runs on the same unchanged file give the same text.
std::string fingerprint(const std::string& path);

// The stored result of Verify Disc for this file: Verified or MismatchAccepted while the file's
// fingerprint still matches the one stored with it, else Unknown.
DiscVerification remembered_verification(const std::string& path);

// True when the file was verified (or accepted) once and has changed since: the one case where
// the launcher re-runs the verification on its own (SETTINGS 2, Verify Disc).
bool needs_reverification(const std::string& path);

// Stores a result with the file's current fingerprint. Unknown clears both keys.
void remember_verification(const std::string& path, DiscVerification result);

// Clears the stored result (call when backend.discPath changes).
void forget_verification();

// Short status for the Verify Disc row: "Verified", "Not verified", ...
const char* verification_label(DiscVerification verification);

// ---- Full verification on a worker thread ----

enum class Outcome {
    Running,     // not finished yet
    Verified,    // hash matches the reference dump
    Mismatch,    // a Pikmin 2 (USA) disc whose data differs from the reference dump
    Canceled,    // cancel() was called
    WrongDisc,   // not the North American Pikmin 2 disc
    ReadError,   // missing, unreadable or not a disc image
};

// One verification run. Starts at construction; the destructor cancels and joins, so dropping
// the object at any time is safe (it may wait for the image to finish opening).
class Verification {
public:
    explicit Verification(std::string path);
    ~Verification();

    Verification(const Verification&)            = delete;
    Verification& operator=(const Verification&) = delete;

    const std::string& path() const { return mPath; }
    bool finished() const { return mDone.load(std::memory_order_acquire); }
    // Valid once finished(); Running before.
    Outcome outcome() const { return finished() ? mOutcome : Outcome::Running; }

    // Asks the worker to stop at its next block; outcome() becomes Canceled.
    void cancel() { mProgress.cancelRequested.store(true, std::memory_order_relaxed); }
    bool cancel_requested() const { return mProgress.cancelRequested.load(std::memory_order_relaxed); }

    // Bytes hashed so far and in total; total is 0 while the image is still being opened.
    uint64_t bytes_read() const { return mProgress.bytesRead.load(std::memory_order_relaxed); }
    uint64_t bytes_total() const { return mProgress.bytesTotal.load(std::memory_order_relaxed); }

private:
    void run();

    std::string mPath;
    borealis::disc::Progress mProgress;
    Outcome mOutcome = Outcome::Running;
    std::atomic_bool mDone = false;
    std::thread mWorker; // last: starts after every other member exists
};

// User-facing sentence for a finished outcome.
const char* outcome_message(Outcome outcome);

} // namespace mppm::disc
