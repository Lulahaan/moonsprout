#pragma once
// Data-mod overlay, milestone M5.2: the C++ view of the mods folder for the launcher's Mods window
// (port/launcher/mods_window.cpp). Implemented by the core package in port/mods/mods.cpp on top of
// port/mods/pure/modset.*. Committed in step 0; only core edits it.
//
// Host C++ (exceptions on, std::filesystem). Main thread only unless a function says otherwise.
// Nothing here throws: every problem becomes an Issue.
//
// Untrusted input: name, author, description, ids and every Issue::message may contain text from a
// mod. Escape all of it before it goes into RML.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mppm::mods {

using Hash = std::array<uint8_t, 32>;

enum class Severity : uint8_t {
    Warning, // shown; the mod still loads
    Error,   // the mod does not load
};

struct Issue {
    Severity severity = Severity::Error;
    std::string path;    // the manifest entry it is about ("files/user/abe/vs/stages.txt"); "" = the mod or folder
    std::string message; // one plain-text sentence, ending with a period
};

struct ModInfo {
    std::string id;          // the folder name (always set)
    std::string name;        // manifest "name", else the id
    std::string version;     // manifest "version"; "" when it did not parse
    std::string author;      // manifest "author" (optional)
    std::string description; // manifest "description" (optional)
    std::string minMppm;     // manifest "mppm" as written, e.g. ">=0.1"; "" when absent
    bool cosmetic = false;   // manifest "affects": "cosmetic" (default "gameplay"); for M5.3b
    bool listed = false;     // has a line in load_order.txt
    bool enabled = false;    // listed without '-'
    bool present = true;     // the folder exists (false: listed in load_order.txt but missing)
    bool valid = false;      // no Error issue (manifest, files, disc-independent validators;
                             // after the mount also the disc-dependent checks, see mounted())
    uint32_t fileCount = 0;  // files listed in the manifest
    uint64_t totalBytes = 0; // their sizes
    std::vector<std::string> keys; // normalized keys of the listed files, sorted bytewise:
                                   // "/user/abe/vs/stages.txt", "/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt"
    std::vector<Issue> issues;
    std::filesystem::path dir;     // the mod's folder
};

struct Conflict {
    std::string key;                 // normalized key
    std::string winner;              // id of the mod whose file is used (lowest in load_order.txt)
    std::vector<std::string> losers; // ids of the earlier enabled mods with the same key, nearest first
    std::string archiveMod;          // inner keys ("<archive>:<inner>"): the id of the mod whose loose
                                     // file replaces the whole archive, when it takes part without
                                     // holding the key itself; "" otherwise. That mod counts as giving
                                     // the key at its place in the load order (README 5).
};

struct Scan {
    std::filesystem::path dir;       // the mods folder; empty when mods are off for this run
    bool off = false;                // --no-mods / MPPM_NO_MODS=1
    bool exists = false;             // the folder exists
    std::vector<ModInfo> mods;       // listed mods in load_order.txt order (top = lowest priority),
                                     // then folders not listed (new, off), sorted by id
    std::vector<Conflict> conflicts; // among enabled valid mods, sorted by key
    std::vector<Issue> issues;       // folder level: load_order.txt, unreadable folder, limits
    int enabledValid = 0;            // mods that would mount
    Hash modsHash{};                 // of the mods that would mount, in order; zero when none
    std::vector<uint8_t> setBytes;   // the serialized mod set the hashes are computed from
};

// Reads the mods folder now: load_order.txt, every manifest, every listed file (hashed), the
// disc-independent validators and the conflicts. Can take a while for large mods (it reads up to
// 256 MiB per mod, off mods included): thread-safe, and the Mods window calls it on a worker.
Scan scan();

// What scan() reads from the module's state (the folder, --no-mods), taken on the calling thread so
// a worker thread never touches that state (it may still run while the process exits).
struct ScanArgs {
    std::filesystem::path dir;
    bool off = false;
};
ScanArgs scan_args(); // main thread
Scan scan(const ScanArgs& args); // any thread; scan() is scan(scan_args())

// What p2_mods_mount used. `ran` is false before the mount (start menu before Play). Disc-dependent
// problems (archive missing, inner file not in the archive, overlay refused a path) are added to the
// failed mods' issues there, with valid = false.
struct Mounted {
    bool ran = false;
    Scan scan;
    Hash folderHash{}; // the mods hash of the folder as the boot read it, before the mount dropped
                       // any mod (disc-dependent checks); equals scan.modsHash when none was dropped.
                       // A folder scan with another mods hash is a change to apply (restart).
};
const Mounted& mounted();

// Writes load_order.txt atomically (temporary file, then rename) in the mods folder, creating the
// folder: a comment header, then one line per entry, top = lowest priority, "-<id>" when off.
// Returns false and sets *error (one sentence) when it cannot. False with "Mods are off for this
// run." when mods are off.
struct OrderEntry {
    std::string id;
    bool enabled = false;
};
bool write_load_order(const std::vector<OrderEntry>& order, std::string* error);

// Creates the mods folder if it does not exist (for "Open Mods Folder").
bool ensure_dir(std::string* error);

// The content hash of a scan's mod set (plan 7a; equals p2_mods_content_hash for mounted().scan).
// Blocks while the build id is computed the first time (it reads the executable): call it from a
// worker thread. Thread-safe.
Hash content_hash(const Scan& scan);

// Lower-case hex of the first `digits` hex digits (default all 64).
std::string hex(const Hash& hash, size_t digits = 64);

// Why the mod list cannot be changed now ("" when it can): "Locked while a replay or harness
// session runs." while p2_net_active(); "Mods are off for this run." with --no-mods.
std::string locked_reason();

} // namespace mppm::mods
