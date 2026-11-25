#pragma once
// SHA-256 (FIPS 180-4), written for the data-mod overlay (M5.2). Pure: no Aurora, SDL or p2_net.
// Checked against the NIST vectors in tests/mods/core_test.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mppm::mods::pure {

class Sha256 {
public:
	Sha256();
	void update(const void* data, size_t len);
	void update(std::string_view text) { update(text.data(), text.size()); }
	std::array<uint8_t, 32> finish(); // the object must not be used afterwards

	static std::array<uint8_t, 32> of(const void* data, size_t len);

private:
	void block(const uint8_t* p);
	uint32_t mState[8];
	uint8_t mBuf[64];
	size_t mBufLen  = 0;
	uint64_t mTotal = 0; // bytes
};

} // namespace mppm::mods::pure
