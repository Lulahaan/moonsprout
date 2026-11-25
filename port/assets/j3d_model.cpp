// J3D model files (.bmd .bdl) and material tables (.bmt), converted to host byte order in place
// when they are loaded.
//
// Converted: every block header and the tables the loaders and factories read (hierarchy, vertex
// formats, envelopes, draw matrices, joints, shapes, materials). Left in GameCube byte order:
// vertex arrays and display lists (Aurora reads them big-endian), ResTIMG headers (BE() fields)
// and name tables (ResNTAB, BE() fields). The file header stays big-endian. Swapping the first
// block's type marks the file as converted.
#pragma clang diagnostic ignored "-Wregister" // JSystem headers
#include "JSystem/J3D/J3DFileBlock.h"
#include "JSystem/J3D/J3DJointFactory.h"
#include "JSystem/J3D/J3DMaterialFactory.h"
#include "JSystem/J3D/J3DShape.h"
#include "JSystem/J3D/J3DShapeFactory.h"
#include "JSystem/J3D/J3DTexMtx.h"
#include "JSystem/J3D/J3DPE.h"
#include "p2_endian.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Block {
	u8* base;
	u32 size;

	u16 u16At(u32 off) const { return p2_read_be<u16>(base + off); }
	u32 u32At(u32 off) const { return p2_read_be<u32>(base + off); }
	u8* at(u32 off) const { return base + off; }

	void swapHeader(u32 from16, u32 to16, u32 from32, u32 to32) const
	{
		for (u32 o = from16; o < to16; o += 2) {
			p2_swap(*reinterpret_cast<u16*>(base + o));
		}
		for (u32 o = from32; o < to32; o += 4) {
			p2_swap(*reinterpret_cast<u32*>(base + o));
		}
	}
};

template <typename T>
void swapAt(u8* p, u32 count)
{
	p2_swap_array(reinterpret_cast<T*>(p), count);
}

template <typename T>
void swapField(u8* p)
{
	p2_swap(*reinterpret_cast<T*>(p));
}

// u16 values in [from, to) of a record.
void swap16Range(u8* rec, u32 from, u32 to)
{
	swapAt<u16>(rec + from, (to - from) / 2);
}

void convertInfo(const Block& b)
{
	u32 hier = b.u32At(0x14);
	if (hier != 0) {
		for (u8* node = b.at(hier); node + 4 <= b.base + b.size; node += 4) {
			u16 type = p2_read_be<u16>(node);
			swapAt<u16>(node, 2);
			if (type == 0) {
				break;
			}
		}
	}
	b.swapHeader(0x08, 0x0A, 0x0C, 0x18);
}

void convertVertex(const Block& b)
{
	u32 fmt = b.u32At(0x08);
	if (fmt != 0) {
		for (u8* e = b.at(fmt); e + 16 <= b.base + b.size; e += 16) {
			u32 attr = p2_read_be<u32>(e);
			swapAt<u32>(e, 3);
			if (attr == 0xFF) { // GX_VA_NULL
				break;
			}
		}
	}
	b.swapHeader(0, 0, 0x08, 0x40);
}

void convertEnvelope(const Block& b)
{
	u32 count   = b.u16At(0x08);
	u32 mixCnt  = b.u32At(0x0C);
	u32 mixIdx  = b.u32At(0x10);
	u32 weights = b.u32At(0x14);
	u32 invBind = b.u32At(0x18);
	u32 total   = 0;
	if (mixCnt != 0) {
		for (u32 i = 0; i < count; i++) {
			total += b.base[mixCnt + i];
		}
	}
	u32 joints = 0;
	if (mixIdx != 0) {
		for (u32 i = 0; i < total; i++) {
			joints = std::max<u32>(joints, p2_read_be<u16>(b.at(mixIdx + i * 2)) + 1u);
		}
		swapAt<u16>(b.at(mixIdx), total);
	}
	if (weights != 0) {
		swapAt<f32>(b.at(weights), total);
	}
	if (invBind != 0) {
		swapAt<f32>(b.at(invBind), joints * 12);
	}
	b.swapHeader(0x08, 0x0A, 0x0C, 0x1C);
}

void convertDraw(const Block& b)
{
	u32 count = b.u16At(0x08);
	u32 idx   = b.u32At(0x10);
	if (idx != 0) {
		swapAt<u16>(b.at(idx), count);
	}
	b.swapHeader(0x08, 0x0A, 0x0C, 0x14);
}

void convertJoint(const Block& b)
{
	u32 count = b.u16At(0x08);
	u32 init  = b.u32At(0x0C);
	u32 remap = b.u32At(0x10);
	u32 n     = 0;
	if (remap != 0) {
		for (u32 i = 0; i < count; i++) {
			n = std::max<u32>(n, p2_read_be<u16>(b.at(remap + i * 2)) + 1u);
		}
		swapAt<u16>(b.at(remap), count);
	}
	for (u32 i = 0; i < n && init != 0; i++) {
		u8* rec = b.at(init + i * sizeof(J3DJointInitData));
		swapField<u16>(rec + 0x00);
		swapAt<f32>(rec + 0x04, 3);  // scale
		swapAt<s16>(rec + 0x10, 3);  // rotation
		swapAt<f32>(rec + 0x18, 3);  // translation
		swapAt<f32>(rec + 0x24, 7);  // radius, min, max
	}
	b.swapHeader(0x08, 0x0A, 0x0C, 0x18);
}

void convertShape(const Block& b)
{
	u32 count   = b.u16At(0x08);
	u32 init    = b.u32At(0x0C);
	u32 remap   = b.u32At(0x10);
	u32 attribs = b.u32At(0x18);
	u32 mtxTab  = b.u32At(0x1C);
	u32 mtxInit = b.u32At(0x24);
	u32 drwInit = b.u32At(0x28);

	u32 n = 0;
	for (u32 i = 0; i < count; i++) {
		n = std::max<u32>(n, p2_read_be<u16>(b.at(remap + i * 2)) + 1u);
	}
	swapAt<u16>(b.at(remap), count);

	std::vector<u32> descLists;
	u32 groups = 0;
	for (u32 i = 0; i < n; i++) {
		u8* rec       = b.at(init + i * sizeof(J3DShapeInitData));
		u32 groupNum  = p2_read_be<u16>(rec + 0x02);
		u32 descList  = p2_read_be<u16>(rec + 0x04);
		u32 mtxIndex  = p2_read_be<u16>(rec + 0x06);
		u32 drawIndex = p2_read_be<u16>(rec + 0x08);
		groups        = std::max(groups, std::max(mtxIndex, drawIndex) + groupNum);
		if (std::find(descLists.begin(), descLists.end(), descList) == descLists.end()) {
			descLists.push_back(descList);
		}
		swap16Range(rec, 0x02, 0x0A);
		swapAt<f32>(rec + 0x0C, 7);
	}

	for (u32 list : descLists) { // GXVtxDescList {attr, type}, ends at GX_VA_NULL
		for (u8* e = b.at(attribs + list); e + 8 <= b.base + b.size; e += 8) {
			u32 attr = p2_read_be<u32>(e);
			swapAt<u32>(e, 2);
			if (attr == 0xFF) {
				break;
			}
		}
	}

	u32 mtxEntries = 0;
	for (u32 i = 0; i < groups; i++) {
		u8* rec    = b.at(mtxInit + i * sizeof(J3DShapeMtxInitData));
		u32 cnt    = p2_read_be<u16>(rec + 0x02);
		u32 first  = p2_read_be<u32>(rec + 0x04);
		mtxEntries = std::max(mtxEntries, first + cnt);
		swapAt<u16>(rec, 2);
		swapField<u32>(rec + 0x04);

		u8* drw = b.at(drwInit + i * sizeof(J3DShapeDrawInitData));
		swapAt<u32>(drw, 2);
	}
	if (mtxTab != 0) {
		swapAt<u16>(b.at(mtxTab), mtxEntries);
	}
	b.swapHeader(0x08, 0x0A, 0x0C, 0x2C);
}

void convertTexture(const Block& b)
{
	b.swapHeader(0x08, 0x0A, 0x0C, 0x14); // ResTIMG headers and names stay big-endian
}

// MAT3 (v26) and MAT2 (v21) share their table formats; only the header layout differs.
struct MatLayout {
	u32 headerEnd;
	// Offsets (within the block header) of each table's offset field; 0 = not present.
	u32 entry, remap, names, ind, cull, matColor, numChans, chanInfo, ambColor, light, numTexGens, texGen, texGen2,
	    texMtx, texMtx2, texNo, tevOrder, tevColor, tevKColor, numTev, tevStage, swapMode, swapTable, fog, alphaComp, blend,
	    zMode, zComp, dither, nbtScale;
};

constexpr MatLayout kMat3 = { 0x84, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40,
	                          0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80 };
constexpr MatLayout kMat2 = { 0x78, 0x0C, 0x10, 0,    0x14, 0x18, 0x1C, 0x20, 0x24, 0,    0,    0x28, 0x2C, 0x30, 0x34,
	                          0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74 };

void convertMaterial(const Block& b, const MatLayout& L, u32 entrySize)
{
	auto tableOf = [&](u32 field) -> u32 { return field ? b.u32At(field) : 0; };
	// A table ends where the next one starts. Empty tables share their offset with the table after
	// them, so a later field at the same offset also ends this one.
	auto region = [&](u32 field) -> u32 {
		u32 off = tableOf(field);
		if (off == 0) {
			return 0;
		}
		u32 end = b.size;
		for (u32 o = 0x0C; o < L.headerEnd; o += 4) {
			u32 v = b.u32At(o);
			if (v != 0 && (v > off || (v == off && o > field)) && v < end) {
				end = v;
			}
		}
		return end - off;
	};

	u32 count = b.u16At(0x08);
	if (u32 remap = tableOf(L.remap)) {
		swapAt<u16>(b.at(remap), count);
	}

	if (u32 entries = tableOf(L.entry)) {
		u32 n = region(L.entry) / entrySize;
		for (u32 i = 0; i < n; i++) {
			u8* rec = b.at(entries + i * entrySize);
			if (entrySize == sizeof(J3DMaterialInitData)) {
				swap16Range(rec, 0x08, 0x9C);
				swap16Range(rec, 0xBC, entrySize);
			} else {
				swap16Range(rec, 0x08, offsetof(J3DMaterialInitData_v21, mTevKColorSels));
				swap16Range(rec, offsetof(J3DMaterialInitData_v21, mTevOrderInfoIndex), entrySize);
			}
		}
	}

	if (u32 ind = tableOf(L.ind)) {
		u32 n = region(L.ind) / sizeof(J3DIndInitData);
		for (u32 i = 0; i < n; i++) {
			u8* rec = b.at(ind + i * sizeof(J3DIndInitData));
			for (u32 m = 0; m < 3; m++) {
				swapAt<f32>(rec + 0x14 + m * 0x1C, 6);
			}
		}
	}
	if (u32 cull = tableOf(L.cull)) {
		swapAt<u32>(b.at(cull), region(L.cull) / 4);
	}
	auto texMtx = [&](u32 field) {
		if (u32 t = tableOf(field)) {
			u32 n = region(field) / sizeof(J3DTexMtxInfo);
			for (u32 i = 0; i < n; i++) {
				u8* rec = b.at(t + i * sizeof(J3DTexMtxInfo));
				swapField<u16>(rec + 0x02);
				swapAt<f32>(rec + 0x04, 5);      // center, SRT scale
				swapField<s16>(rec + 0x18);      // SRT rotation
				swapAt<f32>(rec + 0x1C, 2 + 16); // SRT translation, effect matrix
			}
		}
	};
	texMtx(L.texMtx);
	texMtx(L.texMtx2);
	if (u32 texNo = tableOf(L.texNo)) {
		swapAt<u16>(b.at(texNo), region(L.texNo) / 2);
	}
	if (u32 tevColor = tableOf(L.tevColor)) {
		swapAt<s16>(b.at(tevColor), region(L.tevColor) / 2);
	}
	if (u32 fog = tableOf(L.fog)) {
		u32 n = region(L.fog) / sizeof(J3DFogInfo);
		for (u32 i = 0; i < n; i++) {
			u8* rec = b.at(fog + i * sizeof(J3DFogInfo));
			swapField<u16>(rec + 0x02);
			swapAt<f32>(rec + 0x04, 4);
			swapAt<u16>(rec + 0x18, 10);
		}
	}
	if (u32 nbt = tableOf(L.nbtScale)) {
		u32 n = region(L.nbtScale) / 0x10;
		for (u32 i = 0; i < n; i++) {
			swapAt<f32>(b.at(nbt + i * 0x10 + 4), 3);
		}
	}
	// Everything else (colors, counts, texgen, TEV order/stage/swap, alpha, blend, z, dither) is bytes.
	b.swapHeader(0x08, 0x0A, 0x0C, L.headerEnd);
}

} // namespace

void p2_hostJ3DModel(const void* data)
{
	if (data == nullptr) {
		return;
	}
	auto* header = static_cast<const J3DFileHeader*>(data);
	u32 version  = header->mJ3dVersion;
	if (version != 'J3D1' && version != 'J3D2') {
		return;
	}
	u8* cursor  = const_cast<u8*>(reinterpret_cast<const u8*>(header + 1));
	u32 nblocks = header->mBlockCount;
	for (u32 i = 0; i < nblocks; i++) {
		char tag[5] = {};
		std::memcpy(tag, cursor, 4);
		u32 size = p2_read_be<u32>(cursor + 4);
		if (i == 0 && !(tag[3] >= '0' && tag[3] <= '9')) {
			return; // already converted
		}
		Block b{ cursor, size };
		if (!std::strcmp(tag, "INF1")) {
			convertInfo(b);
		} else if (!std::strcmp(tag, "VTX1")) {
			convertVertex(b);
		} else if (!std::strcmp(tag, "EVP1")) {
			convertEnvelope(b);
		} else if (!std::strcmp(tag, "DRW1")) {
			convertDraw(b);
		} else if (!std::strcmp(tag, "JNT1")) {
			convertJoint(b);
		} else if (!std::strcmp(tag, "SHP1")) {
			convertShape(b);
		} else if (!std::strcmp(tag, "TEX1")) {
			convertTexture(b);
		} else if (!std::strcmp(tag, "MAT3")) {
			convertMaterial(b, kMat3, sizeof(J3DMaterialInitData));
		} else if (!std::strcmp(tag, "MAT2")) {
			convertMaterial(b, kMat2, sizeof(J3DMaterialInitData_v21));
		} else {
			std::fprintf(stderr, "mppm: J3D model block %s is not converted for the host yet\n", tag);
		}
		b.swapHeader(0, 0, 0, 8);
		cursor += size;
	}
}
