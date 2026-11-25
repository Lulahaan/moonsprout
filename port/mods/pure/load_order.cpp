// Data-mod overlay (M5.2): see load_order.hpp.
#include "load_order.hpp"

#include "paths.hpp"

namespace mppm::mods::pure {

namespace {

constexpr const char* kFile = "load_order.txt";

std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
	return s;
}

} // namespace

LoadOrder parse_load_order(std::string_view text)
{
	LoadOrder out;
	if (text.size() > kLoadOrderMaxBytes) {
		out.issues.push_back({Severity::Error, kFile, "load_order.txt is larger than 64 KiB, so no mod is on."});
		return out;
	}
	if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3); // UTF-8 byte order mark
	size_t lineNo = 0;
	size_t pos    = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string_view::npos) nl = text.size();
		const std::string_view line = trim(text.substr(pos, nl - pos));
		pos = nl + 1;
		if (++lineNo > kLoadOrderMaxLines) {
			out.issues.push_back({Severity::Warning, kFile, "load_order.txt has more than 1024 lines; the rest is ignored."});
			break;
		}
		if (line.empty() || line.front() == '#') continue;
		const bool enabled = line.front() != '-';
		const std::string_view id = enabled ? line : trim(line.substr(1));
		if (!valid_id(id)) {
			out.issues.push_back(
			    {Severity::Warning, kFile, "Line " + std::to_string(lineNo) + " of load_order.txt is not a mod id."});
			continue;
		}
		bool dup = false;
		for (const OrderEntry& e : out.entries) dup = dup || e.id == id;
		if (dup) {
			out.issues.push_back({Severity::Warning, kFile,
			                      "Line " + std::to_string(lineNo) + " of load_order.txt repeats " + std::string(id) +
			                          "; the first line counts."});
			continue;
		}
		out.entries.push_back({std::string(id), enabled});
	}
	return out;
}

std::string format_load_order(const std::vector<OrderEntry>& order)
{
	std::string s = "# Moonsprout mod load order: top = lowest priority, bottom = highest. \"-\" turns a mod off.\n";
	for (const OrderEntry& e : order) {
		if (!e.enabled) s += '-';
		s += e.id;
		s += '\n';
	}
	return s;
}

} // namespace mppm::mods::pure
