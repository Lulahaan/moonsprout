#ifndef _PORT_PPC_INTRINSICS_H
#define _PORT_PPC_INTRINSICS_H
// Portable C versions of the MWCC PowerPC intrinsics used by the decomp.
// Results are exact (not the hardware estimates) but deterministic across hosts.

#include <stdint.h>
// System libm first (the decomp's wrappers are -iquote only, so this is the host header). glibc
// declares its own __fabs/__fabsf, so ours are named p2_* and mapped by macro after that.
#ifdef __cplusplus
extern "C++" {
#endif
#include <math.h>
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline double p2_fabs(double x) { return __builtin_fabs(x); }
static inline float p2_fabsf(float x) { return __builtin_fabsf(x); }
#define __fabs  p2_fabs
#define __fabsf p2_fabsf
static inline double __fnabs(double x) { return -__builtin_fabs(x); }
static inline float __fnabsf(float x) { return -__builtin_fabsf(x); }

// PPC operand order: fmadd(a, c, b) = a * c + b. Kept unfused on purpose (-ffp-contract=off).
static inline double __fmadd(double a, double c, double b) { return a * c + b; }
static inline double __fmsub(double a, double c, double b) { return a * c - b; }
static inline double __fnmadd(double a, double c, double b) { return -(a * c + b); }
static inline double __fnmsub(double a, double c, double b) { return -(a * c - b); }
static inline float __fmadds(float a, float c, float b) { return a * c + b; }
static inline float __fmsubs(float a, float c, float b) { return a * c - b; }
static inline float __fnmadds(float a, float c, float b) { return -(a * c + b); }
static inline float __fnmsubs(float a, float c, float b) { return -(a * c - b); }

static inline double __fsel(double a, double b, double c) { return a >= 0.0 ? b : c; }
static inline float __fsels(float a, float b, float c) { return a >= 0.0f ? b : c; }

static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline double __fsqrt(double x) { return __builtin_sqrt(x); }
static inline float __fsqrts(float x) { return __builtin_sqrtf(x); }

static inline uint32_t __cntlzw(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }

static inline uint32_t __rlwinm(uint32_t x, int sh, int mb, int me)
{
	uint32_t r    = (x << sh) | (x >> ((32 - sh) & 31));
	uint32_t mask = (mb <= me) ? ((0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)))
	                           : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
	return r & mask;
}

static inline uint32_t __rlwimi(uint32_t dst, uint32_t x, int sh, int mb, int me)
{
	uint32_t r    = (x << sh) | (x >> ((32 - sh) & 31));
	uint32_t mask = (mb <= me) ? ((0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)))
	                           : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
	return (r & mask) | (dst & ~mask);
}

static inline uint16_t __lhbrx(const void* p, int off) { return __builtin_bswap16(*(const uint16_t*)((const char*)p + off)); }
static inline uint32_t __lwbrx(const void* p, int off) { return __builtin_bswap32(*(const uint32_t*)((const char*)p + off)); }
static inline void __sthbrx(uint16_t v, void* p, int off) { *(uint16_t*)((char*)p + off) = __builtin_bswap16(v); }
static inline void __stwbrx(uint32_t v, void* p, int off) { *(uint32_t*)((char*)p + off) = __builtin_bswap32(v); }

// Cache / sync instructions are no-ops on the host.
#define __dcbz(p, off)  ((void)0)
#define __dcbf(p, off)  ((void)0)
#define __dcbst(p, off) ((void)0)
#define __dcbi(p, off)  ((void)0)
#define __sync()        ((void)0)
#define __eieio()       ((void)0)
#define __isync()       ((void)0)

#ifdef __cplusplus
}
#endif

#endif
