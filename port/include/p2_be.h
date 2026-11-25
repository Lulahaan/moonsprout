// Big-endian field type for host builds. Resource structs that are read straight out of
// GameCube files declare their multi-byte fields as BE(T): on the host the value stays in
// file byte order in memory and is swapped on every read and write, so the data needs no
// conversion pass. Under MWCC, BE(T) is plain T (see types.h).
#pragma once

#include "p2_endian.h"

namespace p2 {

template <typename T>
struct BigEndian {
	static_assert(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

	T mRaw; // file (big-endian) byte order


	operator T() const { return p2_bswap(mRaw); }
	T get() const { return p2_bswap(mRaw); }
	// Offsets stored in files are sometimes cast to pointers before being rebased.
	template <typename P>
	explicit operator P*() const
	{
		return (P*)(uintptr_t)get();
	}

	BigEndian& operator=(T v)
	{
		mRaw = p2_bswap(v);
		return *this;
	}
	BigEndian& operator+=(T v) { return *this = T(get() + v); }
	BigEndian& operator-=(T v) { return *this = T(get() - v); }
	BigEndian& operator|=(T v) { return *this = T(get() | v); }
	BigEndian& operator&=(T v) { return *this = T(get() & v); }
	BigEndian& operator^=(T v) { return *this = T(get() ^ v); }
	BigEndian& operator++() { return *this = T(get() + 1); }
	BigEndian& operator--() { return *this = T(get() - 1); }
	T operator++(int)
	{
		T old = get();
		*this = T(old + 1);
		return old;
	}
	T operator--(int)
	{
		T old = get();
		*this = T(old - 1);
		return old;
	}
};

} // namespace p2
