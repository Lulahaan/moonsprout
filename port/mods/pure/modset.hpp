#pragma once
// Data-mod overlay (M5.2): scanning a mods folder, conflicts, the set serialization S and the two
// hashes (design 4.1, 4.5, 4.7, 4.8). Pure: no Aurora, SDL or p2_net (the build id is a parameter).

#include "p2_mods.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mppm::mods::pure {

constexpr size_t kMaxModFolders       = 256;
constexpr uint64_t kMaxFileBytes      = 64ull << 20;
constexpr uint64_t kMaxInnerFileBytes = 16ull << 20;
constexpr uint64_t kMaxModBytes       = 256ull << 20;
constexpr uint64_t kMaxAllModsBytes   = 512ull << 20;

// One listed file of a mod.
struct FileRec {
	std::string entry;       // the manifest entry (control characters removed, clipped)
	bool ok = false;         // path rules passed and the file is a regular file inside the mod
	std::string key;         // normalized key
	bool archive = false;
	std::string archivePath; // archive entries: normalized disc path of the archive
	std::string innerPath;   // archive entries: normalized inner path
	std::filesystem::path path; // on disk
	uint64_t size = 0;
	bool hashed   = false;   // read and hashed by the scan
	Hash sha{};
};

// A Scan plus the file records of each mod (files[i] belongs to scan.mods[i]).
struct FullScan {
	Scan scan;
	std::vector<std::vector<FileRec>> files;
};

struct ScanOptions {
	bool off       = false; // mods are off for this run: an empty Scan with off = true
	bool readAll   = true;  // also read and hash the files of mods that are not on (the launcher's
	                        // view, at most 512 MiB of them in all); false checks them by name
	                        // and size only (the mount)
};

// Reads the mods folder `dir` (design 4.1-4.6, 4.8). `buildVersion` is MPPM_VERSION_STRING. Fills
// every Scan field, including conflicts, setBytes and modsHash (finish_set). Never throws.
FullScan scan_dir(const std::filesystem::path& dir, std::string_view buildVersion, const ScanOptions& options);

// Recomputes the set-level fields from the mods' current `valid` flags: the 512 MiB limit for all
// enabled mods (marks the mods past it invalid), enabledValid, conflicts, setBytes, modsHash.
void finish_set(FullScan& full);

// The set serialization S (design 4.7).
struct SetFile {
	std::string key;
	uint64_t size = 0;
	Hash sha{};
};
struct SetMod {
	std::string id;
	std::string version;
	std::vector<SetFile> files; // any order: sorted bytewise by key here
};
std::vector<uint8_t> serialize_set(const std::vector<SetMod>& mods);

// SHA-256("mppm-mods-v1" || S); 32 zero bytes when `count` is 0.
Hash mods_hash_of(const std::vector<uint8_t>& setBytes, int count);
// SHA-256("mppm-content-v1" || u64le(buildId) || S).
Hash content_hash_of(uint64_t buildId, const std::vector<uint8_t>& setBytes);

// The archive of an inner key ("/a.szs:x/y" -> "/a.szs"); "" for a loose key.
std::string archive_of_key(std::string_view key);

struct KeyedMod {
	std::string id;
	std::vector<std::string> keys;
};

// Which mod's file is used for each key, the mods given in load order (README 5 and 6): the last
// mod with the key, except for an inner key ("<archive>:<inner>") when a later mod replaces the
// whole archive (loose key "<archive>"): that mod's archive is used as it is, so the key has no
// entry. Returns key -> index into `mods`.
std::map<std::string, size_t> pick_winners(const std::vector<KeyedMod>& mods);

// Conflicts among the mods given in load order (id, keys): winner = the last, losers nearest first,
// sorted by key. For an inner key, the last mod that replaces its whole archive takes part as if it
// gave the key at its place (Conflict::archiveMod), so the winner agrees with pick_winners.
std::vector<Conflict> find_conflicts(const std::vector<KeyedMod>& mods);

// The folder warning for a mod folder whose name is not a mod id (it can never be listed or load).
extern const char* const kBadFolderName;

// Problems of mods that are not on which the mount still logs (enabled mods log all theirs): a
// folder whose name is not a mod id, and a load_order.txt line whose folder is missing. Pairs of
// (mod id, issue) in scan order.
std::vector<std::pair<std::string, Issue>> off_notices(const Scan& scan);

std::string hex_of(const Hash& hash, size_t digits = 64);

// Reads a whole regular file of at most maxBytes into *out. False when it cannot.
bool read_file(const std::filesystem::path& path, uint64_t maxBytes, std::string* out);

// std::fopen for a filesystem path (wide on Windows, where path::c_str() is wchar_t*). mode is
// ASCII ("rb", "wb").
std::FILE* open_file(const std::filesystem::path& path, const char* mode);

} // namespace mppm::mods::pure
