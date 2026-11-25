#if TARGET_PC
#ifdef __cplusplus
extern "C++" {
#endif
#ifndef _P2_PC_EXTRAS_H
#define _P2_PC_EXTRAS_H
#if defined(_WIN32)
#include <string.h>
#include "types.h"
#define stricmp _stricmp
#else
#include <strings.h>
#include "types.h"
static inline int stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
#endif
#endif
#ifdef __cplusplus
}
#endif
#else // TARGET_PC

#ifndef _EXTRAS_H
#define _EXTRAS_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

int stricmp(const char*, const char*);

#ifdef __cplusplus
}
#endif
#endif

#endif // TARGET_PC
