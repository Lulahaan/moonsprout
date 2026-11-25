/* p2libm: the vendored math library (musl 1.2.5 subset, see README.md). Public interface.
 * Every function returns the same bits on every host and CPU: plain C, no FMA, no long double,
 * no CPU dispatch, built with -ffp-contract=off. Game code reaches these through the symbol
 * routing in CMakeLists.txt (sinf -> p2m_sinf), not by including this header. */
#ifndef P2M_H
#define P2M_H

#ifdef __cplusplus
extern "C" {
#endif

float p2m_sinf(float x);
float p2m_cosf(float x);
float p2m_tanf(float x);
float p2m_asinf(float x);
float p2m_acosf(float x);
float p2m_atanf(float x);
float p2m_atan2f(float y, float x);
float p2m_expf(float x);
float p2m_logf(float x);
float p2m_log10f(float x);
float p2m_powf(float x, float y);
float p2m_fmodf(float x, float y);
float p2m_floorf(float x);
float p2m_ceilf(float x);
long p2m_lrintf(float x);
float p2m_sqrtf(float x);

double p2m_sin(double x);
double p2m_cos(double x);
double p2m_tan(double x);
double p2m_asin(double x);
double p2m_acos(double x);
double p2m_atan(double x);
double p2m_atan2(double y, double x);
double p2m_exp(double x);
double p2m_log(double x);
double p2m_log10(double x);
double p2m_pow(double x, double y);
double p2m_fmod(double x, double y);
double p2m_floor(double x);
double p2m_ceil(double x);
double p2m_ldexp(double x, int n);
double p2m_frexp(double x, int* e);
double p2m_modf(double x, double* iptr);
double p2m_sqrt(double x);

#ifdef __cplusplus
}
#endif

#endif
