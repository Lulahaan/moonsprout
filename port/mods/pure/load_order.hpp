#pragma once
// Data-mod overlay (M5.2): load_order.txt (design 4.3). Pure: no Aurora, SDL or p2_net.

#include "p2_mods.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mppm::mods::pure {

constexpr size_t kLoadOrderMaxBytes = 64 * 1024;
constexpr size_t kLoadOrderMaxLines = 1024;

struct LoadOrder {
	std::vector<OrderEntry> entries; // top = lowest priority; first occurrence of each id
	std::vector<Issue> issues;       // folder level, path "load_order.txt"
};

// Parses the text of load_order.txt. Over 64 KiB is an Error and gives no entries.
LoadOrder parse_load_order(std::string_view text);

// The file the launcher writes: the header comment, then "<id>" or "-<id>" per line.
std::string format_load_order(const std::vector<OrderEntry>& order);

} // namespace mppm::mods::pure
