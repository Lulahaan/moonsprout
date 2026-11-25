// Pieces of Metrowerks MSL that game code touches directly, implemented for host builds.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 // dladdr
#endif
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#define P2_HAVE_DLADDR 1
#endif
#if defined(__linux__)
#include <link.h> // ElfW, ET_DYN
#endif

#include "p2_net.h"

#define ROW16(f, base) f(base+0), f(base+1), f(base+2), f(base+3), f(base+4), f(base+5), f(base+6), f(base+7), \
                       f(base+8), f(base+9), f(base+10), f(base+11), f(base+12), f(base+13), f(base+14), f(base+15)
#define LOWER(c) (unsigned char)(((c) >= 'A' && (c) <= 'Z') ? (c) + 32 : (c))
#define UPPER(c) (unsigned char)(((c) >= 'a' && (c) <= 'z') ? (c) - 32 : (c))
#define TABLE(f) { ROW16(f, 0), ROW16(f, 16), ROW16(f, 32), ROW16(f, 48), ROW16(f, 64), ROW16(f, 80), ROW16(f, 96), ROW16(f, 112), \
                   ROW16(f, 128), ROW16(f, 144), ROW16(f, 160), ROW16(f, 176), ROW16(f, 192), ROW16(f, 208), ROW16(f, 224), ROW16(f, 240) }

const unsigned char __lower_map[256] = TABLE(LOWER);
const unsigned char __upper_map[256] = TABLE(UPPER);

// ---- MSL_C rand()/srand(), see include/PowerPC_EABI_Support/MSL_C/MSL_Common/rand.h ----------
//
// M5.1b (docs/ONLINE_MULTIPLAYER.md section 4 item 3): three streams of the same LCG, chosen per
// call by thread and scope (the specification is the comment block in port/include/p2_net.h).
//   SIM    main game thread outside every presentation scope and outside Section::draw; any
//          thread during a DVD hand-off. The only stream the checksum sees.
//   PRES   main game thread inside a presentation scope or inside Section::draw.
//   OTHER  every other thread. Guarded by a spin lock.
// Before the first p2_msl_srand every call uses SIM (state 1), exactly as the single stream did.

#if defined(_MSC_VER) && !defined(__clang__)
#define P2_TLS __declspec(thread)
#else
#define P2_TLS _Thread_local
#endif

#define P2_LOAD(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define P2_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define P2_INC(p)      __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)

enum { kAuditSlots = 32, kAuditDrawUnflagged = 0, kAuditOtherInSession = 1, kAuditKinds = 2 };

typedef struct AuditEntry {
	uintptr_t addr;
	uint64_t count;
} AuditEntry;

static unsigned int s_sim   = 1;
static unsigned int s_pres  = 1;
static unsigned int s_other = 1;
static int s_seeded;           // a p2_msl_srand ran (written on the main thread before any split matters)
static P2_TLS int t_mainThread; // this thread called p2_msl_srand
static int s_presDepth;         // main thread only
static int s_drawDepth;         // main thread only
static int s_handoff;           // atomic
static int s_session;           // atomic: p2_rng_session(1) ran and (0) did not yet
static int s_counting;          // atomic: counters run (from the first p2_rng_session(1) on)
static int s_otherLock;         // atomic spin lock over s_other and the OTHER audit table

static int s_perturbRead;
static uint32_t s_perturbPres, s_perturbOther;

static int s_auditRead, s_audit;
static AuditEntry s_auditTab[kAuditKinds][kAuditSlots];
static uint64_t s_auditDropped[kAuditKinds];

// Counters (P2RngStats). The main-thread ones are plain; the others are updated atomically.
static uint64_t s_cSim, s_cPres, s_cDrawUnflagged; // main thread
static uint64_t s_cSimHandoff, s_cOther, s_cOtherInSession; // atomic

static inline unsigned int lcg_step(unsigned int* state)
{
	*state = *state * 1103515245u + 12345u;
	return (*state >> 16) & 0x7fff;
}

static void lock_other(void)
{
	while (__atomic_exchange_n(&s_otherLock, 1, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&s_otherLock, __ATOMIC_RELAXED)) {
		}
	}
}
static void unlock_other(void) { __atomic_store_n(&s_otherLock, 0, __ATOMIC_RELEASE); }

static int env_true(const char* name)
{
	const char* v = getenv(name);
	return v && *v && strcmp(v, "0") != 0;
}

static int audit_on(void)
{
	if (!s_auditRead) {
		s_audit     = env_true("MPPM_RNG_AUDIT");
		s_auditRead = 1;
	}
	return s_audit;
}

// Caller holds the table's owner: the main thread (draw-unflagged) or the OTHER lock.
static void audit_note(int kind, uintptr_t addr)
{
	AuditEntry* t = s_auditTab[kind];
	for (int i = 0; i < kAuditSlots; ++i) {
		if (t[i].addr == addr) {
			++t[i].count;
			return;
		}
		if (t[i].addr == 0) {
			t[i].addr  = addr;
			t[i].count = 1;
			return;
		}
	}
	++s_auditDropped[kind];
}

// MPPM_RNG_PERTURB="pres=<u32>,other=<u32>" (either part may be left out). Read once.
static void read_perturb(void)
{
	if (s_perturbRead) return;
	s_perturbRead = 1;
	const char* v = getenv("MPPM_RNG_PERTURB");
	if (!v || !*v) return;
	const char* p = v;
	int bad       = 0;
	while (*p) {
		uint32_t* dst = NULL;
		if (strncmp(p, "pres=", 5) == 0) {
			dst = &s_perturbPres;
			p += 5;
		} else if (strncmp(p, "other=", 6) == 0) {
			dst = &s_perturbOther;
			p += 6;
		} else {
			bad = 1;
			break;
		}
		if (*p < '0' || *p > '9') {
			bad = 1;
			break;
		}
		char* end              = NULL;
		unsigned long long val = strtoull(p, &end, 0);
		if (end == p || val > 0xFFFFFFFFull) {
			bad = 1;
			break;
		}
		*dst = (uint32_t)val;
		p    = end;
		if (*p == ',') ++p;
		else if (*p) {
			bad = 1;
			break;
		}
	}
	if (bad) {
		s_perturbPres = s_perturbOther = 0;
		fprintf(stderr, "mppm: net: bad MPPM_RNG_PERTURB '%s' (want pres=<u32>,other=<u32>); ignored\n", v);
	} else {
		fprintf(stderr, "mppm: net: rng perturb pres=%u other=%u\n", s_perturbPres, s_perturbOther);
	}
}

uint32_t p2_rng_mix(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

__attribute__((noinline)) int p2_msl_rand(void)
{
	if (!s_seeded) return (int)lcg_step(&s_sim);
	const int counting = P2_LOAD(&s_counting);
	if (t_mainThread) {
		if (s_presDepth > 0) {
			if (counting) ++s_cPres;
			return (int)lcg_step(&s_pres);
		}
		if (s_drawDepth > 0) {
			// The sim-stream assert (plan 2.7): this call would have been a sim draw.
			if (counting) {
				++s_cDrawUnflagged;
				if (audit_on()) audit_note(kAuditDrawUnflagged, (uintptr_t)__builtin_return_address(0));
			}
			return (int)lcg_step(&s_pres);
		}
		if (counting) ++s_cSim;
		return (int)lcg_step(&s_sim);
	}
	if (P2_LOAD(&s_handoff)) {
		// The main thread is blocked in p2_net_dvd_sync until this command is done.
		if (counting) P2_INC(&s_cSimHandoff);
		return (int)lcg_step(&s_sim);
	}
	lock_other();
	int r = (int)lcg_step(&s_other);
	if (counting) {
		++s_cOther;
		if (P2_LOAD(&s_session)) {
			++s_cOtherInSession;
			if (audit_on()) audit_note(kAuditOtherInSession, (uintptr_t)__builtin_return_address(0));
		}
	}
	unlock_other();
	return r;
}

void p2_msl_srand(unsigned int seed)
{
	read_perturb();
	t_mainThread = 1;
	s_sim        = seed;
	s_pres       = p2_rng_mix(seed ^ 0x50524553u) ^ s_perturbPres;
	lock_other();
	s_other = p2_rng_mix(seed ^ 0x4F544852u) ^ s_perturbOther;
	unlock_other();
	P2_STORE(&s_seeded, 1);
}

unsigned int p2_msl_rand_state(void) { return s_sim; }

unsigned int p2_rng_state(int stream)
{
	switch (stream) {
	case P2_RNG_SIM: return s_sim;
	case P2_RNG_PRES: return s_pres;
	case P2_RNG_OTHER: {
		lock_other();
		unsigned int v = s_other;
		unlock_other();
		return v;
	}
	default: return 0;
	}
}

void p2_rng_set_sim(unsigned int state) { s_sim = state; }

void p2_rng_pres_begin(void)
{
	if (t_mainThread) ++s_presDepth;
}
void p2_rng_pres_end(void)
{
	if (t_mainThread && s_presDepth > 0) --s_presDepth;
}
void p2_rng_draw_begin(void)
{
	if (t_mainThread) ++s_drawDepth;
}
void p2_rng_draw_end(void)
{
	if (t_mainThread && s_drawDepth > 0) --s_drawDepth;
}

void p2_rng_handoff(int on) { P2_STORE(&s_handoff, on ? 1 : 0); }

static void print_audit(int kind, const char* name)
{
	const AuditEntry* t = s_auditTab[kind];
	if (t[0].addr == 0) return;
	fprintf(stderr, "mppm: net: rng audit %s (address relative to its module; addr2line -e <module> <address>):\n", name);
	for (int i = 0; i < kAuditSlots && t[i].addr; ++i) {
		uintptr_t rel      = t[i].addr;
		const char* module = "?";
#if P2_HAVE_DLADDR
		Dl_info info;
		if (dladdr((void*)t[i].addr, &info) && info.dli_fbase) {
			int relocatable = 1;
#if defined(__linux__)
			// A non-PIE executable (ET_EXEC) is linked at its run address: addr2line takes it as is.
			relocatable = ((const ElfW(Ehdr)*)info.dli_fbase)->e_type == ET_DYN;
#endif
			if (relocatable) rel = t[i].addr - (uintptr_t)info.dli_fbase;
			if (info.dli_fname) module = info.dli_fname;
		}
#endif
		// The return address is the instruction after the call: addr2line on rel-1 names the call.
		fprintf(stderr, "mppm: net:   0x%llx count=%llu module=%s\n", (unsigned long long)(rel ? rel - 1 : 0),
		        (unsigned long long)t[i].count, module);
	}
	if (s_auditDropped[kind]) {
		fprintf(stderr, "mppm: net:   (%llu more calls from other addresses)\n", (unsigned long long)s_auditDropped[kind]);
	}
}

void p2_rng_session(int on)
{
	if (on) {
		P2_STORE(&s_counting, 1);
		P2_STORE(&s_session, 1);
		(void)audit_on();
		return;
	}
	if (!P2_LOAD(&s_session)) return;
	P2_STORE(&s_session, 0);
	if (audit_on()) {
		print_audit(kAuditDrawUnflagged, "draw-unflagged");
		lock_other();
		print_audit(kAuditOtherInSession, "other-in-session");
		unlock_other();
		fflush(stderr);
	}
}

void p2_rng_stats(P2RngStats* out)
{
	if (!out) return;
	out->sim           = s_cSim;
	out->simHandoff    = __atomic_load_n(&s_cSimHandoff, __ATOMIC_RELAXED);
	out->pres          = s_cPres;
	out->drawUnflagged = s_cDrawUnflagged;
	lock_other();
	out->other          = s_cOther;
	out->otherInSession = s_cOtherInSession;
	unlock_other();
}

// For the self-test in port/net/net.cpp only (not in p2_net.h): sets the perturbation as if
// MPPM_RNG_PERTURB had given it.
void p2_rng_test_set_perturb(uint32_t pres, uint32_t other)
{
	s_perturbRead  = 1;
	s_perturbPres  = pres;
	s_perturbOther = other;
}
