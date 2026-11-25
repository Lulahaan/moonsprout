#ifndef _MSL_STRTOUL_H
#define _MSL_STRTOUL_H

#include "types.h"
#if TARGET_PC
#include <stdlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus

u32 __strtoul(int base, int max_width, int (*ReadProc)(void*, int, int), void* ReadProcArg, int* chars_scanned, int* negative,
              int* overflow);
u64 __strtoull(int base, int max_width, int (*ReadProc)(void*, int, int), void* ReadProcArg, int* chars_scanned, int* negative,
               int* overflow);
#if !TARGET_PC // from <stdlib.h> on host builds
int atoi(const char* str);
#endif
#if !TARGET_PC // from <stdlib.h> on host builds
u32 strtoul(const char* str, char** end, int base);
#endif

#ifdef __cplusplus
};
#endif // ifdef __cplusplus

#endif
