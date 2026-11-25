#pragma once
// Data-mod overlay (M5.2): mod.json (design 4.2). Pure: no Aurora, SDL or p2_net.

#include "p2_mods.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mppm::mods::pure {

constexpr size_t kManifestMaxBytes = 64 * 1024;
constexpr int kManifestMaxDepth    = 16;
constexpr size_t kManifestMaxFiles = 4096;

struct Manifest {
	std::string id;          // as written when it is a valid id
	std::string version;     // "" when missing or malformed
	std::string name;        // control characters removed; the folder id when missing
	std::string author;
	std::string description;
	std::string minMppm;     // as written (">=0.1"), control characters removed
	bool cosmetic = false;
	std::vector<std::string> files; // the raw "files" strings (checked later with entry_key)
	std::vector<Issue> issues;      // Error / Warning, path "" (the manifest itself)
};

// Largest nesting depth of [ and { outside JSON strings (unbalanced input counts as written).
int json_depth(std::string_view text);

// Parses and checks mod.json text for the mod in folder `folderId`. `buildVersion` is
// MPPM_VERSION_STRING (its numeric prefix is compared with "mppm"). Never throws.
Manifest parse_manifest(std::string_view text, std::string_view folderId, std::string_view buildVersion);

// MAJOR.MINOR.PATCH with optional -pre and +build ([0-9A-Za-z.-]), at most 32 characters.
bool valid_semver(std::string_view version);

// The numeric prefix of a version string: "0.1.0-dev+abc" -> {0, 1, 0}. Missing parts are 0.
struct Version3 {
	unsigned long major = 0, minor = 0, patch = 0;
};
Version3 numeric_prefix(std::string_view version);

// ">=X", ">=X.Y" or ">=X.Y.Z" (each 1..9 digits). False when malformed.
bool parse_min_mppm(std::string_view text, Version3* out);

} // namespace mppm::mods::pure
