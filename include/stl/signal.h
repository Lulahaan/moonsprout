#if TARGET_PC
#ifdef __cplusplus
extern "C++" {
#endif
#include_next <signal.h>
#include "types.h"
#ifdef __cplusplus
}
#endif
#else // TARGET_PC

#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void exit(int);

typedef void (*sig_func)(int sig);

int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif

#endif // TARGET_PC
