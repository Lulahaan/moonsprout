// Data-mod overlay (M5.2): see validate.hpp.
#include "validate.hpp"

#include <cstring>

namespace mppm::mods::pure {

namespace {

bool ends_with(std::string_view s, std::string_view suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// Calls fn(token) for every whitespace-separated token outside # comments. A NUL ends the text,
// as it does for the game's parser.
template <typename Fn> void for_each_token(std::string_view text, Fn fn)
{
	size_t i = 0;
	const size_t n = text.size();
	while (i < n) {
		const char c = text[i];
		if (c == '\0') return;
		if (c == '#') {
			while (i < n && text[i] != '\n' && text[i] != '\0') i++;
			continue;
		}
		if (space(c)) {
			i++;
			continue;
		}
		const size_t start = i;
		while (i < n && !space(text[i]) && text[i] != '#' && text[i] != '\0') i++;
		if (!fn(text.substr(start, i - start))) return;
	}
}

} // namespace

StageCount count_stages(std::string_view text)
{
	StageCount out;
	bool first = true;
	long depth = 0;
	for_each_token(text, [&](std::string_view tok) {
		if (first) {
			first = false;
			if (!tok.empty() && tok.size() <= 9 && tok.find_first_not_of("0123456789") == std::string_view::npos) {
				out.hasNumber = true;
				out.declared  = std::stol(std::string(tok));
			}
			return true;
		}
		if (tok == "{") {
			if (depth++ == 0) out.blocks++;
		} else if (tok == "}" && depth > 0) {
			depth--;
		}
		return true;
	});
	return out;
}

bool braces_balanced(std::string_view text)
{
	long depth = 0;
	bool ok    = true;
	for_each_token(text, [&](std::string_view tok) {
		if (tok == "{") depth++;
		else if (tok == "}" && --depth < 0) ok = false;
		return ok;
	});
	return ok && depth == 0;
}

void validate_file(const std::string& key, const std::string& entry, const uint8_t* data, size_t size,
                   std::vector<Issue>& issues)
{
	const std::string_view text(reinterpret_cast<const char*>(data), size);
	const bool loose = key.find(':') == std::string::npos;

	const bool vsList = key == "/user/abe/vs/stages.txt";
	const bool chList = key == "/user/matoba/challenge/stages.txt" || key == "/user/matoba/challenge/kfes-stages.txt";
	if (vsList || chList) {
		const char* what = vsList ? "The VS stage list" : "The Challenge stage list";
		const StageCount c = count_stages(text);
		if (!c.hasNumber) {
			issues.push_back({Severity::Error, entry, std::string(what) + " does not start with its number of stages."});
		} else if (c.declared != c.blocks) {
			issues.push_back({Severity::Error, entry,
			                  std::string(what) + " says " + std::to_string(c.declared) + " stages but has " +
			                      std::to_string(c.blocks) + "."});
		} else if (vsList && c.blocks < 1) {
			issues.push_back({Severity::Error, entry, "The VS stage list has no stage; it needs at least 1."});
		} else if (vsList && c.blocks > 10) {
			issues.push_back({Severity::Error, entry,
			                  "The VS stage list has " + std::to_string(c.blocks) +
			                      " stages; the stage select shows at most 10."});
		} else if (chList && c.blocks < 30) {
			issues.push_back({Severity::Error, entry,
			                  "The Challenge stage list has " + std::to_string(c.blocks) +
			                      " stages; it needs at least 30."});
		}
	}

	if (loose && (ends_with(key, ".szs") || ends_with(key, ".arc"))) {
		if (size < 4 || (std::memcmp(data, "Yaz0", 4) != 0 && std::memcmp(data, "RARC", 4) != 0)) {
			issues.push_back({Severity::Error, entry, "The file is not a Yaz0 or RARC archive."});
		}
	}

	if (ends_with(key, ".txt") || ends_with(key, ".ini")) {
		if (size > 0 && std::memchr(data, 0, size) != nullptr) {
			issues.push_back({Severity::Warning, entry, "The file contains a NUL byte; the game stops reading there."});
		}
		if (!braces_balanced(text)) {
			issues.push_back({Severity::Warning, entry, "The { and } in this file do not balance."});
		}
	}
}

} // namespace mppm::mods::pure
