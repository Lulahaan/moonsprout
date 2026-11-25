#ifndef _MSL_COMMON_ARITH_H
#define _MSL_COMMON_ARITH_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus

#if TARGET_PC
#ifdef __cplusplus
}
#endif
#include <stdlib.h> // div_t, abs, labs, div
#ifdef __cplusplus
extern "C" {
#endif
#else
typedef struct {
	int quot; /* quotient */
	int rem;  /* remainder */
} div_t;

int abs(int __x);
s32 labs(s32 __x);
div_t div(s32 __numer, s32 __denom);
#endif

inline int _abs(int __x)
{
	return __x > 0 ? __x : -__x;
}

#ifdef __cplusplus
};
#endif // ifdef __cplusplus

#endif
