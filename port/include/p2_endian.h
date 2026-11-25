// Byte-order helpers for converting the GameCube's big-endian resource data in place at load
// time. Each format's converter swaps its header and tables once; bulk payloads that Aurora
// consumes in GameCube layout (GX texture and display-list data) are left untouched.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

template <typename T>
inline T p2_bswap(T v)
{
	static_assert(std::is_trivially_copyable_v<T>);
	if constexpr (sizeof(T) == 1) {
		return v;
	} else if constexpr (sizeof(T) == 2) {
		uint16_t u;
		std::memcpy(&u, &v, 2);
		u = __builtin_bswap16(u);
		std::memcpy(&v, &u, 2);
		return v;
	} else if constexpr (sizeof(T) == 4) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		u = __builtin_bswap32(u);
		std::memcpy(&v, &u, 4);
		return v;
	} else {
		static_assert(sizeof(T) == 8);
		uint64_t u;
		std::memcpy(&u, &v, 8);
		u = __builtin_bswap64(u);
		std::memcpy(&v, &u, 8);
		return v;
	}
}

// Swap a field in place.
template <typename T>
inline void p2_swap(T& v)
{
	v = p2_bswap(v);
}

// Swap `count` consecutive values in place.
template <typename T>
inline void p2_swap_array(T* v, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		p2_swap(v[i]);
	}
}

// Read a big-endian value from unaligned memory without modifying it.
template <typename T>
inline T p2_read_be(const void* p)
{
	T v;
	std::memcpy(&v, p, sizeof(T));
	return p2_bswap(v);
}

// True if the four bytes at `p` spell `tag` in file (big-endian) order, i.e. not yet converted.
inline bool p2_has_be_tag(const void* p, const char (&tag)[5])
{
	return std::memcmp(p, tag, 4) == 0;
}
