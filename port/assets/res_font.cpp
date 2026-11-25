// BFN (ResFONT) conversion: swap the header and the INF1/WID1/GLY1/MAP1 block fields to host
// order. Glyph sheets (GLY1 payload) are GX textures and stay in GameCube layout for Aurora.
#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JUtility/JUTResFONT_Ascfont_fix12.h"
#include "p2_endian.h"

#include <cstdlib>

static void swapResFont(ResFONT* font)
{
	p2_swap(font->mMagic);
	p2_swap(font->mFileSize);
	p2_swap(font->mNumBlocks);

	u8* cursor = font->mData;
	for (u32 i = 0; i < font->mNumBlocks; i++) {
		BlockHeader* block = reinterpret_cast<BlockHeader*>(cursor);
		p2_swap(block->mMagic);
		p2_swap(block->mSize);
		switch (block->mMagic) {
		case 'INF1': {
			auto* inf = static_cast<ResFONT::InfoBlock*>(block);
			p2_swap(inf->mFontType);
			p2_swap(inf->mAscent);
			p2_swap(inf->mDescent);
			p2_swap(inf->mWidth);
			p2_swap(inf->mLeading);
			p2_swap(inf->mDefaultCode);
			break;
		}
		case 'WID1': {
			// Width entries are byte pairs; only the header needs swapping.
			auto* wid = static_cast<ResFONT::WidthBlock*>(block);
			p2_swap(wid->mStartCode);
			p2_swap(wid->mEndCode);
			break;
		}
		case 'GLY1': {
			auto* gly = static_cast<ResFONT::GlyphBlock*>(block);
			p2_swap(gly->mStartCode);
			p2_swap(gly->mEndCode);
			p2_swap(gly->mCellWidth);
			p2_swap(gly->mCellHeight);
			p2_swap(gly->mTextureSize);
			p2_swap(gly->mTextureFormat);
			p2_swap(gly->mNumRows);
			p2_swap(gly->mNumColumns);
			p2_swap(gly->mTextureWidth);
			p2_swap(gly->mTextureHeight);
			break;
		}
		case 'MAP1': {
			auto* map = static_cast<ResFONT::MapBlock*>(block);
			p2_swap(map->mMappingMethod);
			p2_swap(map->mStartCode);
			p2_swap(map->mEndCode);
			p2_swap(map->mNumEntries);
			// The u16 code table starts at mLeading (offset 0x10): one entry per code for indexed
			// maps, (code, glyph) pairs for binary search, and an optional single lead entry for
			// SJIS conversion. Direct maps have no table.
			u16* table = &map->mLeading;
			u32 count  = 0;
			switch (map->mMappingMethod) {
			case ResFONT::MapBlock::MM_Indexed:
			case ResFONT::MapBlock::MM_SjisConversion:
				count = map->mNumEntries;
				break;
			case ResFONT::MapBlock::MM_BinarySearch:
				count = map->mNumEntries * 2u;
				break;
			default:
				break;
			}
			p2_swap_array(table, count);
			break;
		}
		default:
			break;
		}
		cursor += block->mSize;
	}
}

// The built-in system font is compiled in as a `const int[]` (read-only, and each word is in host
// order), so convert a copy of it once.
static const ResFONT* hostSystemFont()
{
	static ResFONT* sCopy = nullptr;
	if (sCopy == nullptr) {
		const u32 size = (u32)JUTResFONT_Ascfont_fix12[2]; // mFileSize as a native int
		u32* words     = static_cast<u32*>(std::aligned_alloc(32, (size + 31) & ~31u));
		for (u32 i = 0; i < size / 4; i++) {
			words[i] = __builtin_bswap32((u32)JUTResFONT_Ascfont_fix12[i]);
		}
		sCopy = reinterpret_cast<ResFONT*>(words);
		swapResFont(sCopy);
	}
	return sCopy;
}

// Called by JUTResFont::protected_initiate. Converts the resource in place the first time it is
// seen (its magic still reads "FONT" in file order) and returns the pointer to use.
const ResFONT* p2_hostResFont(const ResFONT* font)
{
	if (font == nullptr) {
		return nullptr;
	}
	if (font == reinterpret_cast<const ResFONT*>(JUTResFONT_Ascfont_fix12)) {
		return hostSystemFont();
	}
	if (p2_has_be_tag(font, "FONT")) {
		swapResFont(const_cast<ResFONT*>(font));
	}
	return font;
}
