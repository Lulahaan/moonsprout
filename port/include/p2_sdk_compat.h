#ifndef _P2_SDK_COMPAT_H
#define _P2_SDK_COMPAT_H
// Host builds compile the game against Aurora's Dolphin SDK headers (so struct layouts match its
// implementation). The decomp's SDK headers pulled in each other more broadly and carry a few
// extra names; this header restores those on top of Aurora. Included by every include/Dolphin/*
// forwarder under TARGET_PC.

// The decomp's C library wrappers (include/stl) are on the -iquote path only, so Aurora's own
// <string.h>-style includes never re-enter types.h and including Aurora from here is safe.
#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/mtx.h>
#include <dolphin/mtx/mtx44ext.h>
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#include <dolphin/vi.h>
#include <dolphin/pad.h>
#include <dolphin/dvd.h>
#include <dolphin/card.h>
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <dolphin/dsp.h>
#include <dolphin/si.h>
#include <dolphin/base/PPCArch.h>
#include "p2_sdk_aliases.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- mtx.h
// Aurora maps PSMTX* onto its C implementation on non-GEKKO targets, except this one.
#ifndef PSMTX44Copy
#define PSMTX44Copy C_MTX44Copy
#endif
typedef f32 Mtx23[2][3];
typedef f32 Mtx33[3][3];
typedef f32 (*MtxP)[4];
typedef f32 (*Mtx3P)[3];
typedef const f32 (*CMtxP)[4];
typedef f32 PSQuaternion[4];

// ---- gd.h: the decomp's GDCurrentDL is Aurora's GDLObj (same layout, SDK field names).
typedef struct _GDLObj GDCurrentDL;
#define __GDCheckOverflowed(size) GDOverflowCheck(size)
#define __GDSetCurrent(obj)       GDSetCurrent(obj)

// ---- GXTypes.h
#define GX_BITFIELD(field, pos, size, value)       (__rlwimi((field), (value), 31 - (pos) - (size) + 1, (pos), (pos) + (size) - 1))
#define GX_BITFIELD_SET(field, pos, size, value)   ((field) = GX_BITFIELD(field, pos, size, value))
#define GX_BITFIELD_TRUNC(field, pos, size, value) (__rlwimi((field), (value), 0, (pos), (pos) + (size) - 1))
#define GX_BITGET(field, pos, size)                ((field) >> (31 - (pos) - (size) + 1) & ((1 << (size)) - 1))
// The original reinterprets the struct as a big-endian u32 (RGBA); build that value explicitly.
#define GXCOLOR_AS_U32(color) \
	(((u32)(color).r << 24) | ((u32)(color).g << 16) | ((u32)(color).b << 8) | (u32)(color).a)
#define RGBA_TO_U32(r, g, b, a) (((u8)(r) << 24) | ((u8)(g) << 16) | ((u8)(b) << 8) | ((u8)(a)))

// ---- GXFifo.h
#define GX_FIFO_OBJ_SIZE (sizeof(GXFifoObj))

// ---- GXFifo.h write-gather pipe -> Aurora's command stream
#define GX_WRITE_U8(val)  GXCmd1u8((u8)(val))
#define GX_WRITE_U16(val) GXCmd1u16((u16)(val))
#define GX_WRITE_U32(val) GXCmd1u32((u32)(val))
#define GX_WRITE_F32(val) GXParam1f32((f32)(val))

// ---- GXHardware.h register loads (write-gather pipe -> Aurora's command stream)
#define GX_BP_LOAD_REG(data)                     \
	do {                                         \
		GXCmd1u8(0x61); /* LOAD_BP_REG */        \
		GXParam1u32((u32)(data));                \
	} while (0)
#define GX_CP_LOAD_REG(addr, data)               \
	do {                                         \
		GXCmd1u8(0x08); /* LOAD_CP_REG */        \
		GXParam1u8((u8)(addr));                  \
		GXParam1u32((u32)(data));                \
	} while (0)
#define GX_XF_LOAD_REG_HDR(addr)                 \
	do {                                         \
		GXCmd1u8(0x10); /* LOAD_XF_REG */        \
		GXParam1u32((u32)(addr));                \
	} while (0)
#define GX_BP_SET_OPCODE(cmd, opcode) (cmd) = GX_BITFIELD_SET(cmd, 0, 8, (opcode))

// ---- mtx.h inlines the decomp adds
static inline void MTXSetPosition(Mtx mtx, const Vec* pos)
{
	mtx[0][3] = pos->x;
	mtx[1][3] = pos->y;
	mtx[2][3] = pos->z;
}

// ---- card.h (decomp names for the CARD_STAT banner/icon bits)
typedef enum { BannerColorCI8 = 0x1, BannerPresent = 0x2, IconAnimationPingPong = 0x4 } CARDBannerFlag;

// ---- SDK functions Aurora does not provide (port/sdk/gx_extras.cpp)
void GXSetDrawSync(u16 token);
void GXWaitDrawDone(void);
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const Mtx44 projMtx);
void __GXSetIndirectMask(u32 mask);

// Enum spellings that differ from Aurora come from p2_sdk_aliases.h (generated, value-based:
// several decomp names are guesses, but the matched values are right).

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// GameCube code writes GX commands straight to the write-gather pipe (GXWGFifo.u8 = ... at
// 0xCC008000). On the host those assignments go to Aurora's command FIFO (port/sdk/wgpipe.cpp).
extern "C" {
void p2_wgpipe_u8(unsigned char v);
void p2_wgpipe_u16(unsigned short v);
void p2_wgpipe_u32(unsigned int v);
void p2_wgpipe_f32(float v);
}
struct P2WGPipe {
	struct U8 {
		void operator=(unsigned char v) const { p2_wgpipe_u8(v); }
	};
	struct U16 {
		void operator=(unsigned short v) const { p2_wgpipe_u16(v); }
	};
	struct U32 {
		void operator=(unsigned int v) const { p2_wgpipe_u32(v); }
	};
	struct F32 {
		void operator=(float v) const { p2_wgpipe_f32(v); }
	};
	U8 u8, s8;
	U16 u16, s16;
	U32 u32, s32;
	F32 f32;
};
static const P2WGPipe p2_wgpipe = {};
#undef GXWGFifo
#define GXWGFifo p2_wgpipe
#endif

// Aurora's GXSetChanMatColor/GXSetChanAmbColor store the whole RGBA even when only the colour
// (GX_COLORn) or only the alpha (GX_ALPHAn) half of a channel is set; the SDK keeps the other half.
// J2D sets just the alpha before every picture, which turned all 2D material colours black.
#ifdef __cplusplus
extern "C" {
#endif
void p2_GXSetChanMatColor(GXChannelID id, GXColor color);
void p2_GXSetChanAmbColor(GXChannelID id, GXColor color);
#ifdef __cplusplus
}
#endif
#define GXSetChanMatColor p2_GXSetChanMatColor
#define GXSetChanAmbColor p2_GXSetChanAmbColor

// The SDK's DVD path parser restarts at the root when a path component starts with '/' (so
// "a/b//c/d" means "/c/d"); some game paths rely on that. Aurora collapses repeated slashes
// instead, so normalise first (port/sdk/dvd_fixes.cpp).
#ifdef __cplusplus
extern "C" {
#endif
s32 p2_DVDConvertPathToEntrynum(const char* path);
BOOL p2_DVDOpen(const char* path, DVDFileInfo* info);
BOOL p2_DVDOpenDir(const char* path, DVDDir* dir);
BOOL p2_DVDChangeDir(const char* path);
#ifdef __cplusplus
}
#endif
#define DVDConvertPathToEntrynum p2_DVDConvertPathToEntrynum
#define DVDOpen p2_DVDOpen
#define DVDOpenDir p2_DVDOpenDir
#define DVDChangeDir p2_DVDChangeDir

// include/Dolphin/gd.h defines this, but on a case-insensitive filesystem (Windows, macOS)
// "Dolphin/gd.h" finds Aurora's dolphin/gd.h first, so define it here too (same definition).
#ifndef GD_CURR_U8PTR
#define GD_CURR_U8PTR() ((u8*)GDGetCurrPointer())
#endif

#endif
