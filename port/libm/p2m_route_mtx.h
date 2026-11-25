/* p2libm: forced first include of Aurora's matrix library (aurora_mtx, CMakeLists.txt).
 * <math.h> is read under its own names first; then the calls of mtx.c, mtx44.c, vec.c and quat.c
 * go to the vendored functions, so simulation code that reaches MTXRotRad, MTXRotAxisRad or the
 * quaternion functions gets the same bits on every host. fmaf (vec.c) stays the host's: IEEE-754
 * requires it to be correctly rounded, so every libm returns the same bits. */
#ifndef P2M_ROUTE_MTX_H
#define P2M_ROUTE_MTX_H

#include <math.h>
#include "p2m.h"

#define sinf p2m_sinf
#define cosf p2m_cosf
#define tanf p2m_tanf
#define asinf p2m_asinf
#define acosf p2m_acosf
#define atanf p2m_atanf
#define atan2f p2m_atan2f
#define sqrtf p2m_sqrtf

#endif
