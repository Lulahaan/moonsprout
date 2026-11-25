#ifndef _TYPES_H
#define _TYPES_H

#ifdef __INTELLISENSE__
#include "../.vscode/warnings.h"
#endif

#include "BuildSettings.h"

// r2 is  8051E360
// r13 is 8051C680

#if TARGET_PC
#ifdef __cplusplus
extern "C++" {
#endif
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#ifndef __cplusplus
#include <stdbool.h>
#define nullptr 0
#endif
#ifdef __cplusplus
}
#endif

typedef int BOOL;
typedef unsigned int uint;

// MWCC's long is 32 bits; use fixed-width types so LP64 hosts keep the original sizes.
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;
typedef long double f128;
typedef volatile f32 vf32;
typedef volatile f64 vf64;
typedef volatile f128 vf128;

typedef u32 unknown;
#include "ppc_intrinsics.h"
// Pointer-sized integer, for code that stored pointers in u32 on the GameCube.
typedef uintptr_t uptr;
typedef intptr_t sptr;

#else
typedef int BOOL;
typedef unsigned int uint;

typedef signed char s8;
typedef signed short s16;
typedef signed long s32;
typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;
typedef long double f128;
typedef volatile f32 vf32;
typedef volatile f64 vf64;
typedef volatile f128 vf128;

typedef u32 size_t;
typedef u32 unknown;

#ifndef __cplusplus
typedef u16 wchar_t;
#endif

typedef u32 uptr;
typedef s32 sptr;
#endif // TARGET_PC

#define SHORT_FLOAT_MAX (32768.0f)
#define SHORT_FLOAT_MIN (-32768.0f)

#define USHORT_MAX (65535)

// Basic defines to allow newer-like C++ code to be written
#define TRUE    1
#define FALSE   0
#if !TARGET_PC
#define NULL    ((void*)0)
#define nullptr 0
#endif

// Maximum length of a path in engine
#ifdef PATH_MAX
#undef PATH_MAX
#endif
#define PATH_MAX (256)

#define SET_FLAG(x, val)     (x |= (val))
#define RESET_FLAG(x, val)   (x &= ~(val))
#define IS_FLAG(x, val)      (x & val)
#define ARRAY_SIZE(o)        (sizeof((o)) / sizeof(*(o)))
#define ALIGN_PREV(X, N)     ((X) & ~((N) - 1))
#define ALIGN_NEXT(X, N)     ALIGN_PREV(((X) + (N) - 1), N)
#define IS_ALIGNED(X, N)     ((X & ((N) - 1)) == 0)
#define IS_NOT_ALIGNED(X, N) (((X) & ((N) - 1)) != 0)
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

// MWCC lets a switch jump past initialized locals; standard C++ does not. These wrap such case
// bodies in a block on host builds only, leaving MWCC codegen (and register allocation) unchanged.
#if TARGET_PC
#define PC_SCOPE_BEGIN {
#define PC_SCOPE_END   }
#else
#define PC_SCOPE_BEGIN
#define PC_SCOPE_END
#endif

// Host-only statements written on an existing source line, so files with line-numbered asserts
// keep their __LINE__ values (and MWCC output) unchanged.
#if TARGET_PC
#define PC_ONLY(...) __VA_ARGS__
#define PC_SWITCH(pc, gc) pc
#else
#define PC_ONLY(...)
#define PC_SWITCH(pc, gc) gc
#endif

// Multi-byte fields of structs read directly from GameCube resource files. On the host they
// stay big-endian in memory and swap on access (port/include/p2_be.h).
#if TARGET_PC && defined(__cplusplus)
#include "p2_be.h"
#include "p2_host.h"
#define BE(T) p2::BigEndian<T>
#define BE_READ(T, p) p2_read_be<T>(p)
#else
#define BE(T) T
#define BE_READ(T, p) (*(T*)(p))
#endif

// Multi-character literals of 5-8 characters ('Ngame'): MWCC evaluates them to a 64-bit value,
// clang truncates them to the last four characters. Sources spell them MC64('Ngame', 0x4E67616D65);
// MWCC still sees the literal, host builds the 64-bit value.
#if TARGET_PC
#define MC64(lit, val) ((unsigned long long)val##ULL)
#else
#define MC64(lit, val) lit
#endif

// Explicit specializations defined in headers: not implicitly inline in standard C++ (duplicate
// symbols at link time); MWCC treated them as weak. Keep MWCC codegen unchanged.
#if TARGET_PC
#define PC_INLINE inline
#else
#define PC_INLINE
#endif

// `template <>` on explicit specializations that MWCC accepted without it. Adding it changes
// MWCC's function emission order, so only emit it for host builds.
#if TARGET_PC
#define PC_TEMPLATE_SPEC template <>
#else
#define PC_TEMPLATE_SPEC
#endif

// The original has a bare `return;` in a non-void function (MWCC returns whatever is left in r3).
// Host builds return `value` instead; see the fidelity notes in docs/PORT_PLAN.md.
#if TARGET_PC
#define RETURN_GARBAGE(value) return value
#else
#define RETURN_GARBAGE(value) return
#endif

#define ASSERT_HANG(cond) \
	if (!(cond)) {        \
		while (true) { }  \
	}

#define CLAMP_VALUE_ABOVE(val, limit)              ((val) > (limit)) ? (limit) : (val)
#define MAX(a, b)                                  (((a) > (b)) ? (a) : (b))
#define MIN(a, b)                                  (((a) < (b)) ? (a) : (b))
#define ROUND_F32_TO_U8(a)                         ((a) >= 0.0f) ? ((a) + 0.5f) : ((a) - 0.5f)
#define INTERPOLATE_BETWEEN(src, dest, proportion) (proportion) * ((f32)(dest) - (f32)(src)) + (f32)(src)

#ifdef __MWERKS__
#define WEAKFUNC        __declspec(weak)
#define DECL_SECT(name) __declspec(section name)
#define ASM             asm
#else
#define WEAKFUNC
#define DECL_SECT(name)
#define ASM
#endif

#ifdef __MWERKS__
#define FAST_COPY(dst, src, size) __memcpy((dst), (src), (size))
#else
#define FAST_COPY(dst, src, size) memcpy((dst), (src), (size))
#endif

// byte-matching hack to force something to hang around longer
// in MWCC's register allocation steps
#ifdef __MWERKS__
#define BUMP_VAR(value) asm { mr value, value }
#else
#define BUMP_VAR(value) ((void)0)
#endif

#if TARGET_PC
// Decomp-era Dolphin SDK names on top of Aurora's SDK headers (see port/include/p2_sdk_compat.h).
#include "p2_sdk_compat.h"
#endif

#endif // _TYPES_H
