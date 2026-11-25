// Data-mod overlay (M5.2): see overlay.hpp. The callbacks run on any thread: every handle has its
// own cursor and the blobs never change after the mount.
#include "overlay.hpp"

#include <aurora/dvd.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace mppm::mods::overlay {

namespace {

struct Cursor {
	const Blob* blob;
	int64_t pos;
};

void* cb_open(void* userData)
{
	if (userData == nullptr) return nullptr;
	return new (std::nothrow) Cursor{static_cast<const Blob*>(userData), 0};
}

void cb_close(void* handle)
{
	delete static_cast<Cursor*>(handle);
}

int64_t cb_read(void* handle, uint8_t* buf, size_t len)
{
	Cursor* c = static_cast<Cursor*>(handle);
	if (c == nullptr || buf == nullptr) return -1;
	const int64_t size = (int64_t)c->blob->size;
	if (c->pos >= size) return 0;
	const uint64_t left = (uint64_t)(size - c->pos);
	const uint64_t n    = len < left ? len : left;
	std::memcpy(buf, c->blob->data + c->pos, (size_t)n);
	c->pos += (int64_t)n;
	return (int64_t)n;
}

// nod's convention (nod.h): whence 0 = from the start, 1 = from the current position, 2 = from the end.
int64_t cb_seek(void* handle, int64_t offset, int32_t whence)
{
	Cursor* c = static_cast<Cursor*>(handle);
	if (c == nullptr) return -1;
	const int64_t size = (int64_t)c->blob->size;
	int64_t base;
	switch (whence) {
	case 0: base = 0; break;
	case 1: base = c->pos; break;
	case 2: base = size; break;
	default: return -1;
	}
	if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && base < -offset)) return -1;
	const int64_t pos = base + offset;
	if (pos < 0 || pos > size) return -1;
	c->pos = pos;
	return pos;
}

const AuroraOverlayCallbacks kCallbacks = {cb_open, cb_close, cb_read, cb_seek};
bool sInstalled = false;

} // namespace

uint8_t* alloc_blob(uint64_t size, Blob* out)
{
	const uint64_t bytes = (size + 32 + 31) & ~(uint64_t)31;
	uint8_t* p = static_cast<uint8_t*>(std::aligned_alloc(32, (size_t)bytes));
	if (p == nullptr) return nullptr;
	std::memset(p + size, 0, (size_t)(bytes - size));
	out->data = p;
	out->size = size;
	return p;
}

void install_callbacks()
{
	if (sInstalled) return;
	sInstalled = true;
	aurora_dvd_overlay_callbacks(&kCallbacks);
}

} // namespace mppm::mods::overlay
