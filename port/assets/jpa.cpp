// JPA particle resources (JPAC2-10 .jpc). The runtime reads the block structs straight from the file
// every frame, so the file is converted to host byte order once, in place. ResTIMG headers inside TEX1
// stay big-endian (BE() fields); GX image data stays in GameCube layout for Aurora.
// Marker: the version word at +4 is swapped, so a converted file reads '2-10' natively, which is what
// JPAResourceLoader checks; a second call sees "01-2" in memory order and returns.
//
// Block layouts (include/JSystem/JParticle/JPABlock.h, JPAShape.h; sizes checked against the disc):
//   BEM1 0x7C: u32/f32 0x08-0x67, s16/u16 0x68-0x77, u8 0x78
//   FLD1 0x44: u32/f32 0x08-0x3F, u8 0x40
//   KFA1: u8 header 0x08-0x0B, then 4 f32 per key
//   BSP1: u32 flags, s16 colour-key table offsets, f32 size x/y, u16 blend cfg, s16 anim frame max,
//         optional f32 tex-coord anim table at 0x34, colour key tables {s16 index; GXColor}
//   ESP1 0x60: u32/f32 0x08-0x27, s16 0x28-0x2B, f32 0x2C-0x5F
//   SSP1 0x48: u32/f32 0x08-0x33, GXColor 0x34/0x38, f32 0x3C, s16 0x40/0x42, u8 0x44/0x45, s16 0x46
//   ETX1 0x28: u32 flags, f32 ind tex mtx 0x0C-0x23, s8 0x24-0x26
//   TDB1: u16 texture ids
#include "types.h"
#include "p2_endian.h"

#include <cstring>

namespace {

inline void sw16(u8* b, u32 o) { p2_swap(*reinterpret_cast<u16*>(b + o)); }
inline void sw32(u8* b, u32 o) { p2_swap(*reinterpret_cast<u32*>(b + o)); }
inline void r16(u8* b, u32 from, u32 to)
{
	for (u32 o = from; o + 2 <= to; o += 2) {
		sw16(b, o);
	}
}
inline void r32(u8* b, u32 from, u32 to)
{
	for (u32 o = from; o + 4 <= to; o += 4) {
		sw32(b, o);
	}
}

void convertBlock(u8* b, u32 tag, u32 size)
{
	switch (tag) {
	case 'BEM1':
		r32(b, 0x08, 0x68);
		r16(b, 0x68, 0x78);
		break;
	case 'FLD1':
		r32(b, 0x08, 0x40);
		break;
	case 'KFA1': {
		const u32 end = 0x0C + b[0x09] * 0x10;
		r32(b, 0x0C, end < size ? end : size);
		break;
	}
	case 'BSP1': {
		sw32(b, 0x08);
		sw16(b, 0x0C);
		sw16(b, 0x0E);
		sw32(b, 0x10);
		sw32(b, 0x14);
		sw16(b, 0x18);
		sw16(b, 0x24);
		u32 flags;
		std::memcpy(&flags, b + 0x08, 4);
		if (flags & 0x01000000) {
			r32(b, 0x34, 0x34 + 0x28); // tex-coord animation table (10 f32)
		}
		s16 prm, env;
		std::memcpy(&prm, b + 0x0C, 2);
		std::memcpy(&env, b + 0x0E, 2);
		const u8 clr = b[0x21];
		if (clr & 0x02) {
			for (u32 i = 0; i < b[0x22]; i++) {
				sw16(b, prm + i * 6); // JPAClrAnmKeyData.index
			}
		}
		if ((clr & 0x08) && !((clr & 0x02) && env == prm)) {
			for (u32 i = 0; i < b[0x23]; i++) {
				sw16(b, env + i * 6);
			}
		}
		break;
	}
	case 'ESP1':
		r32(b, 0x08, 0x28);
		r16(b, 0x28, 0x2C);
		r32(b, 0x2C, 0x60);
		break;
	case 'SSP1':
		r32(b, 0x08, 0x34);
		sw32(b, 0x3C);
		sw16(b, 0x40);
		sw16(b, 0x42);
		sw16(b, 0x46);
		break;
	case 'ETX1':
		r32(b, 0x08, 0x24);
		break;
	case 'TDB1':
		r16(b, 0x08, size);
		break;
	default:
		break;
	}
}

} // namespace

void p2_hostJPC(const void* data)
{
	u8* p = const_cast<u8*>(static_cast<const u8*>(data));
	if (p == nullptr || std::memcmp(p, "JPAC", 4) != 0 || std::memcmp(p + 4, "2-10", 4) != 0) {
		return; // another format, or already converted
	}
	sw32(p, 0x04);
	sw16(p, 0x08);
	sw16(p, 0x0A);
	sw32(p, 0x0C);
	const u16 resNum = *reinterpret_cast<u16*>(p + 0x08);
	const u16 texNum = *reinterpret_cast<u16*>(p + 0x0A);
	u32 off          = 0x10;
	for (u32 r = 0; r < resNum; r++) {
		u8* h = p + off;
		sw16(h, 0); // usrIdx
		sw16(h, 2); // block count (bytes 4..7 are u8 counts)
		const u16 blocks = *reinterpret_cast<u16*>(h + 2);
		off += 8;
		for (u32 j = 0; j < blocks; j++) {
			u8* b          = p + off;
			const u32 tag  = p2_read_be<u32>(b);
			const u32 size = p2_read_be<u32>(b + 4);
			convertBlock(b, tag, size);
			sw32(b, 0); // the loader compares the tag natively against 'FLD1' etc.
			sw32(b, 4);
			off += size;
		}
	}
	u32 o = *reinterpret_cast<u32*>(p + 0x0C);
	for (u32 t = 0; t < texNum; t++) {
		u8* b          = p + o;
		const u32 size = p2_read_be<u32>(b + 4);
		sw32(b, 0); // TEX1 tag and size; the ResTIMG inside stays big-endian
		sw32(b, 4);
		o += size;
	}
}
