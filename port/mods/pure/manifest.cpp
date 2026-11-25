// Data-mod overlay (M5.2): see manifest.hpp.
#include "manifest.hpp"

#include "paths.hpp"

#include <nlohmann/json.hpp>

namespace mppm::mods::pure {

namespace {

void error(Manifest& m, std::string message)
{
	m.issues.push_back({Severity::Error, "", std::move(message)});
}

bool all_digits(std::string_view s, size_t maxLen)
{
	if (s.empty() || s.size() > maxLen) return false;
	for (const char c : s) {
		if (c < '0' || c > '9') return false;
	}
	return true;
}

bool ident_chars(std::string_view s)
{
	if (s.empty()) return false;
	for (const char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) {
			return false;
		}
	}
	return true;
}

int compare(const Version3& a, const Version3& b)
{
	if (a.major != b.major) return a.major < b.major ? -1 : 1;
	if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
	if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
	return 0;
}

// Optional string key with a code point limit; sets *out (control characters removed).
void optional_string(Manifest& m, const nlohmann::json& root, const char* key, size_t maxChars, std::string* out)
{
	const auto it = root.find(key);
	if (it == root.end()) return;
	if (!it->is_string()) {
		error(m, std::string("\"") + key + "\" must be a string.");
		return;
	}
	std::string text = strip_controls(it->get_ref<const std::string&>());
	if (utf8_length(text) > maxChars) {
		error(m, std::string("\"") + key + "\" is longer than " + std::to_string(maxChars) + " characters.");
		return;
	}
	*out = std::move(text);
}

} // namespace

int json_depth(std::string_view text)
{
	int depth = 0, maxDepth = 0;
	bool inString = false, escaped = false;
	for (const char c : text) {
		if (inString) {
			if (escaped) escaped = false;
			else if (c == '\\') escaped = true;
			else if (c == '"') inString = false;
			continue;
		}
		if (c == '"') inString = true;
		else if (c == '[' || c == '{') {
			if (++depth > maxDepth) maxDepth = depth;
		} else if ((c == ']' || c == '}') && depth > 0) {
			depth--;
		}
	}
	return maxDepth;
}

bool valid_semver(std::string_view v)
{
	if (v.empty() || v.size() > 32) return false;
	size_t plus = v.find('+');
	if (plus != std::string_view::npos && !ident_chars(v.substr(plus + 1))) return false;
	std::string_view core = v.substr(0, plus);
	const size_t dash = core.find('-');
	if (dash != std::string_view::npos && !ident_chars(core.substr(dash + 1))) return false;
	core = core.substr(0, dash);
	for (int part = 0; part < 3; part++) {
		const size_t dot = core.find('.');
		if ((part < 2) == (dot == std::string_view::npos)) return false;
		const std::string_view num = core.substr(0, dot);
		if (!all_digits(num, 9)) return false;
		if (num.size() > 1 && num[0] == '0') return false;
		core = dot == std::string_view::npos ? std::string_view() : core.substr(dot + 1);
	}
	return true;
}

Version3 numeric_prefix(std::string_view v)
{
	Version3 out;
	unsigned long* parts[3] = {&out.major, &out.minor, &out.patch};
	size_t i = 0;
	for (int p = 0; p < 3; p++) {
		size_t digits = 0;
		unsigned long n = 0;
		while (i < v.size() && v[i] >= '0' && v[i] <= '9' && digits < 9) {
			n = n * 10 + (unsigned long)(v[i] - '0');
			i++;
			digits++;
		}
		if (digits == 0) break;
		*parts[p] = n;
		if (i >= v.size() || v[i] != '.') break;
		i++;
	}
	return out;
}

bool parse_min_mppm(std::string_view text, Version3* out)
{
	if (text.size() < 3 || text.substr(0, 2) != ">=") return false;
	std::string_view rest = text.substr(2);
	Version3 v;
	unsigned long* parts[3] = {&v.major, &v.minor, &v.patch};
	for (int p = 0; p < 3; p++) {
		const size_t dot = rest.find('.');
		const std::string_view num = rest.substr(0, dot);
		if (!all_digits(num, 9)) return false;
		*parts[p] = std::stoul(std::string(num));
		if (dot == std::string_view::npos) {
			*out = v;
			return true;
		}
		rest = rest.substr(dot + 1);
	}
	return false; // a fourth part
}

Manifest parse_manifest(std::string_view text, std::string_view folderId, std::string_view buildVersion)
{
	Manifest m;
	m.name = std::string(folderId);
	if (text.size() > kManifestMaxBytes) {
		error(m, "mod.json is larger than 64 KiB.");
		return m;
	}
	if (json_depth(text) > kManifestMaxDepth) {
		error(m, "mod.json nests deeper than 16 levels.");
		return m;
	}
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
	} catch (...) {
		root = nlohmann::json(nlohmann::json::value_t::discarded);
	}
	if (root.is_discarded()) {
		error(m, "mod.json is not valid JSON.");
		return m;
	}
	if (!root.is_object()) {
		error(m, "mod.json must hold a JSON object.");
		return m;
	}

	// id
	if (const auto it = root.find("id"); it == root.end()) {
		error(m, "\"id\" is missing.");
	} else if (!it->is_string()) {
		error(m, "\"id\" must be a string.");
	} else {
		const std::string& id = it->get_ref<const std::string&>();
		if (!valid_id(id)) error(m, "\"id\" must be 1 to 64 characters of a-z, 0-9 and -.");
		else if (id != folderId) error(m, "\"id\" must equal the folder name.");
		else m.id = id;
	}

	// version
	if (const auto it = root.find("version"); it == root.end()) {
		error(m, "\"version\" is missing.");
	} else if (!it->is_string() || !valid_semver(it->get_ref<const std::string&>())) {
		error(m, "\"version\" must be a version like 1.0.0 (at most 32 characters).");
	} else {
		m.version = it->get_ref<const std::string&>();
	}

	// name
	if (const auto it = root.find("name"); it != root.end()) {
		if (!it->is_string()) {
			error(m, "\"name\" must be a string.");
		} else {
			std::string name = strip_controls(it->get_ref<const std::string&>());
			const size_t len = utf8_length(name);
			if (len == 0 || len > 64) error(m, "\"name\" must be 1 to 64 characters.");
			else m.name = std::move(name);
		}
	}

	// mppm
	if (const auto it = root.find("mppm"); it != root.end()) {
		Version3 need;
		if (!it->is_string()) {
			error(m, "\"mppm\" must be a string such as \">=0.1\".");
		} else if (!parse_min_mppm(it->get_ref<const std::string&>(), &need)) {
			m.minMppm = clip_text(it->get_ref<const std::string&>(), 32);
			error(m, "\"mppm\" must look like >=1, >=1.2 or >=1.2.3.");
		} else {
			m.minMppm = it->get_ref<const std::string&>();
			if (compare(need, numeric_prefix(buildVersion)) > 0) {
				error(m, "Needs Moonsprout " + m.minMppm.substr(2) + " or newer.");
			}
		}
	}

	// files
	if (const auto it = root.find("files"); it == root.end()) {
		error(m, "\"files\" is missing.");
	} else if (!it->is_array()) {
		error(m, "\"files\" must be an array of paths.");
	} else if (it->empty() || it->size() > kManifestMaxFiles) {
		error(m, "\"files\" must list 1 to 4096 paths.");
	} else {
		bool allStrings = true;
		for (const auto& f : *it) {
			if (!f.is_string()) {
				allStrings = false;
				continue;
			}
			m.files.push_back(f.get_ref<const std::string&>());
		}
		if (!allStrings) error(m, "\"files\" must contain only strings.");
	}

	optional_string(m, root, "author", 64, &m.author);
	optional_string(m, root, "description", 1024, &m.description);

	if (const auto it = root.find("affects"); it != root.end()) {
		if (it->is_string() && it->get_ref<const std::string&>() == "gameplay") m.cosmetic = false;
		else if (it->is_string() && it->get_ref<const std::string&>() == "cosmetic") m.cosmetic = true;
		else error(m, "\"affects\" must be \"gameplay\" or \"cosmetic\".");
	}

	static const char* const kKnown[] = {"id", "version", "name", "mppm", "files", "author", "description", "affects"};
	for (const auto& item : root.items()) {
		bool known = false;
		for (const char* k : kKnown) known = known || item.key() == k;
		if (!known) {
			m.issues.push_back({Severity::Warning, "", "Unknown key \"" + clip_text(item.key(), 64) + "\" ignored."});
		}
	}
	return m;
}

} // namespace mppm::mods::pure
