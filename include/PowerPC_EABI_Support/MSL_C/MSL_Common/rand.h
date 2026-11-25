#ifndef _MSL_RAND_H
#define _MSL_RAND_H

#include "types.h"

#if TARGET_PC
// The game depends on MSL's rand() (LCG, RAND_MAX 0x7FFF). Route calls to a copy of it so gameplay
// randomness matches the original and is identical on every host (netplay determinism).
#include <stdlib.h>
#ifdef __cplusplus
#include <cstdlib>
extern "C" int p2_msl_rand(void);
extern "C" void p2_msl_srand(unsigned int seed);
namespace std {
using ::p2_msl_rand; // so std::rand (after the macro below) still resolves
using ::p2_msl_srand;
} // namespace std
#endif
#define rand  p2_msl_rand
#define srand p2_msl_srand
#endif

#ifdef __cplusplus
extern "C" {
#endif // ifdef __cplusplus

int rand();
void srand(u32 seed);

#ifdef __cplusplus
};
#endif // ifdef __cplusplus

#endif
