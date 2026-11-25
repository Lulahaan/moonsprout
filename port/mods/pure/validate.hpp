#pragma once
// Data-mod overlay (M5.2): disc-independent content validators (design 4.6). Pure: no Aurora, SDL
// or p2_net.

#include "p2_mods.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mppm::mods::pure {

// Stage list shape: the first token (after # comments are removed) and the number of top-level
// "{ ... }" blocks (braces as whole tokens).
struct StageCount {
	bool hasNumber = false; // the first token is a non-negative integer
	long declared  = 0;
	long blocks    = 0;
};
StageCount count_stages(std::string_view text);

// True when "{" / "}" tokens balance after # comments are removed.
bool braces_balanced(std::string_view text);

// Runs every validator of design 4.6 that applies to `key` and appends its issues (path = `entry`,
// the manifest entry).
void validate_file(const std::string& key, const std::string& entry, const uint8_t* data, size_t size,
                   std::vector<Issue>& issues);

} // namespace mppm::mods::pure
