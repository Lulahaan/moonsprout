#pragma once
// Data-mod overlay, milestone M5.2: the pure part of in-archive overrides (no Aurora, no SDL, no game
// headers), so the selftest can run it on synthetic data. Every function treats its input as untrusted:
// offsets and counts are bounded, nothing reads past `n`, and a malformed input is an error, never a
// crash. Used by port/mods/archive_mount.cpp and tests/mods/archive_test.cpp.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mppm::mods::archive_index {

constexpr size_t kMaxDecoded = 64u << 20;      // Yaz0 output limit
constexpr uint32_t kMaxFolders = 65535;        // RARC directory entries
constexpr uint32_t kMaxFileEntries = 1u << 20; // RARC file entries (files and folder links)
constexpr int kMaxDepth = 32;                  // folder nesting below the root
constexpr uint32_t kMaxInfoKeyBytes = 16u << 20;

// True when the data starts with the Yaz0 magic.
bool is_yaz0(const uint8_t* src, size_t n);

// Decodes a whole Yaz0 stream (16-byte header: "Yaz0", BE u32 decoded size, 8 reserved). Fails on a
// decoded size over kMaxDecoded, a source overrun (the stream ends before the decoded size is reached)
// and a back reference before the start of the output. On failure `out` is cleared and `error` (when
// given) holds a short reason.
bool yaz0_decode(const uint8_t* src, size_t n, std::vector<uint8_t>& out, std::string* error = nullptr);

struct Node {
    uint32_t fileIndex = 0; // index into the archive's file table (the entry that names this path)
    bool isFolder = false;
};

struct Index {
    bool ok = false;
    std::string error;  // short reason when !ok
    uint64_t key = 0;   // info_key of the info block (0 when it cannot be computed)
    // lower-case inner path relative to the root folder ("kochappy/enemyparm.txt", "kochappy") -> node.
    // A path that occurs twice (names differing only in case) keeps its first entry.
    std::map<std::string, Node> paths;
};

// Indexes a decoded RARC archive held in data[0, n): checks the "RARC" magic, the header length (BE u32
// at 0x08) and every table offset and count against n, walks the folders from folder 0 (each at most
// once, depth <= kMaxDepth) skipping "." and "..", and records every file and folder.
Index rarc_index(const uint8_t* data, size_t n);

// FNV-1a 64 over the raw (disc byte order) info block [0, end), end = be32(+0x14) + be32(+0x10)
// (string table offset plus length). 0 when avail < 0x20, end < 0x20, end > kMaxInfoKeyBytes or
// end > avail.
uint64_t info_key(const void* raw, size_t avail);

// FNV-1a 64 of a byte range (basis 0xcbf29ce484222325, prime 0x100000001b3).
uint64_t fnv1a64(const uint8_t* p, size_t n);

} // namespace mppm::mods::archive_index
