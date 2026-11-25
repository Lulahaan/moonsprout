// RARC (JKRArchive) conversion.
//
// The header and the info block (directory table, file table, string table) are big-endian. The
// file table also has a 32-bit slot per entry that the game fills with a pointer to the loaded
// file; on 64-bit hosts SDIFileEntry is wider than the on-disc entry, so each archive gets a
// host-side copy of its file table with real pointer slots.
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "p2_endian.h"
#include "p2_mods.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// On-disc layout of a file entry (0x14 bytes).
struct DiscFileEntry {
	u16 mFileID;
	u16 mHash;
	u32 mFlag;
	u32 mDataOffset;
	u32 mSize;
	u32 mData;
};
static_assert(sizeof(DiscFileEntry) == 0x14);

// Marks an info block whose tables were already converted (an otherwise unused padding byte).
constexpr u8 kHostTablesMark = 0xA5;

// Data-mod overlay (M5.2): _1B[3], the padding byte next to the mark, holds the id of the archive
// override set found when the tables were converted (0 = none), so a re-mount of the same converted
// buffer finds it again. Written only while overrides are installed.
constexpr int kModSetByte = 3;

} // namespace

void JKRArchive::p2HostHeader(void* header)
{
	if (header == nullptr || !p2_has_be_tag(header, "RARC")) {
		return; // already converted (or not an archive header)
	}
	p2_swap_array(static_cast<u32*>(header), 8);
}

void JKRArchive::p2HostTables()
{
	if (mDataInfo == nullptr) {
		return;
	}
	SArcDataInfo* info = mDataInfo;
	DiscFileEntry* disc;
	const P2ModsArcSet* modSet = nullptr; // in-archive overrides of this archive (M5.2), if any
	if (info->_1B[4] != kHostTablesMark) {
		if (p2_mods_arc_any()) {
			modSet = p2_mods_arc_find(info, mEntryNum); // matched on the raw (disc byte order) info block
		}
		p2_swap(info->mNumDirEntries);
		p2_swap(info->mDirEntryOffset);
		p2_swap(info->mNumFileEntries);
		p2_swap(info->mFileEntryOffset);
		p2_swap(info->mStrTableLength);
		p2_swap(info->mStrTableOffset);
		p2_swap(info->mNextFreeFileID);
		info->_1B[4] = kHostTablesMark;
		if (p2_mods_arc_any()) {
			info->_1B[kModSetByte] = modSet ? static_cast<u8>(modSet->id) : 0;
		}

		u8* base     = reinterpret_cast<u8*>(info);
		mDirectories = reinterpret_cast<SDIDirEntry*>(base + info->mDirEntryOffset);
		mStrTable    = reinterpret_cast<const char*>(base + info->mStrTableOffset);
		for (u32 i = 0; i < info->mNumDirEntries; i++) {
			SDIDirEntry& dir = mDirectories[i];
			p2_swap(dir.mType);
			p2_swap(dir.mOffset);
			p2_swap(dir._08);
			p2_swap(dir.mNum);
			p2_swap(dir.mFirstIdx);
		}
		disc = reinterpret_cast<DiscFileEntry*>(base + info->mFileEntryOffset);
		for (u32 i = 0; i < info->mNumFileEntries; i++) {
			p2_swap(disc[i].mFileID);
			p2_swap(disc[i].mHash);
			p2_swap(disc[i].mFlag);
			p2_swap(disc[i].mDataOffset);
			p2_swap(disc[i].mSize);
		}
	} else {
		disc = reinterpret_cast<DiscFileEntry*>(reinterpret_cast<u8*>(info) + info->mFileEntryOffset);
		if (p2_mods_arc_any()) {
			modSet = p2_mods_arc_get(info->_1B[kModSetByte]);
		}
	}

	// Host file table, freed with the archive's heap.
	JKRHeap* heap = mHeap ? mHeap : JKRHeap::sSystemHeap;
	auto* host    = static_cast<SDIFileEntry*>(JKRAllocFromHeap(heap, sizeof(SDIFileEntry) * (info->mNumFileEntries + 1), 16));
	if (host == nullptr) {
		std::fprintf(stderr, "mppm: rarc: no memory for the file table (%u entries)\n", static_cast<unsigned>(info->mNumFileEntries));
		std::fflush(stderr);
		std::abort();
	}
	for (u32 i = 0; i < info->mNumFileEntries; i++) {
		host[i].mFileID     = disc[i].mFileID;
		host[i].mHash       = disc[i].mHash;
		host[i].mFlag       = disc[i].mFlag;
		host[i].mDataOffset = disc[i].mDataOffset;
		host[i].mSize       = disc[i].mSize;
		host[i].mData       = nullptr;
	}
	if (modSet != nullptr) {
		// Replace whole files of this archive (docs/ONLINE_MULTIPLAYER.md 7a, second step). fetchResource
		// returns a preset mData as is, and getResSize returns mSize.
		//
		// Invariants (nothing in src/ breaks them today):
		// - mDataOffset still points at the disc file. removeResource / detachResource /
		//   removeResourceAll clear mData, and a later fetchResource would read the disc file back
		//   with the mod's mSize. No game code calls them (only JKernel itself); if one ever does, keep
		//   the override pointer and restore it there.
		// - EMM_Mem hands every mount the same port-owned buffer instead of a fresh copy: a
		//   JKRMemArchive frees only its whole image, so a per-mount copy would leak on every mount
		//   to a heap that is not destroyed. In-place work on the resource therefore persists between
		//   mounts. The port's endian converters (j3d_model, j3d_anim, jpa, jstudio_fvb, res_font)
		//   mark what they converted (fonts: the big-endian tag) and skip it the next time; two threads mounting the same modded archive at
		//   the same moment would race on that check (a known hazard, never seen in a run).
		for (u32 k = 0; k < modSet->count; k++) {
			const P2ModsArcFile& f = modSet->files[k];
			if (f.index >= info->mNumFileEntries || ((host[f.index].mFlag >> 24) & 0x02) != 0) {
				continue; // never a folder
			}
			void* data = const_cast<u8*>(f.data); // EMM_Mem: the port-owned buffer, shared between mounts
			if (mMountMode != EMM_Mem) {
				// The Dvd/Aram/Comp destructors free mData to the archive's heap: give them a copy from it.
				data = JKRAllocFromHeap(heap, (f.size + 32 + 31) & ~31u, 32);
				if (data == nullptr) {
					p2_mods_arc_note(modSet, k, mMountMode, 0);
					continue;
				}
				std::memcpy(data, f.data, f.size + 32); // includes the zero padding
			}
			host[f.index].mData = data;
			host[f.index].mSize = f.size;
			host[f.index].mFlag &= ~((0x04u | 0x80u) << 24); // not compressed
			if (mMountMode == EMM_Comp) {
				host[f.index].mFlag &= ~(0x10u << 24); // the Comp destructor frees entries without 0x10
			}
			p2_mods_arc_note(modSet, k, mMountMode, 1);
		}
	}
	mFileEntries = host;
}
