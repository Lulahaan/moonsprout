// Data-mod overlay (M5.2): see paths.hpp.
#include "paths.hpp"

#include <vector>

namespace mppm::mods::pure {

namespace {

constexpr size_t kMaxEntryBytes     = 240;
constexpr size_t kMaxComponents     = 16;
constexpr size_t kMaxComponentBytes = 64;

bool component_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
	       c == '-';
}

// Windows device names, with or without an extension: "con", "NUL.txt", "com1.bin".
bool device_name(std::string_view comp)
{
	const size_t dot = comp.find('.');
	const std::string base = ascii_lower(comp.substr(0, dot));
	if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
	if (base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' &&
	    base[3] <= '9') {
		return true;
	}
	return false;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

EntryKey fail(const char* message)
{
	EntryKey r;
	r.error = message;
	return r;
}

} // namespace

std::string ascii_lower(std::string_view text)
{
	std::string out(text);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	}
	return out;
}

EntryKey entry_key(std::string_view entry)
{
	if (entry.empty()) return fail("The path is empty.");
	if (entry.size() > kMaxEntryBytes) return fail("The path is longer than 240 bytes.");
	if (entry.front() == '/') return fail("The path must be relative to the mod folder (no leading /).");
	if (entry.find('\\') != std::string_view::npos) return fail("The path uses a backslash; use / between folders.");
	if (entry.find(':') != std::string_view::npos) return fail("The path contains a colon.");

	std::vector<std::string_view> comps;
	size_t start = 0;
	while (true) {
		const size_t slash = entry.find('/', start);
		const std::string_view comp = entry.substr(start, slash == std::string_view::npos ? std::string_view::npos
		                                                                                    : slash - start);
		if (comp.empty()) return fail("The path has an empty folder name (// or a trailing /).");
		comps.push_back(comp);
		if (comps.size() > kMaxComponents) return fail("The path has more than 16 parts.");
		if (slash == std::string_view::npos) break;
		start = slash + 1;
	}
	for (const std::string_view comp : comps) {
		if (comp == "." || comp == "..") return fail("The path contains a . or .. part.");
		if (comp.size() > kMaxComponentBytes) return fail("A part of the path is longer than 64 bytes.");
		for (const char c : comp) {
			if (!component_char(c)) {
				return fail("The path contains a character other than letters, digits, '.', '_' and '-'.");
			}
		}
		if (comp.front() == '.') return fail("A part of the path starts with a dot.");
		if (comp.back() == '.') return fail("A part of the path ends with a dot.");
		if (device_name(comp)) return fail("A part of the path is a reserved device name (con, nul, com1 ...).");
	}

	EntryKey r;
	if (comps[0] == "files") {
		if (comps.size() < 2) return fail("The path names no file under files/.");
		std::string key;
		for (size_t i = 1; i < comps.size(); i++) {
			key += '/';
			key += ascii_lower(comps[i]);
		}
		r.ok  = true;
		r.key = std::move(key);
		return r;
	}
	if (comps[0] == "archives") {
		size_t arcEnd = 0; // index of the archive component
		for (size_t i = 1; i < comps.size(); i++) {
			const std::string low = ascii_lower(comps[i]);
			if (ends_with(low, ".szs") || ends_with(low, ".arc")) {
				arcEnd = i;
				break;
			}
		}
		if (arcEnd == 0) return fail("The path under archives/ names no .szs or .arc archive.");
		if (arcEnd + 1 >= comps.size()) return fail("The path names an archive but no file inside it.");
		for (size_t i = 1; i <= arcEnd; i++) {
			r.archivePath += '/';
			r.archivePath += ascii_lower(comps[i]);
		}
		for (size_t i = arcEnd + 1; i < comps.size(); i++) {
			if (!r.innerPath.empty()) r.innerPath += '/';
			r.innerPath += ascii_lower(comps[i]);
		}
		r.ok      = true;
		r.archive = true;
		r.key     = r.archivePath + ":" + r.innerPath;
		return r;
	}
	return fail("The path must start with files/ or archives/.");
}

bool valid_id(std::string_view id)
{
	if (id.empty() || id.size() > 64) return false;
	for (const char c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
	}
	return true;
}

std::string strip_controls(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		const unsigned char c = (unsigned char)text[i];
		if (c < 0x20 || c == 0x7f) continue;
		if (c == 0xc2 && i + 1 < text.size()) {
			const unsigned char n = (unsigned char)text[i + 1];
			if (n >= 0x80 && n <= 0x9f) { // U+0080..U+009F
				i++;
				continue;
			}
		}
		out += (char)c;
	}
	return out;
}

size_t utf8_length(std::string_view text)
{
	size_t n = 0;
	for (const char c : text) {
		if (((unsigned char)c & 0xc0) != 0x80) n++;
	}
	return n;
}

std::string clip_text(std::string_view text, size_t maxBytes)
{
	std::string s = strip_controls(text);
	if (s.size() <= maxBytes) return s;
	size_t cut = maxBytes;
	while (cut > 0 && ((unsigned char)s[cut] & 0xc0) == 0x80) cut--;
	s.resize(cut);
	return s;
}

} // namespace mppm::mods::pure
