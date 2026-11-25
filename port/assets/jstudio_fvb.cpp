// JStudio function-value files (.fvb, embedded in cutscene data).
//
// The function value objects keep pointers into the file's paragraph payloads and read the key
// tables (f32/u32) directly every frame, so the payloads are converted to host byte order once,
// in place. Everything else stays big-endian: the header, block headers and paragraph headers are
// read through BE() fields and BE_READ (fvb-data.h, JGadget::binary).
//
// Paragraph payloads (see JStudio::fvb::TObject::prepare / prepare_data_):
//   0x01 data         32-bit words (composite control, constant, transition, list, hermite)
//   0x10 refer names  u32 count, then { u32 size; char name[size]; pad to 4 } per entry
//   0x11 refer index  u32 count, u32 index[count]
//   0x12 range        f32 begin, f32 end
//   0x13 progress, 0x14 adjust, 0x16 interpolate: u32 enum
//   0x15 outside      u16 begin, u16 end
#include "types.h"
#include "p2_endian.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr u32 kConvertedMark = 0x50324843; // 'P2HC', stored in the header's unused word _08

u32 roundUp4(u32 v) { return (v + 3) & ~3u; }

void swapWords(u8* p, u32 size)
{
	for (u32 i = 0; i + 4 <= size; i += 4) {
		p2_swap(*reinterpret_cast<u32*>(p + i));
	}
}

void convertPayload(u32 type, u8* payload, u32 size)
{
	switch (type) {
	case 0x10: {
		u32 count = p2_read_be<u32>(payload);
		p2_swap(*reinterpret_cast<u32*>(payload));
		u8* entry = payload + 4;
		for (; count != 0 && entry + 4 <= payload + size; count--) {
			u32 len = p2_read_be<u32>(entry);
			p2_swap(*reinterpret_cast<u32*>(entry));
			entry += 4 + roundUp4(len);
		}
		break;
	}
	case 0x15:
		p2_swap(*reinterpret_cast<u16*>(payload));
		p2_swap(*reinterpret_cast<u16*>(payload + 2));
		break;
	case 0x01:
	case 0x11:
	case 0x12:
	case 0x13:
	case 0x14:
	case 0x16:
		swapWords(payload, size);
		break;
	default:
		std::fprintf(stderr, "mppm: FVB paragraph type 0x%x is not converted for the host\n", type);
		break;
	}
}

} // namespace

void p2_hostFVB(const void* data)
{
	if (data == nullptr) {
		return;
	}
	u8* file = const_cast<u8*>(static_cast<const u8*>(data));
	if (std::memcmp(file, "FVB", 4) != 0 || p2_read_be<u16>(file + 4) != 0xFEFF) {
		return;
	}
	if (p2_read_be<u32>(file + 8) == kConvertedMark) {
		return;
	}

	u32 blockCount = p2_read_be<u32>(file + 0xC);
	u8* block      = file + 0x10;
	for (u32 b = 0; b < blockCount; b++) {
		u32 blockSize = p2_read_be<u32>(block);
		u16 idSize    = p2_read_be<u16>(block + 6);
		u8* para      = block + 8 + roundUp4(idSize);
		u8* blockEnd  = block + blockSize;
		while (para < blockEnd) {
			u32 head = p2_read_be<u16>(para);
			u32 size, type;
			u8* payload;
			if ((head & 0x8000) == 0) {
				size    = head;
				type    = p2_read_be<u16>(para + 2);
				payload = para + 4;
			} else {
				size    = ((head << 16) & 0x7FFF0000) | p2_read_be<u16>(para + 2);
				type    = p2_read_be<u32>(para + 4);
				payload = para + 8;
			}
			if (type == 0) {
				break;
			}
			if (size != 0) {
				convertPayload(type, payload, size);
			}
			para = payload + roundUp4(size);
		}
		block = blockEnd;
	}

	u32 mark = kConvertedMark;
	p2_swap(mark);
	std::memcpy(file + 8, &mark, 4); // stored big-endian like the rest of the header
}
