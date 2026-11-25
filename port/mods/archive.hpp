#pragma once
// Data-mod overlay, milestone M5.2: in-archive overrides, the interface between the core package
// (port/mods/mods.cpp calls it during p2_mods_mount) and the archive package (implements it in
// port/mods/archive_mount.cpp). Committed in step 0 as port/mods/archive.hpp; only core edits it.
// Plan 7a "Second step": v1 replaces files that exist in the archive; adding files (v2) and nested
// archives are reported as errors by check().

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mppm::mods::archive {

// One archives/ file of an enabled, valid mod: the winner for its key (last wins).
struct Entry {
    std::string modId;       // for messages
    std::string archivePath; // normalized disc path of the archive, e.g. "/enemy/parm/enemyparms.szs"
    std::string innerPath;   // normalized path inside the archive, relative to its root directory,
                             // e.g. "kochappy/enemyparm.txt"
    const uint8_t* data = nullptr; // port-owned by core for the process: 32-byte aligned, size + 32
    uint32_t size = 0;             // readable bytes with the 32 after `size` zero
};

struct Problem {
    size_t entry = 0;    // index into the entries passed in
    std::string message; // one plain-text sentence, ending with a period, e.g.
                         // "The archive /enemy/parm/enemyparms.szs has no file kochappy/enemyparam.txt."
};

// Reads each referenced archive once through the Aurora DVD API (DVDOpen / DVDReadPrio / DVDClose),
// after the loose overlay is registered, so a mod that replaces a whole archive is seen as the game
// will see it. Yaz0-decodes it (bounded), checks it is a RARC, and that every inner path names a
// file (not a directory) of that archive. More than 254 distinct archives is a problem for the
// entries past the limit. No state changes. Empty result = all entries can be installed.
std::vector<Problem> check(const std::vector<Entry>& entries);

// Builds the registry behind p2_mods_arc_* from entries that passed check() (reads each archive
// again for its content key and file indices). Called once per process in M5.2, before the game
// starts; the registry is immutable afterwards. Logs one warning when two archives have identical
// tables (buffer mounts of either then use the first set).
void install(const std::vector<Entry>& entries);

} // namespace mppm::mods::archive
