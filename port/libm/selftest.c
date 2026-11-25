/* p2libm_selftest: shows that the vendored math gives the same bits on every host, and that the
 * host's libm does not (docs/online/M5_1B_BUILD.md).
 *
 * usage: p2libm_selftest [expected.txt]
 *
 * For every function it prints one FNV-1a 32 hash of the result bits over fixed pseudo-random
 * inputs, "p2m <name> <hash>" for the vendored function and "host <name> <hash>" for the system
 * one, then "ulp <name> max=<n> one=<n> more=<n>": the largest distance in units in the last place
 * between the two over the same inputs, how many results differ by exactly 1 ulp and how many by
 * more. With an argument (port/libm/selftest_expected.txt) it compares every "p2m" line with the
 * file and exits 1 on any difference.
 *
 * The host functions are called through volatile pointers, so the compiler cannot fold or inline
 * them. Run it with GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA to see the host hashes change. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p2m.h"

#define N_SAMPLES 4000000

typedef float (*F1)(float);
typedef float (*F2)(float, float);
typedef double (*D1)(double);
typedef double (*D2)(double, double);

static uint32_t fnv_bytes(uint32_t h, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

/* The generator of m51b_libm_probe.c: a 32-bit LCG, 24 bits mapped to [lo, hi). */
static uint32_t lcg(uint32_t* s)
{
	*s = *s * 1103515245u + 12345u;
	return (*s >> 8) & 0xffffff;
}
static float inf_(uint32_t* s, float lo, float hi) { return lo + (hi - lo) * (float)lcg(s) / 16777216.0f; }
static double ind_(uint32_t* s, double lo, double hi) { return lo + (hi - lo) * (double)lcg(s) / 16777216.0; }

/* Distance in ulps between two floats of the same sign (or both zero); NaN pairs count as 0. */
static uint64_t ulpf(float a, float b)
{
	if (a != a && b != b) {
		return 0;
	}
	int32_t ia, ib;
	memcpy(&ia, &a, 4);
	memcpy(&ib, &b, 4);
	if (ia < 0) {
		ia = (int32_t)0x80000000 - ia;
	}
	if (ib < 0) {
		ib = (int32_t)0x80000000 - ib;
	}
	int64_t d = (int64_t)ia - (int64_t)ib;
	return (uint64_t)(d < 0 ? -d : d);
}
static uint64_t ulpd(double a, double b)
{
	if (a != a && b != b) {
		return 0;
	}
	int64_t ia, ib;
	memcpy(&ia, &a, 8);
	memcpy(&ib, &b, 8);
	if (ia < 0) {
		ia = INT64_MIN - ia;
	}
	if (ib < 0) {
		ib = INT64_MIN - ib;
	}
	/* Both are in [-2^63, 2^63): the difference cannot overflow for finite results of one sign. */
	uint64_t d = ia > ib ? (uint64_t)ia - (uint64_t)ib : (uint64_t)ib - (uint64_t)ia;
	return d;
}

typedef struct Result {
	uint32_t p2m;
	uint32_t host;
	uint64_t maxUlp;
	uint32_t one;
	uint32_t more;
} Result;

static void note(Result* r, uint64_t u)
{
	if (u > r->maxUlp) {
		r->maxUlp = u;
	}
	if (u == 1) {
		r->one++;
	} else if (u > 1) {
		r->more++;
	}
}

static Result run_f1(F1 mine, volatile F1 host, float lo, float hi)
{
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 12345;
	for (int i = 0; i < N_SAMPLES; i++) {
		float x = inf_(&s, lo, hi);
		float a = mine(x), b = host(x);
		r.p2m = fnv_bytes(r.p2m, &a, 4);
		r.host = fnv_bytes(r.host, &b, 4);
		note(&r, ulpf(a, b));
	}
	return r;
}

static Result run_f2(F2 mine, volatile F2 host, float lo, float hi)
{
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 777;
	for (int i = 0; i < N_SAMPLES; i++) {
		float x = inf_(&s, lo, hi);
		float y = inf_(&s, lo, hi);
		float a = mine(x, y), b = host(x, y);
		r.p2m = fnv_bytes(r.p2m, &a, 4);
		r.host = fnv_bytes(r.host, &b, 4);
		note(&r, ulpf(a, b));
	}
	return r;
}

static Result run_d1(D1 mine, volatile D1 host, double lo, double hi)
{
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 99;
	for (int i = 0; i < N_SAMPLES; i++) {
		double x = ind_(&s, lo, hi);
		double a = mine(x), b = host(x);
		r.p2m = fnv_bytes(r.p2m, &a, 8);
		r.host = fnv_bytes(r.host, &b, 8);
		note(&r, ulpd(a, b));
	}
	return r;
}

static Result run_d2(D2 mine, volatile D2 host, double lo, double hi)
{
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 4242;
	for (int i = 0; i < N_SAMPLES; i++) {
		double x = ind_(&s, lo, hi);
		double y = ind_(&s, lo, hi);
		double a = mine(x, y), b = host(x, y);
		r.p2m = fnv_bytes(r.p2m, &a, 8);
		r.host = fnv_bytes(r.host, &b, 8);
		note(&r, ulpd(a, b));
	}
	return r;
}

/* The functions with an integer part: the hash covers every output. */
static Result run_lrintf(void)
{
	long (*volatile host)(float) = lrintf;
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 31337;
	for (int i = 0; i < N_SAMPLES; i++) {
		float x = inf_(&s, -1.0e6f, 1.0e6f);
		int64_t a = p2m_lrintf(x), b = host(x);
		r.p2m = fnv_bytes(r.p2m, &a, 8);
		r.host = fnv_bytes(r.host, &b, 8);
		note(&r, a == b ? 0 : 2);
	}
	return r;
}

static Result run_ldexp(void)
{
	double (*volatile host)(double, int) = ldexp;
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 2718;
	for (int i = 0; i < N_SAMPLES; i++) {
		double x = ind_(&s, -100.0, 100.0);
		int n = (int)(lcg(&s) % 2200) - 1100;
		double a = p2m_ldexp(x, n), b = host(x, n);
		r.p2m = fnv_bytes(r.p2m, &a, 8);
		r.host = fnv_bytes(r.host, &b, 8);
		note(&r, ulpd(a, b));
	}
	return r;
}

static Result run_frexp(void)
{
	double (*volatile host)(double, int*) = frexp;
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 1618;
	for (int i = 0; i < N_SAMPLES; i++) {
		double x = ind_(&s, -1.0e6, 1.0e6);
		int ea = 0, eb = 0;
		double a = p2m_frexp(x, &ea), b = host(x, &eb);
		r.p2m = fnv_bytes(fnv_bytes(r.p2m, &a, 8), &ea, 4);
		r.host = fnv_bytes(fnv_bytes(r.host, &b, 8), &eb, 4);
		note(&r, ulpd(a, b) + (ea != eb ? 2 : 0));
	}
	return r;
}

static Result run_modf(void)
{
	double (*volatile host)(double, double*) = modf;
	Result r = { 2166136261u, 2166136261u, 0, 0, 0 };
	uint32_t s = 1414;
	for (int i = 0; i < N_SAMPLES; i++) {
		double x = ind_(&s, -1.0e6, 1.0e6);
		double ia = 0, ib = 0;
		double a = p2m_modf(x, &ia), b = host(x, &ib);
		r.p2m = fnv_bytes(fnv_bytes(r.p2m, &a, 8), &ia, 8);
		r.host = fnv_bytes(fnv_bytes(r.host, &b, 8), &ib, 8);
		note(&r, ulpd(a, b) + ulpd(ia, ib));
	}
	return r;
}

#define MAX_FUNCS 40
static char gNames[MAX_FUNCS][16];
static uint32_t gHashes[MAX_FUNCS];
static int gCount;

static void report(const char* name, Result r)
{
	printf("p2m %s %08x\n", name, r.p2m);
	printf("host %s %08x\n", name, r.host);
	printf("ulp %s max=%llu one=%u more=%u\n", name, (unsigned long long)r.maxUlp, r.one, r.more);
	fflush(stdout);
	if (gCount < MAX_FUNCS) {
		snprintf(gNames[gCount], sizeof gNames[gCount], "%s", name);
		gHashes[gCount] = r.p2m;
		gCount++;
	}
}

int main(int argc, char** argv)
{
	report("sinf", run_f1(p2m_sinf, sinf, -100.0f, 100.0f));
	report("cosf", run_f1(p2m_cosf, cosf, -100.0f, 100.0f));
	report("tanf", run_f1(p2m_tanf, tanf, -1.5f, 1.5f));
	report("asinf", run_f1(p2m_asinf, asinf, -1.0f, 1.0f));
	report("acosf", run_f1(p2m_acosf, acosf, -1.0f, 1.0f));
	report("atanf", run_f1(p2m_atanf, atanf, -50.0f, 50.0f));
	report("atan2f", run_f2(p2m_atan2f, atan2f, -100.0f, 100.0f));
	report("expf", run_f1(p2m_expf, expf, -20.0f, 20.0f));
	report("logf", run_f1(p2m_logf, logf, 0.0001f, 1000.0f));
	report("log10f", run_f1(p2m_log10f, log10f, 0.0001f, 1000.0f));
	report("powf", run_f2(p2m_powf, powf, 0.01f, 8.0f));
	report("fmodf", run_f2(p2m_fmodf, fmodf, -100.0f, 100.0f));
	report("floorf", run_f1(p2m_floorf, floorf, -1000.0f, 1000.0f));
	report("ceilf", run_f1(p2m_ceilf, ceilf, -1000.0f, 1000.0f));
	report("lrintf", run_lrintf());
	report("sqrtf", run_f1(p2m_sqrtf, sqrtf, 0.0f, 10000.0f));
	report("sin", run_d1(p2m_sin, sin, -100.0, 100.0));
	report("cos", run_d1(p2m_cos, cos, -100.0, 100.0));
	report("tan", run_d1(p2m_tan, tan, -1.5, 1.5));
	report("asin", run_d1(p2m_asin, asin, -1.0, 1.0));
	report("acos", run_d1(p2m_acos, acos, -1.0, 1.0));
	report("atan", run_d1(p2m_atan, atan, -50.0, 50.0));
	report("atan2", run_d2(p2m_atan2, atan2, -100.0, 100.0));
	report("exp", run_d1(p2m_exp, exp, -20.0, 20.0));
	report("log", run_d1(p2m_log, log, 0.0001, 1000.0));
	report("log10", run_d1(p2m_log10, log10, 0.0001, 1000.0));
	report("pow", run_d2(p2m_pow, pow, 0.01, 8.0));
	report("fmod", run_d2(p2m_fmod, fmod, -100.0, 100.0));
	report("floor", run_d1(p2m_floor, floor, -1000.0, 1000.0));
	report("ceil", run_d1(p2m_ceil, ceil, -1000.0, 1000.0));
	report("ldexp", run_ldexp());
	report("frexp", run_frexp());
	report("modf", run_modf());
	report("sqrt", run_d1(p2m_sqrt, sqrt, 0.0, 10000.0));

	if (argc < 2) {
		return 0;
	}
	FILE* f = fopen(argv[1], "r");
	if (!f) {
		fprintf(stderr, "p2libm_selftest: cannot open %s\n", argv[1]);
		return 1;
	}
	char line[256];
	int checked = 0, bad = 0;
	while (fgets(line, sizeof line, f)) {
		char tag[8], name[32];
		unsigned hash;
		if (sscanf(line, "%7s %31s %x", tag, name, &hash) != 3 || strcmp(tag, "p2m") != 0) {
			continue;
		}
		int found = 0;
		for (int i = 0; i < gCount; i++) {
			if (strcmp(gNames[i], name) == 0) {
				found = 1;
				checked++;
				if (gHashes[i] != hash) {
					fprintf(stderr, "p2libm_selftest: %s: %08x, expected %08x\n", name, gHashes[i], hash);
					bad++;
				}
			}
		}
		if (!found) {
			fprintf(stderr, "p2libm_selftest: %s: in the file but not tested\n", name);
			bad++;
		}
	}
	fclose(f);
	if (checked != gCount) {
		fprintf(stderr, "p2libm_selftest: %d of %d functions in the file\n", checked, gCount);
		bad++;
	}
	printf("selftest: %d checked, %d bad\n", checked, bad);
	return bad ? 1 : 0;
}
