/* p2libm: forced first include of every p2libm source (-include p2m_names.h).
 * Renames the musl functions and their internal helpers to p2m_* so the sources stay as close
 * to upstream as possible and nothing collides with the host libm. The system headers are read
 * first, under their own names. */
#ifndef P2M_NAMES_H
#define P2M_NAMES_H

#include <stdint.h>
#include <float.h>
#include <math.h>

/* One code path on every target: never the fused multiply-add variants of log.c and pow.c. */
#ifdef __FP_FAST_FMA
#undef __FP_FAST_FMA
#endif

/* musl src/include/features.h */
#define hidden __attribute__((__visibility__("hidden")))

/* Exact operations: always the compiler builtin (one instruction), even under -fno-builtin. */
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)

#define sinf p2m_sinf
#define cosf p2m_cosf
#define tanf p2m_tanf
#define asinf p2m_asinf
#define acosf p2m_acosf
#define atanf p2m_atanf
#define atan2f p2m_atan2f
#define expf p2m_expf
#define logf p2m_logf
#define log10f p2m_log10f
#define powf p2m_powf
#define fmodf p2m_fmodf
#define floorf p2m_floorf
#define ceilf p2m_ceilf
#define lrintf p2m_lrintf
#define sqrtf p2m_sqrtf
#define sin p2m_sin
#define cos p2m_cos
#define tan p2m_tan
#define asin p2m_asin
#define acos p2m_acos
#define atan p2m_atan
#define atan2 p2m_atan2
#define exp p2m_exp
#define log p2m_log
#define log10 p2m_log10
#define pow p2m_pow
#define fmod p2m_fmod
#define floor p2m_floor
#define ceil p2m_ceil
#define ldexp p2m_ldexp
#define frexp p2m_frexp
#define modf p2m_modf
#define sqrt p2m_sqrt
#define rintf p2m_rintf
#define scalbn p2m_scalbn
#define __sin p2m___sin
#define __cos p2m___cos
#define __tan p2m___tan
#define __sindf p2m___sindf
#define __cosdf p2m___cosdf
#define __tandf p2m___tandf
#define __rem_pio2 p2m___rem_pio2
#define __rem_pio2f p2m___rem_pio2f
#define __rem_pio2_large p2m___rem_pio2_large
#define __exp_data p2m___exp_data
#define __exp2f_data p2m___exp2f_data
#define __logf_data p2m___logf_data
#define __log_data p2m___log_data
#define __pow_log_data p2m___pow_log_data
#define __powf_log2_data p2m___powf_log2_data
#define __math_xflowf p2m___math_xflowf
#define __math_uflowf p2m___math_uflowf
#define __math_oflowf p2m___math_oflowf
#define __math_divzerof p2m___math_divzerof
#define __math_invalidf p2m___math_invalidf
#define __math_xflow p2m___math_xflow
#define __math_uflow p2m___math_uflow
#define __math_oflow p2m___math_oflow
#define __math_divzero p2m___math_divzero
#define __math_invalid p2m___math_invalid

#include "p2m.h"

/* Internal helpers that upstream declares through musl's own <math.h>. */
double scalbn(double x, int n);
float rintf(float x);

#endif
