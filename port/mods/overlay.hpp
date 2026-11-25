#pragma once
// Data-mod overlay (M5.2): Aurora's overlay callbacks served from port-owned memory (design 3).

#include <cstdint>

namespace mppm::mods::overlay {

// A file's bytes, read at mount: 32-byte aligned, `size` bytes followed by 32 zero bytes. Never freed.
struct Blob {
	const uint8_t* data = nullptr;
	uint64_t size       = 0;
};

// Allocates a blob for `size` bytes (data writable through the returned pointer); nullptr when out of memory.
uint8_t* alloc_blob(uint64_t size, Blob* out);

// aurora_dvd_overlay_callbacks with the callbacks below; once per process (later calls do nothing).
void install_callbacks();

} // namespace mppm::mods::overlay
