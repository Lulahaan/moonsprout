#if TARGET_PC
#ifndef _DOLPHIN_VEC_H
#define _DOLPHIN_VEC_H
#include "types.h"
#include "p2_sdk_compat.h"
#include <dolphin/mtx.h> // Vec, S16Vec, PSVEC*
typedef S16Vec SVec;
#endif
#else // TARGET_PC

#ifndef _DOLPHIN_VEC_H
#define _DOLPHIN_VEC_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus

typedef struct Vec {
	f32 x;
	f32 y;
	f32 z;
} Vec;

typedef struct SVec {
	s16 x;
	s16 y;
	s16 z;
} SVec;

void PSVECAdd(const Vec*, const Vec*, Vec*);
void PSVECSubtract(const Vec*, const Vec*, Vec*);
void PSVECNormalize(const Vec*, Vec*);
f32 PSVECMag(const Vec*);
void PSVECCrossProduct(const Vec*, const Vec*, Vec*);

#ifdef __cplusplus
}
#endif

#endif

#endif // TARGET_PC
