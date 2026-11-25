#ifndef _THP_THPVIDEODECODE_H
#define _THP_THPVIDEODECODE_H

#include "Dolphin/os.h"
#include "THP/THPRead.h"
#include "types.h"

static void* VideoDecoder(void*);
static void* VideoDecoderForOnMemory(void*);
static void VideoDecode(THPReadBuffer*);

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
BOOL CreateVideoDecodeThread(OSPriority priority, void* task);
void VideoDecodeThreadStart();
void VideoDecodeThreadCancel();
OSMessage PopFreeTextureSet();
BOOL PushFreeTextureSet(OSMessage*);
OSMessage PopDecodedTextureSet(s32 flags);
BOOL PushDecodedTextureSet(OSMessage*);

#if !TARGET_PC // defined static in its .cpp; MWCC accepts the redeclaration, Clang does not
extern BOOL VideoDecodeThreadCreated;
#endif
#if !TARGET_PC // defined static in its .cpp; MWCC accepts the redeclaration, Clang does not
extern OSMessageQueue FreeTextureSetQueue;
#endif
#if !TARGET_PC // defined static in its .cpp; MWCC accepts the redeclaration, Clang does not
extern OSMessageQueue DecodedTextureSetQueue;
#endif
#if !TARGET_PC // defined static in its .cpp; MWCC accepts the redeclaration, Clang does not
extern OSThread VideoDecodeThread;
#endif

#ifdef __cplusplus
};
#endif

#endif
