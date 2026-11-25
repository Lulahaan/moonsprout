#pragma once
// Data-mod overlay (M5.2): manifest path rules, keys and text helpers (design 4.2, 4.4).
// Pure: no Aurora, SDL or p2_net.

#include <cstddef>
#include <string>
#include <string_view>

namespace mppm::mods::pure {

// Result of checking one manifest "files" entry (design 4.4).
struct EntryKey {
	bool ok = false;
	std::string error;       // one sentence when !ok
	std::string key;         // "/user/abe/vs/stages.txt" or "/enemy/parm/enemyparms.szs:kochappy/enemyparm.txt"
	bool archive = false;    // under archives/
	std::string archivePath; // archive: "/enemy/parm/enemyparms.szs"
	std::string innerPath;   // archive: "kochappy/enemyparm.txt"
};

// Checks an entry such as "files/user/abe/vs/stages.txt" or
// "archives/enemy/parm/enemyParms.szs/kochappy/enemyparm.txt" and computes its key.
EntryKey entry_key(std::string_view entry);

// A mod id: ^[a-z0-9-]{1,64}$.
bool valid_id(std::string_view id);

// Removes C0 controls, DEL and C1 controls (U+0080..U+009F) from UTF-8 text. Other bytes are kept.
std::string strip_controls(std::string_view text);

// Number of UTF-8 code points (bytes that are not continuation bytes).
size_t utf8_length(std::string_view text);

// strip_controls, then cut to at most `maxBytes` bytes on a code point boundary.
std::string clip_text(std::string_view text, size_t maxBytes);

// ASCII lower case.
std::string ascii_lower(std::string_view text);

} // namespace mppm::mods::pure
