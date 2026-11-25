// Audio interface, DSP, ARAM base, GBA link and remaining GX entry points Aurora does not provide.
// TODO(M2/M3): AI/DSP are replaced by the software mixer behind JASDsp (port/audio).
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dsp.h>
#include <dolphin/gba.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <cstring>

extern "C" {

// AI (audio DMA) and the DSP mailbox live in port/audio/audio.cpp.

// ---- ARAM
u32 ARGetBaseAddress(void) { return 0x4000; } // the SDK reserves the first 16 KiB

// ---- GBA link cable (the e-Reader card feature): no GBA attached.
void GBAInit(void) { }
s32 GBAGetStatus(s32, u8* status)
{
	if (status) *status = 0;
	return 1; // GBA_NOT_READY
}
s32 GBAReset(s32 chan, u8* status) { return GBAGetStatus(chan, status); }
s32 GBARead(s32 chan, u8*, u8* status) { return GBAGetStatus(chan, status); }
s32 GBAWrite(s32 chan, u8*, u8* status) { return GBAGetStatus(chan, status); }

// ---- GX entry points Aurora omits
static GXDrawSyncCallback s_drawSyncCb = nullptr;
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb)
{
	GXDrawSyncCallback old = s_drawSyncCb;
	s_drawSyncCb           = cb;
	return old;
}
void GXAbortFrame(void) { }
void GXSetMisc(GXMiscToken, u32) { }
void GXSetCopyClamp(GXFBClamp) { }
void GXReadXfRasMetric(u32* a, u32* b, u32* c, u32* d)
{
	*a = *b = *c = *d = 0;
}
void GXInitTexCacheRegion(GXTexRegion* region, GXBool, u32, GXTexCacheSize, u32, GXTexCacheSize)
{
	std::memset(region, 0, sizeof(*region));
}
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { return (f32)xfbHeight / (f32)efbHeight; }
u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) { return (u16)(efbHeight * yScale); }

// ---- MTX: declared by Aurora's mtx44ext.h but not implemented
void C_MTX44Copy(const Mtx44 src, Mtx44 dst)
{
	if (src != dst) {
		std::memcpy(dst, src, sizeof(Mtx44));
	}
}

} // extern "C"
