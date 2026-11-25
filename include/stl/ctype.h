#if TARGET_PC
#ifdef __cplusplus
extern "C++" {
#endif
#include_next <ctype.h>
#include "types.h"
#ifdef __cplusplus
}
#endif
#ifndef _P2_PC_CTYPE_EXTRAS
#define _P2_PC_CTYPE_EXTRAS
#ifdef __cplusplus
extern "C" {
#endif
// MSL tables some game code indexes directly (port/sdk/msl_compat.c).
extern const unsigned char __lower_map[256];
extern const unsigned char __upper_map[256];
#ifdef __cplusplus
}
#endif
static inline int isprintable(int c) { return isprint(c & 0xFF); }
#if !defined(_tolower) && !defined(__GLIBC__) // glibc declares _tolower itself
static inline int _tolower(int c) { return (c == -1 ? -1 : (int)__lower_map[(u8)c]); }
#endif
#endif
#else // TARGET_PC

#ifndef _CTYPE_H
#define _CTYPE_H

#include "types.h"
#include "locale.h"
#include "PowerPC_EABI_Support/MSL_C/MSL_Common/ctype_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int tolower(int __c);

inline int isalpha(int c)
{
	return (int)(__ctype_map[(u8)c] & __letter);
}
inline int isdigit(int c)
{
	return (int)(__ctype_map[(u8)c] & __digit);
}
inline int isspace(int c)
{
	return (int)(__ctype_map[(u8)c] & __whitespace);
}
inline int isupper(int c)
{
	return (int)(__ctype_map[(u8)c] & __upper_case);
}
inline int isxdigit(int c)
{
	return (int)(__ctype_map[(u8)c] & __hex_digit);
}
inline int isprintable(int c)
{
	return (int)(__ctype_map[(u8)c] & __printable);
}
// added underscore to avoid naming conflicts
inline int _tolower(int c)
{
	return (c == -1 ? -1 : (int)__lower_map[(u8)c]);
}
inline int toupper(int c)
{
	return (c == -1 ? -1 : (int)__upper_map[(u8)c]);
}

#ifdef __cplusplus
}
#endif
#endif

#endif // TARGET_PC
