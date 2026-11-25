// J3D animation files (.bck .bca .bpk .bpa .brk .btk .btp .bva), used by both J3D and J2D.
//
// The animation runtime reads the key/value tables directly every frame, so each file is converted
// to host byte order once, in place, when it is loaded. The file header stays big-endian (it is
// read through BE() fields); every data block's type and size are swapped too, which marks the
// file as converted: a block type that still spells "ANK1" etc. in memory has not been converted.
// Name tables (ResNTAB) stay big-endian; JUTNameTab reads them through BE() fields.
#include "JSystem/J3D/J3DFileBlock.h"
#include "p2_endian.h"

#include <cstdio>
#include <cstring>

namespace {

struct Block {
	u8* base;

	u16 u16At(u32 off) const { return p2_read_be<u16>(base + off); }
	s32 s32At(u32 off) const { return p2_read_be<s32>(base + off); }

	void swap16(u32 off) const { p2_swap(*reinterpret_cast<u16*>(base + off)); }
	void swap32(u32 off) const { p2_swap(*reinterpret_cast<u32*>(base + off)); }

	// Swap `count` 2- or 4-byte elements of the table at the (still big-endian) offset field `field`.
	template <typename T>
	void table(u32 field, u32 count) const
	{
		s32 off = s32At(field);
		if (off == 0 || count == 0) {
			return;
		}
		p2_swap_array(reinterpret_cast<T*>(base + off), count);
	}

	// Swap only the leading `u16s` u16 values of each `stride`-byte record.
	void records16(u32 field, u32 count, u32 stride, u32 u16s) const
	{
		s32 off = s32At(field);
		if (off == 0) {
			return;
		}
		for (u32 i = 0; i < count; i++) {
			p2_swap_array(reinterpret_cast<u16*>(base + off + i * stride), u16s);
		}
	}

	// Header fields: u16s in [from, to), then s32s in [from32, to32).
	void header(u32 from16, u32 to16, u32 from32, u32 to32) const
	{
		for (u32 o = from16; o < to16; o += 2) {
			swap16(o);
		}
		for (u32 o = from32; o < to32; o += 4) {
			swap32(o);
		}
	}
};

// Transform key (ANK1) and full (ANF1): 0x0C joints, 0x0E/0x10/0x12 scale/rot/trans counts.
void convertTransform(const Block& b, u32 u16sPerJoint)
{
	u32 joints = b.u16At(0x0C);
	b.table<u16>(0x14, joints * u16sPerJoint);
	b.table<f32>(0x18, b.u16At(0x0E));
	b.table<s16>(0x1C, b.u16At(0x10));
	b.table<f32>(0x20, b.u16At(0x12));
	b.header(0x0A, 0x14, 0x14, 0x24);
}

// Material color key (PAK1) and full (PAF1).
void convertColor(const Block& b, bool key)
{
	u32 mats = b.u16At(0x0E);
	b.table<u16>(0x18, mats * 4 * (key ? 3 : 2));
	b.table<u16>(0x1C, mats);
	if (key) {
		b.table<s16>(0x24, b.u16At(0x10));
		b.table<s16>(0x28, b.u16At(0x12));
		b.table<s16>(0x2C, b.u16At(0x14));
		b.table<s16>(0x30, b.u16At(0x16));
	} // full color values are u8
	b.header(0x0C, 0x18, 0x18, 0x34);
}

// TEV register key (TRK1).
void convertTevReg(const Block& b)
{
	u32 cNum = b.u16At(0x0C);
	u32 kNum = b.u16At(0x0E);
	b.records16(0x20, cNum, 0x1C, 12);
	b.records16(0x24, kNum, 0x1C, 12);
	b.table<u16>(0x28, cNum);
	b.table<u16>(0x2C, kNum);
	for (u32 i = 0; i < 4; i++) {
		b.table<s16>(0x38 + i * 4, b.u16At(0x10 + i * 2));
		b.table<s16>(0x48 + i * 4, b.u16At(0x18 + i * 2));
	}
	b.header(0x0A, 0x20, 0x20, 0x58);
}

// Texture SRT key (TTK1), including the optional "post" tracks.
void convertTextureSRT(const Block& b)
{
	u32 tracks = b.u16At(0x0C);
	b.table<u16>(0x14, tracks * 9);
	b.table<u16>(0x18, tracks / 3);
	b.table<f32>(0x24, tracks);
	b.table<f32>(0x28, b.u16At(0x0E));
	b.table<s16>(0x2C, b.u16At(0x10));
	b.table<f32>(0x30, b.u16At(0x12));

	u32 post = b.u16At(0x34);
	b.table<u16>(0x3C, post * 9);
	b.table<u16>(0x40, post / 3);
	b.table<f32>(0x4C, post);
	b.table<f32>(0x50, b.u16At(0x36));
	b.table<s16>(0x54, b.u16At(0x38));
	b.table<f32>(0x58, b.u16At(0x3A));

	b.header(0x0A, 0x14, 0x14, 0x34);
	b.header(0x34, 0x3C, 0x3C, 0x60);
}

// Texture pattern full (TPT1).
void convertTexPattern(const Block& b)
{
	u32 mats = b.u16At(0x0C);
	b.records16(0x10, mats, 8, 2);
	b.table<u16>(0x14, b.u16At(0x0E));
	b.table<u16>(0x18, mats);
	b.header(0x0A, 0x10, 0x10, 0x20);
}

// Visibility full (VAF1): values are u8.
void convertVisibility(const Block& b)
{
	b.table<u16>(0x10, b.u16At(0x0C) * 2);
	b.header(0x0A, 0x10, 0x10, 0x18);
}

} // namespace

void p2_hostJ3DAnm(const void* data)
{
	if (data == nullptr) {
		return;
	}
	auto* header = static_cast<const J3DFileHeader*>(data);
	if (header->mJ3dVersion != 'J3D1') {
		return;
	}
	u8* cursor  = const_cast<u8*>(reinterpret_cast<const u8*>(header + 1));
	u32 nblocks = header->mBlockCount;
	for (u32 i = 0; i < nblocks; i++) {
		Block b{ cursor };
		char tag[5] = {};
		std::memcpy(tag, cursor, 4);
		u32 size = p2_read_be<u32>(cursor + 4);
		if (i == 0 && !(tag[3] >= '0' && tag[3] <= '9')) {
			return; // first block type is reversed: already converted
		}

		if (!std::strcmp(tag, "ANK1")) {
			convertTransform(b, 27);
		} else if (!std::strcmp(tag, "ANF1")) {
			convertTransform(b, 18);
		} else if (!std::strcmp(tag, "PAK1")) {
			convertColor(b, true);
		} else if (!std::strcmp(tag, "PAF1")) {
			convertColor(b, false);
		} else if (!std::strcmp(tag, "TRK1")) {
			convertTevReg(b);
		} else if (!std::strcmp(tag, "TTK1")) {
			convertTextureSRT(b);
		} else if (!std::strcmp(tag, "TPT1")) {
			convertTexPattern(b);
		} else if (!std::strcmp(tag, "VAF1")) {
			convertVisibility(b);
		} else {
			std::fprintf(stderr, "mppm: J3D animation block %s is not converted for the host yet\n", tag);
		}
		b.swap32(0);
		b.swap32(4);
		cursor += size;
	}
}
