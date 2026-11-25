// p2net_selftest (M5.3b, docs/online/M5_3B_LOCKSTEP.md "Tests"): the pure parts of the session
// package and the netsim lockstep cases. `ninja -C build-pc p2net_selftest && ./build-pc/p2net_selftest`
// [-v] [case ...]. Exit 0 only if every case passed; one PASS/FAIL line per case.
#include "netsim.hpp"

#include "../../../port/net/session/clock_sync.hpp"
#include "../../../port/net/session/delay.hpp"
#include "../../../port/net/session/host_core.hpp"
#include "../../../port/net/session/wire.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace mppm::session;

namespace {

bool g_verbose = false;
int g_fails    = 0;
std::string g_why;

#define CHECK(cond, ...)                                          \
	do {                                                          \
		if (!(cond)) {                                            \
			char b_[512];                                         \
			std::snprintf(b_, sizeof(b_), __VA_ARGS__);           \
			if (!g_why.empty()) g_why += "; ";                    \
			g_why += b_;                                          \
			ok = false;                                           \
		}                                                         \
	} while (0)

uint32_t g_rng = 12345;
uint32_t rnd()
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

// ---- wire ----------------------------------------------------------------------------------------

template <typename T>
bool roundtrip(const T& m, T& back, uint32_t tag = 7)
{
	std::vector<uint8_t> d;
	wire::encode(d, tag, m);
	if (wire::decode(d.data(), d.size(), back) != wire::Result::Ok) return false;
	std::vector<uint8_t> d2;
	wire::encode(d2, tag, back);
	return d == d2;
}

bool case_wire()
{
	bool ok = true;
	std::vector<std::vector<uint8_t>> corpus;
	auto keep = [&corpus](auto& m) {
		std::vector<uint8_t> d;
		wire::encode(d, 0x1234, m);
		corpus.push_back(d);
	};
	{
		wire::Hello h, b;
		h.member = 3;
		h.secret = 0x0123456789ABCDEFull;
		h.spectator = true;
		h.name = "Louie";
		for (int i = 0; i < 32; ++i) h.build[i] = (uint8_t)i, h.content[i] = (uint8_t)(255 - i);
		h.mods = "heave-arenas@1.2.0,other@3";
		CHECK(roundtrip(h, b) && b.name == "Louie" && b.secret == h.secret && b.spectator && b.mods == h.mods, "hello");
		keep(h);
	}
	{
		wire::Welcome w{ 5, 2 }, b;
		CHECK(roundtrip(w, b) && b.member == 5 && b.role == 2, "welcome");
		keep(w);
		wire::Reject r{ 3, "The lobby is full." }, rb;
		CHECK(roundtrip(r, rb) && rb.text == r.text, "reject");
		keep(r);
	}
	{
		wire::Lobby l, b;
		l.generation = 9;
		l.mode = 1;
		l.stage = 4;
		l.seats = 4;
		l.handicap[0] = 0;
		for (int i = 0; i < 5; ++i) {
			wire::LobbyMember m;
			m.id = (uint32_t)i;
			m.slot = i < 4 ? (uint8_t)i : kNoSlot;
			m.flags = 0x1F;
			m.pingMs = (uint16_t)(i * 10);
			m.name = "Name" + std::to_string(i);
			m.note = i == 2 ? "Name2 has different mods: x 1 (host 2)." : "";
			l.members.push_back(m);
		}
		CHECK(roundtrip(l, b) && b.members.size() == 5 && b.members[2].note == l.members[2].note, "lobby");
		keep(l);
	}
	{
		wire::Req q{ wire::kReqSlot, -1 }, b;
		CHECK(roundtrip(q, b) && b.value == -1, "req");
		keep(q);
	}
	{
		SessionConfig c, b;
		c.sessionTag = 0xABCD;
		c.seed = 99;
		c.mode = Mode::Battle2v2;
		c.seats = 4;
		c.slotMask = 0xF;
		c.stage = 3;
		for (int i = 0; i < 4; ++i) c.slotMember[i] = (uint32_t)(i * 2);
		c.simK = 3.6f;
		c.syncedJson = "{\"game.x\":1}";
		c.mods = "a@1";
		CHECK(roundtrip(c, b, 0) && b.slotMember[3] == 6 && b.syncedJson == c.syncedJson && b.simK == 3.6f, "start");
		keep(c);
	}
	{
		wire::Release r{ 5000, 2, 123456789012345ull }, b;
		CHECK(roundtrip(r, b) && b.releaseHostNs == r.releaseHostNs && b.count == 2, "release");
		keep(r);
		wire::Drop dr{ 3, 4501, 2 }, db;
		CHECK(roundtrip(dr, db) && db.effectiveTick == 4501, "drop");
		keep(dr);
		wire::Desync ds{ 900, 1, 0x11, 0x22 }, dsb;
		CHECK(roundtrip(ds, dsb) && dsb.peerTotal == 0x22, "desync");
		keep(ds);
		wire::CksLine cl{ 900, "C 900 deadbeef rng=..." }, clb;
		CHECK(roundtrip(cl, clb) && clb.line == cl.line, "cksline");
		keep(cl);
		wire::End e{ 2, "The host left." }, eb;
		CHECK(roundtrip(e, eb), "end");
		keep(e);
		wire::Bye y{ 1 }, yb;
		CHECK(roundtrip(y, yb), "bye");
		keep(y);
	}
	{
		wire::InputPkt in, b;
		in.tSendUs = 1;
		in.echoUs = 2;
		in.holdUs = 3;
		in.ackBundle = 100;
		in.firstTag = 101;
		in.count = 32;
		in.slot = 1;
		in.delay = 4;
		in.loadedTick = 0;
		in.loadCount = 1;
		in.cksTick = 90;
		in.cksTotal = 0xFEEDu;
		in.downP99 = 512;
		for (int i = 0; i < 32; ++i) in.inputs[i].b[0] = (uint8_t)i;
		CHECK(roundtrip(in, b) && b.count == 32 && b.inputs[31].b[0] == 31, "input");
		std::vector<uint8_t> d;
		wire::encode(d, 1, in);
		CHECK(d.size() == 50 + 32 * 8, "input size %zu", d.size());
		keep(in);
	}
	{
		wire::BundlePkt bp, b;
		bp.hostTick = 77;
		bp.firstTick = 70;
		bp.count = 32;
		bp.slotMask = 0xB;
		bp.nEvents = 8;
		bp.flags = wire::kBundleRelease;
		bp.releaseCount = 1;
		for (int i = 0; i < 32; ++i)
			for (int s = 0; s < 4; ++s)
				if (bp.slotMask & (1 << s)) bp.bundles[i][s].b[1] = (uint8_t)(i + s);
		for (int e = 0; e < 8; ++e) bp.events[e] = wire::Event{ 70u + (uint32_t)e, 1, 3, (uint16_t)e };
		CHECK(roundtrip(bp, b) && b.bundles[31][3].b[1] == 34 && b.bundles[0][2] == Input{} && b.events[7].tick == 77, "bundle");
		std::vector<uint8_t> d;
		wire::encode(d, 1, bp);
		CHECK(d.size() == 64 + 32 * 3 * 8 + 64 && d.size() <= 1152, "bundle size %zu", d.size());
		keep(bp);
	}
	// Bad cases are rejected.
	{
		uint8_t junk[3] = { 0xB5, 1, 32 };
		CHECK(wire::decode_any(junk, 3) == wire::Result::Malformed, "short");
		std::vector<uint8_t> d = corpus.back();
		d[0] = 0;
		CHECK(wire::decode_any(d.data(), d.size()) == wire::Result::Malformed, "magic");
		d = corpus.back();
		d[2] = 99;
		CHECK(wire::decode_any(d.data(), d.size()) == wire::Result::Invalid, "type");
		d = corpus.back();
		d.pop_back();
		CHECK(wire::decode_any(d.data(), d.size()) == wire::Result::Malformed, "truncated bundle");
		wire::InputPkt sp;
		sp.slot = kNoSlot;
		sp.count = 1;
		sp.firstTag = 5;
		std::vector<uint8_t> e;
		wire::encode(e, 1, sp);
		CHECK(wire::decode_any(e.data(), e.size()) == wire::Result::Invalid, "spectator inputs");
	}
	// Fuzz: random and mutated datagrams never crash (ASan/UBSan in this target) and never decode
	// to a different datagram on re-encode.
	size_t accepted = 0;
	for (int i = 0; i < 200000; ++i) {
		std::vector<uint8_t> d;
		if (i % 4 == 0) {
			d.resize(rnd() % 1300);
			for (auto& x : d) x = (uint8_t)rnd();
			if (d.size() >= 2 && (rnd() & 1)) d[0] = wire::kMagic, d[1] = wire::kVersion;
		} else {
			d = corpus[rnd() % corpus.size()];
			const int edits = 1 + (int)(rnd() % 4);
			for (int k = 0; k < edits && !d.empty(); ++k) {
				switch (rnd() % 4) {
				case 0: d[rnd() % d.size()] = (uint8_t)rnd(); break;
				case 1: d.resize(rnd() % (d.size() + 1)); break;
				case 2: d.push_back((uint8_t)rnd()); break;
				default: d[rnd() % d.size()] ^= (uint8_t)(1u << (rnd() % 8)); break;
				}
			}
		}
		std::vector<uint8_t> copy(d); // exact-size heap buffer: ASan sees any overread
		if (wire::decode_any(copy.data(), copy.size()) == wire::Result::Ok) ++accepted;
	}
	if (g_verbose) std::fprintf(stderr, "  fuzz: %zu of 200000 mutated datagrams still decode\n", accepted);
	return ok;
}

// ---- delay ---------------------------------------------------------------------------------------

bool case_delay()
{
	bool ok = true;
	struct Row { int64_t pathUs; int d; } rows[] = { { 0, 1 }, { 15000, 1 }, { 58000, 2 }, { 116000, 4 }, { 300000, 8 } };
	for (const Row& r : rows) {
		int wish = 0;
		const int d = delay_for_path(r.pathUs, &wish);
		CHECK(d == r.d, "path %lld us -> d %d (want %d)", (long long)r.pathUs, d, r.d);
	}
	int wish = 0;
	delay_for_path(300000, &wish);
	CHECK(wish == 10, "300 ms wish %d (want 10, capped to 8)", wish);
	DelayPolicy p;
	p.init(2, 0);
	CHECK(p.evaluate(1000000000LL, 116000, 0) == DelayPolicy::Path && p.current() == 4, "increase at once");
	CHECK(p.evaluate(2000000000LL, 20000, 0) == DelayPolicy::None && p.current() == 4, "decrease waits");
	CHECK(p.boundary() == DelayPolicy::Boundary && p.current() == 1, "decrease at the boundary");
	CHECK(p.evaluate(6000000000LL, 20000, 30) == DelayPolicy::Stall && p.current() == 2, "blame adds 1");
	CHECK(p.evaluate(7000000000LL, 20000, 30) == DelayPolicy::None && p.current() == 2, "blame once per 3 s");
	// The synced settings hash matches the launcher's (key=value lines, sorted, FNV-1a 32).
	CHECK(synced_hash_of("{}") == 2166136261u, "synced hash of {}");
	CHECK(synced_hash_of("{\"b\":1,\"a\":\"x\"}") == synced_hash_of("{\"a\":\"x\",\"b\":1}"), "synced hash order");
	CHECK(content_note("Louie", "a@1.1.0", "a@1.2.0", true, false) == "Louie has different mods: a 1.2.0 (host 1.1.0).", "note: %s",
	      content_note("Louie", "a@1.1.0", "a@1.2.0", true, false).c_str());
	return ok;
}

// ---- sync ----------------------------------------------------------------------------------------

bool case_sync()
{
	bool ok = true;
	// Offset: remote clock = local + 1234567.891 ms (wrapping u32 us), symmetric jitter 8 ms.
	{
		OffsetEstimator est;
		const int64_t off = 1234567891000LL; // ns
		int64_t now       = 0;
		double worst      = 0;
		for (int i = 0; i < 600; ++i) {
			now += 33000000;
			const int64_t d1 = 40000000 + (int64_t)(rnd() % 8001) * 1000, d2 = 40000000 + (int64_t)(rnd() % 8001) * 1000;
			const int64_t hold = (int64_t)(rnd() % 5000) * 1000;
			const int64_t t0 = now, t1 = now + d1 + off, t2 = t1 + hold, t3 = t0 + d1 + hold + d2;
			est.add(t3, us32(t0), us32(t1), us32(t2), us32(t3));
			if (i > 150) { // after 5 s
				const double err = std::fabs((double)us_diff(est.offset_us(), us32(off)) / 1000.0);
				worst            = std::max(worst, err);
			}
		}
		CHECK(worst < 0.5, "offset error %.3f ms with 8 ms jitter (limit 0.5)", worst);
		if (g_verbose) std::fprintf(stderr, "  offset: worst error %.3f ms\n", worst);
	}
	// Phase controller: a 10 ms error, slew applied at most 1 ms per retrace (2 per tick), samples
	// once per tick with the measurement noise of the offset estimate (+-0.4 ms).
	auto run_ctrl = [&](double e0Ms, double noiseMs, double& settleS, double& overshootMs, double& p99Ms) {
		PhaseController pc;
		int64_t grid    = (int64_t)(e0Ms * 1e6); // this client's due minus the host's, ns
		int64_t slew    = 0;
		settleS         = -1;
		overshootMs     = 0;
		std::vector<double> late;
		const int64_t P = kTickNs;
		for (int t = 0; t < 30 * 20; ++t) {
			const int64_t now = (int64_t)t * P;
			for (int r = 0; r < 2; ++r) {
				const int64_t step = std::clamp<int64_t>(slew, -1000000, 1000000);
				slew -= step;
				grid += step;
			}
			const double noise = noiseMs * (((double)(rnd() % 20001) / 10000.0) - 1.0);
			const int64_t e    = grid + (int64_t)(noise * 1e6); // not wrapped (client_core.cpp)
			pc.add(now, e, slew);
			int64_t ns = 0;
			if (pc.update(now, slew, ns)) slew = ns;
			const double eMs = (double)grid / 1e6;
			if (settleS < 0 && std::fabs(eMs) < 1.0) settleS = (double)now / 1e9;
			if (e0Ms > 0 && eMs < 0) overshootMs = std::max(overshootMs, -eMs);
			if (e0Ms < 0 && eMs > 0) overshootMs = std::max(overshootMs, eMs);
			if (now > 3000000000LL) late.push_back(std::fabs(eMs));
		}
		std::sort(late.begin(), late.end());
		p99Ms = late.empty() ? 0 : late[(size_t)(0.99 * (double)(late.size() - 1))];
	};
	double settle, over, p99;
	run_ctrl(10.0, 0.4, settle, over, p99);
	CHECK(settle >= 0 && settle < 2.0, "10 ms phase error settles in %.2f s (limit 2)", settle);
	CHECK(over < 2.0, "overshoot %.2f ms (limit 20%% = 2 ms)", over);
	CHECK(p99 < 2.0, "|e| p99 %.2f ms", p99);
	if (g_verbose) std::fprintf(stderr, "  phase: settle %.2f s, overshoot %.2f ms, p99 %.2f ms\n", settle, over, p99);
	run_ctrl(-10.0, 0.4, settle, over, p99);
	CHECK(settle >= 0 && settle < 2.0 && over < 2.0, "-10 ms: settle %.2f s overshoot %.2f ms", settle, over);
	// A whole-tick lag after a long tick on one peer (1.5 ticks): corrected, not hidden by a wrap.
	run_ctrl(50.05, 0.4, settle, over, p99);
	CHECK(settle >= 0 && settle < 3.0 && over < 2.0 && p99 < 2.0, "1.5-tick lag: settle %.2f s overshoot %.2f ms p99 %.2f ms", settle, over, p99);
	run_ctrl(-66.7, 0.4, settle, over, p99);
	CHECK(settle >= 0 && settle < 3.0 && over < 2.0 && p99 < 2.0, "2-tick lead: settle %.2f s overshoot %.2f ms p99 %.2f ms", settle, over, p99);
	return ok;
}

// ---- netsim cases --------------------------------------------------------------------------------

mppm::net::Impair wan(uint32_t seed, uint32_t lossPpm = 0)
{
	mppm::net::Impair im;
	im.delayUs  = 46000;
	im.jitterUs = 8000;
	im.lossPpm  = lossPpm;
	im.seed     = seed;
	return im;
}

double rate(const netsim::PeerResult& p) { return p.stats.ticks ? 100.0 * (double)p.stats.stalled / (double)p.stats.ticks : 100.0; }

double p99_of(std::vector<int32_t> v)
{
	if (v.empty()) return -1;
	std::sort(v.begin(), v.end());
	return v[(size_t)(0.99 * (double)(v.size() - 1))] / 1000.0;
}

void print_peers(const netsim::Result& r)
{
	for (size_t i = 0; i < r.peers.size(); ++i) {
		const auto& p = r.peers[i];
		std::fprintf(stderr, "  peer %zu: consumed=%u stalled=%llu/%llu (%.2f%%) max=%.1f ms barriers=%llu (%.0f ms) d=%d phase_p99=%.2f ms end=%d\n", i,
		             p.consumed, (unsigned long long)p.stats.stalled, (unsigned long long)p.stats.ticks, rate(p), p.stats.maxMs,
		             (unsigned long long)p.stats.barriers, p.stats.barrierMs, p.delay, p99_of(p.phaseAbsUs), p.endReason);
	}
}

bool same_streams(const netsim::Result& r, std::string& why, int skip = -1)
{
	for (size_t i = 1; i < r.peers.size(); ++i) {
		if ((int)i == skip) continue;
		if (r.peers[i].streamHash != r.peers[0].streamHash) {
			why = "peer " + std::to_string(i) + " consumed a different stream";
			return false;
		}
	}
	return true;
}

bool lockstep(int players, uint32_t ticks, uint32_t lossPpm, double maxStall, const char* name)
{
	bool ok = true;
	netsim::Options o;
	o.mode  = players == 4 ? Mode::Battle2v2 : Mode::Battle;
	o.ticks = ticks;
	o.verbose = g_verbose && std::getenv("NETSIM_TRACE");
	for (int i = 0; i < players; ++i) {
		netsim::PeerSpec s;
		s.impair        = wan((uint32_t)i + 1, lossPpm);
		s.clockOffsetNs = (int64_t)i * 7777777777LL;
		s.phaseNs       = (int64_t)(i * 9) * 1000000;
		s.loads[0]      = 300000000 + (int64_t)i * 150000000;
		s.loads[5000]   = 200000000 + (int64_t)i * 100000000;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish (%.1f s)", (double)r.endTimeNs / 1e9);
	std::string why;
	CHECK(same_streams(r, why), "%s", why.c_str());
	for (size_t i = 0; i < r.peers.size(); ++i) {
		CHECK(r.peers[i].consumed >= ticks, "peer %zu consumed %u", i, r.peers[i].consumed);
		CHECK(rate(r.peers[i]) < maxStall, "peer %zu stall rate %.2f%% (limit %.1f%%)", i, rate(r.peers[i]), maxStall);
		if (i > 0 && lossPpm == 0 && !o.peers[i].spectator) {
			const double p = p99_of(r.peers[i].phaseAbsUs);
			CHECK(p >= 0 && p < 2.0, "peer %zu phase |e| p99 %.2f ms (limit 2)", i, p);
		}
	}
	// d as the formula says: host = ceil((54 + 3) / P) = 2, clients = ceil((54 + 54 + 3) / P) = 4.
	if (lossPpm == 0) {
		CHECK(r.peers[0].delay == 2, "host d %d (want 2)", r.peers[0].delay);
		for (size_t i = 1; i < r.peers.size(); ++i) CHECK(r.peers[i].delay == 4, "peer %zu d %d (want 4)", i, r.peers[i].delay);
	}
	(void)name;
	return ok;
}

bool case_barrier()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 6000;
	int64_t loads0[3] = { 300000000, 2300000000LL, 1100000000 };
	int64_t loads1[3] = { 2000000000LL, 100000000, 900000000 };
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec s;
		s.impair        = wan((uint32_t)i + 11);
		s.clockOffsetNs = -(int64_t)i * 123456789012LL;
		s.phaseNs       = (int64_t)i * 5000000;
		s.spectator     = i == 2;
		s.loads[0]      = loads0[i];
		s.loads[5000]   = loads1[i];
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	std::string why;
	CHECK(same_streams(r, why), "%s", why.c_str());
	for (int b = 0; b < 2; ++b) {
		int64_t lo = INT64_MAX, hi = INT64_MIN;
		for (size_t i = 0; i < r.peers.size(); ++i) {
			const auto& p = r.peers[i];
			CHECK(p.releaseRun.size() >= 2, "peer %zu saw %zu releases", i, p.releaseRun.size());
			if (p.releaseRun.size() < 2) continue;
			CHECK(p.releaseTicks[b] == (b == 0 ? 1u : 5001u), "peer %zu released tick %u", i, p.releaseTicks[b]);
			lo = std::min(lo, p.releaseRun[b]);
			hi = std::max(hi, p.releaseRun[b]);
		}
		if (lo != INT64_MAX) {
			CHECK(hi - lo < 1000000, "barrier %d: first ticks after the release %.3f ms apart (limit 1)", b, (double)(hi - lo) / 1e6);
			if (g_verbose) std::fprintf(stderr, "  barrier %d: B+1 ran within %.3f ms\n", b, (double)(hi - lo) / 1e6);
		}
	}
	return ok;
}

bool case_drop()
{
	bool ok = true;
	netsim::Options o;
	o.mode  = Mode::Battle2v2;
	o.ticks = 5000;
	for (int i = 0; i < 4; ++i) {
		netsim::PeerSpec s;
		s.impair   = wan((uint32_t)i + 21);
		s.loads[0] = 200000000;
		if (i == 3) s.vanishAt = 3000;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	std::string why;
	CHECK(same_streams(r, why, 3), "%s", why.c_str());
	uint32_t e = 0;
	for (int i = 0; i < 3; ++i) {
		const auto& p = r.peers[i];
		CHECK(p.consumed >= 5000, "peer %d consumed %u", i, p.consumed);
		auto it = p.drops.find(3);
		CHECK(it != p.drops.end(), "peer %d saw no drop of seat 3", i);
		if (it == p.drops.end()) continue;
		if (e == 0) e = it->second;
		CHECK(it->second == e, "peer %d drop tick %u, peer 0 %u", i, it->second, e);
		CHECK(p.neutralFromDrop, "peer %d: seat 3 not neutral from E", i);
	}
	CHECK(e > 3000 && e < 3020, "E = %u", e);
	if (g_verbose) std::fprintf(stderr, "  drop: seat 3 neutral from tick %u on every endpoint\n", e);
	return ok;
}

// A seat whose game hangs while its net side keeps sending keepalives (fix round 2): mid-match after
// 10 s of holding the host up, at a load barrier 30 s after the host got there (60 s at the start).
bool stuck_run(const char* what, uint32_t freezeAt, int64_t hangLoadAt, int64_t wantNs, const char* wantLog)
{
	bool ok = true;
	netsim::Options o;
	o.mode  = Mode::Battle2v2;
	o.ticks = 2500;
	for (int i = 0; i < 4; ++i) {
		netsim::PeerSpec s;
		s.impair   = wan((uint32_t)i + 91);
		s.loads[0] = 200000000;
		s.loads[1500] = 300000000;
		if (i == 2) {
			s.freezeAt   = freezeAt;
			s.hangLoadAt = hangLoadAt;
		}
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "%s: did not finish", what);
	std::string why;
	CHECK(same_streams(r, why, 2), "%s: %s", what, why.c_str());
	uint32_t e = 0;
	for (int i : { 0, 1, 3 }) {
		const auto& p = r.peers[i];
		CHECK(p.consumed >= 2500, "%s: peer %d consumed %u", what, i, p.consumed);
		auto it = p.drops.find(2);
		CHECK(it != p.drops.end(), "%s: peer %d saw no drop of seat 2", what, i);
		if (it == p.drops.end()) continue;
		if (e == 0) e = it->second;
		CHECK(it->second == e, "%s: peer %d drop tick %u, peer 0 %u", what, i, it->second, e);
		CHECK(p.neutralFromDrop, "%s: peer %d: seat 2 not neutral from E", what, i);
	}
	bool logged = false;
	for (const auto& l : r.peers[0].logs) logged = logged || l.find(wantLog) != std::string::npos;
	CHECK(logged, "%s: the host did not log \"%s\"", what, wantLog);
	const double longest = std::max(r.peers[0].stats.maxMs, r.peers[0].stats.barrierMs); // barrierMs: the sum of the barrier waits
	CHECK(longest > wantNs / 1e6 - 1000 && longest < wantNs / 1e6 + 2000, "%s: the host's longest wait %.0f ms (want about %.0f)", what,
	      longest, wantNs / 1e6);
	CHECK(r.peers[2].endReason != kEndNone, "%s: the stuck peer's session did not end", what);
	if (g_verbose) std::fprintf(stderr, "  %s: seat 2 dropped from tick %u, the host waited %.0f ms\n", what, e, longest);
	return ok;
}

bool case_stuck()
{
	bool ok = stuck_run("mid-match", 1000, -1, 10000000000LL, "made no progress for 10 s");
	ok      = stuck_run("load", 0, 1500, 30000000000LL, "did not finish loading within 30 s") && ok;
	ok      = stuck_run("start", 0, 0, 60000000000LL, "did not finish loading within 60 s") && ok;
	return ok;
}

bool case_desync()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 2000;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec s;
		s.impair    = wan((uint32_t)i + 31);
		s.spectator = i == 2;
		s.loads[0]  = 200000000;
		if (i == 1) s.desyncAt = 900;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	for (size_t i = 0; i < r.peers.size(); ++i) {
		CHECK(r.peers[i].endReason == kEndDesync, "peer %zu end reason %d", i, r.peers[i].endReason);
		CHECK(r.peers[i].desyncTick == 900, "peer %zu desync tick %u", i, r.peers[i].desyncTick);
		CHECK(r.peers[i].consumed < 960, "peer %zu ran on to tick %u", i, r.peers[i].consumed);
	}
	return ok;
}

bool case_spectator()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 6000;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec s;
		s.impair    = wan((uint32_t)i + 41);
		s.spectator = i == 2;
		if (i == 2) s.impair.delayUs = 150000; // a slow spectator never delays the players
		s.loads[0] = i == 2 ? 3000000000LL : 200000000;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	std::string why;
	CHECK(same_streams(r, why), "%s", why.c_str());
	CHECK(rate(r.peers[0]) < 1.0 && rate(r.peers[1]) < 1.0, "players stall %.2f%% / %.2f%%", rate(r.peers[0]), rate(r.peers[1]));
	CHECK(r.peers[0].stats.barrierMs < 1500, "the host waited %.0f ms at the barrier for the spectator", r.peers[0].stats.barrierMs);
	return ok;
}

bool case_delaychange()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 9000;
	for (int i = 0; i < 2; ++i) {
		netsim::PeerSpec s;
		s.impair.delayUs  = 10000;
		s.impair.jitterUs = 1000;
		s.impair.seed     = (uint32_t)i + 51;
		s.loads[0]        = 200000000;
		s.loads[6000]     = 200000000;
		o.peers.push_back(s);
	}
	// At 60 s the path goes from about 20 to 120 ms (60 each way), at 120 s back.
	o.impairPeer        = 1;
	o.impairChangeAt    = 60000000000LL;
	o.impairChange2At   = 120000000000LL;
	o.impairNew.delayUs = 60000;
	o.impairNew.jitterUs = 1000;
	o.impairNew.seed    = 77;
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	std::string why;
	CHECK(same_streams(r, why), "%s", why.c_str());
	const auto& log = r.peers[1].delayLog;
	int before = 0, raisedAt = -1;
	int64_t raisedT = 0, loweredT = 0;
	for (const auto& [t, d] : log) {
		if (t < o.impairChangeAt) before = d;
		else if (raisedAt < 0 && d > before) {
			raisedAt = d;
			raisedT  = t;
		} else if (raisedAt > 0 && d < raisedAt && !loweredT) {
			loweredT = t;
		}
	}
	CHECK(before == 1, "d before %d (want 1)", before);
	CHECK(raisedAt >= 4 && raisedT - o.impairChangeAt < 1500000000LL, "d raised to %d after %.2f s (want >= 4 within 1 s)", raisedAt,
	      (double)(raisedT - o.impairChangeAt) / 1e9);
	// The decrease waits for the barrier at tick 6000 (about 200 s), well after the path went back.
	CHECK(loweredT == 0 || loweredT > 190000000000LL, "d lowered at %.1f s, before the barrier", (double)loweredT / 1e9);
	CHECK(loweredT != 0, "d never went back down after the barrier");
	CHECK(r.peers[1].stats.maxMs < 200, "longest stall %.1f ms (a gap in the tags would stall for good)", r.peers[1].stats.maxMs);
	if (g_verbose) std::fprintf(stderr, "  delaychange: d %d -> %d after %.2f s, lowered at %.1f s\n", before, raisedAt,
	                            (double)(raisedT - o.impairChangeAt) / 1e9, (double)loweredT / 1e9);
	return ok;
}

bool case_leave()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 2700;
	for (int i = 0; i < 2; ++i) {
		netsim::PeerSpec s;
		s.impair.delayUs  = 1000;
		s.impair.jitterUs = 500;
		s.impair.seed     = (uint32_t)i + 71;
		s.loads[0]        = 100000000;
		if (i == 1) s.leaveAt = 900;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	CHECK(r.peers[0].consumed >= 2700, "host consumed %u", r.peers[0].consumed);
	auto it = r.peers[0].drops.find(1);
	CHECK(it != r.peers[0].drops.end(), "no drop of seat 1 at the host");
	if (it != r.peers[0].drops.end()) CHECK(it->second >= 900 && it->second <= 902, "E = %u (want within 2 of 900)", it->second);
	CHECK(r.peers[0].stats.maxMs < 1000, "host's longest stall %.1f ms", r.peers[0].stats.maxMs);
	CHECK(r.peers[1].endReason == kEndLeft, "the leaver ended with %d", r.peers[1].endReason);
	// A BYE "done" before the host's own last tick (a smaller --exit-after-ticks): the host drops the
	// seat after 1 s of waiting for it instead of waiting for good.
	o.peers[1].leaveAt = 0;
	o.peers[1].doneAt  = 900;
	r                  = netsim::run(o);
	CHECK(r.finished, "done early: did not finish");
	CHECK(r.peers[0].consumed >= 2700, "done early: host consumed %u", r.peers[0].consumed);
	CHECK(r.peers[0].drops.count(1) == 1, "done early: no drop of seat 1 at the host");
	CHECK(r.peers[0].stats.maxMs > 900 && r.peers[0].stats.maxMs < 1500, "done early: host's longest stall %.1f ms (want about 1 s)",
	      r.peers[0].stats.maxMs);
	// The crash screen on the client: its net thread says BYE (error) at once, the host goes on.
	o.peers[1].doneAt  = 0;
	o.peers[1].crashAt = 900;
	r                  = netsim::run(o);
	CHECK(r.finished, "crash: did not finish");
	CHECK(r.peers[0].consumed >= 2700, "crash: host consumed %u", r.peers[0].consumed);
	it = r.peers[0].drops.find(1);
	CHECK(it != r.peers[0].drops.end(), "crash: no drop of seat 1 at the host");
	if (it != r.peers[0].drops.end()) CHECK(it->second >= 900 && it->second <= 902, "crash: E = %u (want within 2 of 900)", it->second);
	CHECK(r.peers[0].stats.maxMs < 1000, "crash: host's longest stall %.1f ms", r.peers[0].stats.maxMs);
	return ok;
}

bool case_chunk()
{
	bool ok = true;
	netsim::Options o;
	o.ticks     = 300;
	o.longStart = true;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec s;
		s.impair    = wan((uint32_t)i + 81);
		s.spectator = i == 2;
		s.loads[0]  = 100000000;
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	CHECK(r.finished, "did not finish");
	for (size_t i = 0; i < r.peers.size(); ++i) {
		CHECK(r.peers[i].syncedLen == r.peers[0].syncedLen && r.peers[i].syncedLen > 1900, "peer %zu got a Synced JSON of %zu bytes", i,
		      r.peers[i].syncedLen);
		CHECK(r.peers[i].mods == r.peers[0].mods && r.peers[i].mods.size() > 600, "peer %zu mods differ", i);
		CHECK(r.peers[i].consumed >= 300, "peer %zu consumed %u", i, r.peers[i].consumed);
	}
	return ok;
}

bool case_lobby()
{
	bool ok = true;
	netsim::Options o;
	o.ticks     = 100;
	o.maxTimeNs = 10000000000LL;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec s;
		s.impair = wan((uint32_t)i + 61);
		if (i == 1) s.contentXor = 0x5A;  // different content: stays in the room, cannot be ready
		if (i == 2) s.wrongSecret = true; // refused at Hello
		o.peers.push_back(s);
	}
	netsim::Result r = netsim::run(o);
	CHECK(!r.hostStarted, "the host started with a mismatched member");
	bool noted = false;
	for (const auto& n : r.notes)
		if (n == "P1 has different game files.") noted = true;
	CHECK(noted, "no content note for P1");
	CHECK(r.peers[2].endReason == kEndRefused, "a wrong secret ended with %d (want refused)", r.peers[2].endReason);
	CHECK(r.peers[1].endReason == kEndNone, "the mismatched member was sent away (%d)", r.peers[1].endReason);
	return ok;
}

// Section 10: a connection that claims a member's id is bound to that member only by a Hello with its
// join secret, so another peer cannot take a welcomed member's seat (or its pre-Hello slot).
struct FakeEnv : CoreEnv {
	std::vector<uint32_t> disconnected;
	GameLink gl;
	bool send(uint32_t, bool, const std::vector<uint8_t>&, bool) override { return true; }
	void disconnect(uint32_t peer, uint32_t) override { disconnected.push_back(peer); }
	void to_game(const N2G&) override { }
	void note(const CoreNote&) override { }
	int64_t slew_get() override { return 0; }
	void slew_set(int64_t) override { }
	bool tick_shift(int) override { return false; }
	void log(const std::string& line) override
	{
		if (g_verbose) std::printf("  host: %s\n", line.c_str());
	}
	GameLink& link() override { return gl; }
	bool dropped(uint32_t peer) const { return std::find(disconnected.begin(), disconnected.end(), peer) != disconnected.end(); }
};

bool case_hijack()
{
	bool ok = true;
	FakeEnv env;
	HostParams hp;
	HostCore h(env, hp);
	const int64_t s = 1000000000LL;
	auto hello = [&](uint32_t peer, uint32_t member, uint64_t secret, int64_t now) {
		wire::Hello m;
		m.member = member;
		m.secret = secret;
		m.name   = "P";
		std::vector<uint8_t> d;
		wire::encode(d, 0, m);
		h.on_datagram(peer, d.data(), d.size(), now, now);
	};
	auto req = [&](uint32_t peer, uint8_t what, int8_t value, int64_t now) {
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Req{ what, value });
		h.on_datagram(peer, d.data(), d.size(), now, now);
	};
	auto member_of = [&](uint32_t id) {
		wire::LobbyMember out;
		out.id = 0xFFFFFFFFu;
		for (const auto& m : h.lobby_view().members)
			if (m.id == id) out = m;
		return out;
	};
	// V (member 2) joins and is welcomed on peer 10.
	h.add_member(2, "V", false, 0xABCDEF, 1 * s);
	h.on_connected(10, 2, 1 * s);
	hello(10, 2, 0xABCDEF, 1 * s);
	CHECK(h.connected_count() == 2, "V was not welcomed (%d connected)", h.connected_count());
	// A claims member 2 on peer 11: V's connection stays, A's requests do nothing.
	h.on_connected(11, 2, 2 * s);
	CHECK(!env.dropped(10), "a connection claiming V's id disconnected V");
	req(11, wire::kReqReady, 1, 2 * s);
	CHECK(!(member_of(2).flags & wire::kMemReady), "an unverified connection set V ready");
	hello(11, 2, 0x1234, 2 * s);
	CHECK(env.dropped(11), "a wrong secret was not refused");
	CHECK(!env.dropped(10) && h.connected_count() == 2, "V lost its seat to a wrong secret");
	req(10, wire::kReqReady, 1, 3 * s);
	CHECK(member_of(2).flags & wire::kMemReady, "V's own ready was not taken");
	// A peer that never says Hello is dropped after 5 s.
	h.on_connected(12, 2, 3 * s);
	h.tick(9 * s);
	CHECK(env.dropped(12), "a silent connection was kept past 5 s");
	// Before V's first Hello, a claim of the same id does not displace the real connection either.
	h.add_member(3, "W", false, 0x777, 10 * s);
	h.on_connected(20, 3, 10 * s);
	h.on_connected(21, 3, 10 * s);
	hello(21, 3, 0x1, 10 * s);
	hello(20, 3, 0x777, 10 * s);
	CHECK(env.dropped(21) && !env.dropped(20), "the wrong Hello won the member's connection");
	CHECK(h.connected_count() == 3, "W was not welcomed (%d connected)", h.connected_count());
	// A connection for a member the lobby has not admitted yet is let go at once (section 10 caps);
	// after MemberJoined, a wrong Hello is refused and the right one binds.
	h.on_connected(30, 4, 11 * s);
	CHECK(env.dropped(30), "a connection for a member the lobby has not admitted was kept");
	h.add_member(4, "X", true, 0x999, 12 * s);
	h.on_connected(32, 4, 12 * s);
	h.on_connected(33, 4, 12 * s);
	hello(33, 4, 0x2, 12 * s);
	hello(32, 4, 0x999, 12 * s);
	CHECK(env.dropped(33) && !env.dropped(32), "the wrong Hello won X's connection");
	CHECK(h.connected_count() == 4, "X was not welcomed after MemberJoined (%d connected)", h.connected_count());
	return ok;
}

// A refused or no-op REQ slot must not bump the lobby: a client with a --slot wish repeats its REQ on
// every LOBBY, so a broadcast per refusal was a REQ/LOBBY feedback loop (fix round 3).
bool case_reqslot()
{
	bool ok = true;
	FakeEnv env;
	HostParams hp;
	HostCore h(env, hp);
	const int64_t s = 1000000000LL;
	auto hello = [&](uint32_t peer, uint32_t member, uint64_t secret, int64_t now) {
		wire::Hello m;
		m.member = member;
		m.secret = secret;
		m.name   = "P";
		std::vector<uint8_t> d;
		wire::encode(d, 0, m);
		h.on_datagram(peer, d.data(), d.size(), now, now);
	};
	auto req = [&](uint32_t peer, int8_t value, int64_t now) {
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Req{ wire::kReqSlot, value });
		h.on_datagram(peer, d.data(), d.size(), now, now);
		return h.lobby_view().generation;
	};
	auto slot_of = [&](uint32_t id) {
		for (const auto& m : h.lobby_view().members)
			if (m.id == id) return (int)m.slot;
		return -2;
	};
	h.add_member(2, "V", false, 0xABCDEF, 1 * s);
	h.on_connected(10, 2, 1 * s);
	hello(10, 2, 0xABCDEF, 1 * s);
	h.add_member(3, "W", false, 0x777, 1 * s);
	h.on_connected(11, 3, 1 * s);
	hello(11, 3, 0x777, 1 * s);
	CHECK(h.connected_count() == 3, "%d connected (want 3)", h.connected_count());
	const int vs = slot_of(2);
	CHECK(vs == 1 && slot_of(3) == (int)kNoSlot, "V at %d, W at %d (want 1 and no seat)", vs, slot_of(3));
	uint32_t g = h.lobby_view().generation;
	CHECK(req(10, 0, 2 * s) == g, "a refused REQ for the host's seat bumped the lobby");
	CHECK(req(10, 5, 2 * s) == g, "a REQ for a seat out of range bumped the lobby");
	CHECK(req(10, 1, 2 * s) == g, "a REQ for V's own seat bumped the lobby");
	CHECK(req(11, -1, 2 * s) == g, "standing up while unseated bumped the lobby");
	// V picks seat 1 for itself (no move: same seat), then W's REQ for it: V seated automatically, so a swap.
	uint32_t g2 = req(11, 1, 3 * s);
	CHECK(g2 != g && slot_of(3) == 1 && slot_of(2) == (int)kNoSlot, "W did not take V's automatic seat (W %d, V %d)", slot_of(3),
	      slot_of(2));
	// W chose seat 1: V's repeated REQs for it are refused without a broadcast.
	for (int i = 0; i < 100; ++i)
		CHECK(req(10, 1, 4 * s) == g2, "a refused REQ for a chosen seat bumped the lobby");
	CHECK(slot_of(2) == (int)kNoSlot, "V took W's chosen seat");
	CHECK(req(11, -1, 5 * s) != g2 && slot_of(3) == (int)kNoSlot, "W could not stand up");
	// A reconnect with the member's secret from other game files: the hashes are checked again.
	auto flags_of = [&](uint32_t id) {
		for (const auto& m : h.lobby_view().members)
			if (m.id == id) return (int)m.flags;
		return 0;
	};
	{
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Req{ wire::kReqReady, 1 });
		h.on_datagram(10, d.data(), d.size(), 6 * s, 6 * s);
	}
	req(10, 1, 6 * s);
	{
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Req{ wire::kReqReady, 1 });
		h.on_datagram(10, d.data(), d.size(), 6 * s, 6 * s);
	}
	CHECK((flags_of(2) & wire::kMemReady) && (flags_of(2) & wire::kMemContentOk), "V is not ready with matching content");
	h.on_connected(13, 2, 7 * s);
	{
		wire::Hello m;
		m.member     = 2;
		m.secret     = 0xABCDEF;
		m.name       = "P";
		m.content[0] = 0x5A;
		std::vector<uint8_t> d;
		wire::encode(d, 0, m);
		h.on_datagram(13, d.data(), d.size(), 7 * s, 7 * s);
	}
	CHECK(env.dropped(10), "the old connection was kept after the reconnect");
	CHECK(!(flags_of(2) & wire::kMemContentOk) && !(flags_of(2) & wire::kMemReady),
	      "a reconnect with other content kept V's content check and ready (flags %x)", flags_of(2));
	return ok;
}

// Shared helpers for the FakeEnv host cases.
struct HostRig {
	FakeEnv env;
	HostCore h;
	explicit HostRig(const HostParams& hp) : h(env, hp) { }
	void hello(uint32_t peer, uint32_t member, uint64_t secret, int64_t now, const std::string& name = "P")
	{
		wire::Hello m;
		m.member = member;
		m.secret = secret;
		m.name   = name;
		std::vector<uint8_t> d;
		wire::encode(d, 0, m);
		h.on_datagram(peer, d.data(), d.size(), now, now);
	}
	void join(uint32_t peer, uint32_t member, int64_t now, const std::string& name = "P", bool spectator = false)
	{
		h.add_member(member, name, spectator, 0x1000u + member, now);
		h.on_connected(peer, member, now);
		hello(peer, member, 0x1000u + member, now);
	}
	void req(uint32_t peer, uint8_t what, int8_t value, int64_t now)
	{
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Req{ what, value });
		h.on_datagram(peer, d.data(), d.size(), now, now);
	}
	wire::LobbyMember member(uint32_t id) const
	{
		wire::LobbyMember out;
		out.id   = 0xFFFFFFFFu;
		out.slot = 0xFE;
		for (const auto& m : h.lobby_view().members)
			if (m.id == id) out = m;
		return out;
	}
	// Every seat in [0, seats) holds at most one member, no member sits outside it.
	bool unique_seats(std::string& why) const
	{
		const wire::Lobby l = h.lobby_view();
		int count[kMaxSeats] = {};
		for (const auto& m : l.members) {
			if (m.slot == kNoSlot) continue;
			if (m.slot >= l.seats) {
				why = "member " + std::to_string(m.id) + " sits in seat " + std::to_string(m.slot) + " of " + std::to_string(l.seats);
				return false;
			}
			if (++count[m.slot] > 1) {
				why = "two members in seat " + std::to_string(m.slot);
				return false;
			}
		}
		return true;
	}
};

// Fix-min item 1: 2v2 -> a 2-seat mode with the host in seat 2 or 3 kept two members in seat 0 (one of
// them a silent spectator at START). Every member keeps a unique seat or watches.
bool case_modeswitch()
{
	bool ok = true;
	const int64_t s = 1000000000LL;
	std::string why;
	{
		// Full room: host in seat 2, V 0, W 1, X 3 (all seated automatically). To Battle: the host takes
		// W's seat 1 (the highest automatic seat below 2), W and X watch.
		HostParams hp;
		hp.mode     = Mode::Battle2v2;
		hp.hostSlot = 2;
		HostRig r(hp);
		r.join(10, 2, 1 * s, "V");
		r.join(11, 3, 1 * s, "W");
		r.join(12, 4, 1 * s, "X");
		CHECK(r.member(2).slot == 0 && r.member(3).slot == 1 && r.member(4).slot == 3, "2v2 seats V %d W %d X %d", r.member(2).slot,
		      r.member(3).slot, r.member(4).slot);
		for (uint32_t peer : { 10u, 11u, 12u }) r.req(peer, wire::kReqReady, 1, 2 * s);
		r.h.set_ready(true);
		r.h.set_config(Mode::Battle, 0, 0, 0);
		CHECK(r.unique_seats(why), "full room: %s", why.c_str());
		CHECK(r.h.local_seat() == 1 && r.member(0).slot == 1, "full room: host in seat %d", r.h.local_seat());
		CHECK(r.member(2).slot == 0, "full room: V moved from seat 0 to %d", r.member(2).slot);
		CHECK(r.member(3).slot == kNoSlot && r.member(4).slot == kNoSlot, "full room: W %d, X %d (want both watching)", r.member(3).slot,
		      r.member(4).slot);
		CHECK(!(r.member(3).flags & wire::kMemReady) && !(r.member(4).flags & wire::kMemReady), "full room: a watcher is still ready");
		// START seats exactly what the lobby showed.
		r.req(10, wire::kReqReady, 1, 3 * s);
		r.h.set_ready(true);
		CHECK(r.h.start(4 * s), "full room: could not start (%s)", r.h.status_line().c_str());
		CHECK(r.h.config().slotMember[0] == 2 && r.h.config().slotMember[1] == 0, "full room: START seats %u,%u (want 2,0)",
		      r.h.config().slotMember[0], r.h.config().slotMember[1]);
	}
	{
		// Host in seat 3, V in 0: the host moves to the free seat 1, V stays.
		HostParams hp;
		hp.mode     = Mode::Battle2v2;
		hp.hostSlot = 3;
		HostRig r(hp);
		r.join(10, 2, 1 * s, "V");
		r.h.set_config(Mode::Challenge, 0, 0, 0);
		CHECK(r.unique_seats(why), "one member: %s", why.c_str());
		CHECK(r.member(0).slot == 1 && r.member(2).slot == 0, "one member: host %d V %d (want 1 and 0)", r.member(0).slot, r.member(2).slot);
	}
	{
		// Host in seat 2, V 0, W chose seat 3, nobody in 1: the host takes seat 1, W moves to... nothing
		// is free, so W watches; V keeps seat 0.
		HostParams hp;
		hp.mode     = Mode::Battle2v2;
		hp.hostSlot = 2;
		HostRig r(hp);
		r.join(10, 2, 1 * s, "V");
		r.join(11, 3, 1 * s, "W");
		r.req(11, wire::kReqSlot, 3, 2 * s);
		CHECK(r.member(3).slot == 3, "W did not take seat 3 (%d)", r.member(3).slot);
		r.h.set_config(Mode::Battle, 0, 0, 0);
		CHECK(r.unique_seats(why), "chosen seat: %s", why.c_str());
		CHECK(r.member(0).slot == 1 && r.member(2).slot == 0 && r.member(3).slot == kNoSlot, "chosen seat: host %d V %d W %d",
		      r.member(0).slot, r.member(2).slot, r.member(3).slot);
		// Back to 2v2: W can take a seat again.
		r.h.set_config(Mode::Battle2v2, 0, 0, 0);
		r.req(11, wire::kReqSlot, 2, 3 * s);
		CHECK(r.unique_seats(why) && r.member(3).slot == 2, "back to 2v2: W at %d (%s)", r.member(3).slot, why.c_str());
	}
	return ok;
}

// Fix-min item 2: after START the client keeps playing while its P2P link to the host is alive, even
// when the lobby server says "closed host_left"; it ends once that link is silent too.
bool case_hostleft()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 1500;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec sp;
		sp.impair    = wan((uint32_t)i + 101);
		sp.spectator = i == 2;
		sp.loads[0]  = 100000000;
		if (i > 0) sp.lobbyHostLeftAt = 300;
		o.peers.push_back(sp);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "lobby only: did not finish");
	for (size_t i = 0; i < r.peers.size(); ++i) {
		CHECK(r.peers[i].consumed >= o.ticks, "lobby only: peer %zu stopped at tick %u", i, r.peers[i].consumed);
		if (i == 0) continue;
		bool logged = false;
		for (const auto& l : r.peers[i].logs) logged = logged || l.find("lobby says the host left") != std::string::npos;
		CHECK(logged, "lobby only: peer %zu did not log the lobby's host_left", i);
	}
	std::string why;
	CHECK(same_streams(r, why), "lobby only: %s", why.c_str());
	// The host really goes (its game and link vanish) at the same time: the clients end after the grace.
	o.peers[0].vanishAt       = 300;
	o.peers[0].vanishNoticeNs = 5000000000LL; // the transports notice only after 5 s
	r                         = netsim::run(o);
	for (size_t i = 1; i < r.peers.size(); ++i) {
		CHECK(r.peers[i].endReason == kEndHostLeft, "host gone: peer %zu end reason %d", i, r.peers[i].endReason);
		CHECK(r.peers[i].consumed < 400, "host gone: peer %zu ran on to tick %u", i, r.peers[i].consumed);
		bool silent = false;
		for (const auto& l : r.peers[i].logs) silent = silent || l.find("the host's connection is silent too") != std::string::npos;
		CHECK(silent, "host gone: peer %zu did not end by the grace rule", i);
	}
	return ok;
}

// Fix-min item 3: strings are cut at UTF-8 code point boundaries; names are bounded like the server's
// sanitize_name (24 code points); a Hello name cut mid-character by an older encoder is kept.
bool case_utf8()
{
	bool ok = true;
	const std::string e = "\xC3\xA9";          // é, 2 bytes
	const std::string g = "\xF0\x9F\x8D\x84";  // 🍄, 4 bytes
	CHECK(wire::utf8_cut("abc", 2) == "ab", "ascii cut");
	CHECK(wire::utf8_cut("a" + e, 2) == "a", "cut before a 2-byte character");
	CHECK(wire::utf8_cut(g + g, 7) == g, "cut inside a 4-byte character");
	CHECK(wire::utf8_cut(e + e + e, 100, 2) == e + e, "code point cap");
	std::string many;
	for (int i = 0; i < 30; ++i) many += e;
	std::string mush;
	for (int i = 0; i < 30; ++i) mush += g;
	CHECK(wire::clean_name(many) == many.substr(0, 48), "clean_name keeps 24 code points");
	CHECK(wire::clean_name("  Lou" + e + "  ") == "Lou" + e, "clean_name trims");
	CHECK(wire::clean_name("a" + e.substr(0, 1)) == "a", "clean_name drops a cut last character");
	CHECK(wire::clean_name("a\xC3" "b").empty(), "clean_name refuses invalid UTF-8 inside");
	CHECK(wire::clean_name("a\x01\x7F" "b") == "ab", "clean_name drops control characters");
	// Hello and LOBBY: long names keep 24 whole characters, notes whole characters within 120 bytes.
	{
		wire::Hello h, b;
		h.name = mush;
		std::vector<uint8_t> d;
		wire::encode(d, 0, h);
		CHECK(wire::decode(d.data(), d.size(), b) == wire::Result::Ok, "a long Hello name is refused");
		CHECK(b.name == mush.substr(0, 96), "Hello name %zu bytes (want 24 mushrooms = 96)", b.name.size());
	}
	{
		wire::Lobby l, b;
		wire::LobbyMember m;
		m.name = "x" + many;
		m.note.assign(119, 'n');
		m.note += e + e;
		l.members.push_back(m);
		std::vector<uint8_t> d;
		wire::encode(d, 0, l);
		CHECK(wire::decode(d.data(), d.size(), b) == wire::Result::Ok && b.members.size() == 1, "lobby with long strings refused");
		if (b.members.size() == 1) {
			CHECK(b.members[0].name == ("x" + many).substr(0, 1 + 23 * 2), "lobby name %zu bytes", b.members[0].name.size());
			CHECK(b.members[0].note == std::string(119, 'n'), "lobby note cut inside a character (%zu bytes)", b.members[0].note.size());
		}
	}
	// The mods note of content_note is cut at a character boundary too.
	{
		std::string mods;
		for (int i = 0; i < 20; ++i) mods += (i ? "," : "") + std::string("m") + e + std::to_string(i) + "@1";
		const std::string n = content_note(many, "", mods, true, false);
		CHECK(n.size() <= wire::kMaxNote && wire::clean_name(n.substr(0, n.size() - 3)) != "", "note: %zu bytes, valid %d", n.size(),
		      (int)!wire::clean_name(n.substr(0, n.size() - 3)).empty());
	}
	// The host bounds its own name; a Hello name cut mid-character is kept (without the cut character).
	{
		HostParams hp;
		hp.name = "  " + many + "!";
		HostRig r(hp);
		CHECK(r.member(0).name == many.substr(0, 48), "host name %zu bytes (want 24 characters = 48)", r.member(0).name.size());
		const int64_t s = 1000000000LL;
		r.h.add_member(2, "Player", false, 0x1002, 1 * s);
		r.h.on_connected(10, 2, 1 * s);
		// An older client's 24-byte cut: "a" + 11 é + the first byte of the 12th.
		std::string cut = "a";
		for (int i = 0; i < 12; ++i) cut += e;
		cut.resize(24);
		r.hello(10, 2, 0x1002, 1 * s, cut);
		CHECK(r.h.connected_count() == 2, "the member with a cut name was not welcomed");
		CHECK(r.member(2).name == cut.substr(0, 23), "a Hello name cut mid-character became \"%s\"", r.member(2).name.c_str());
	}
	return ok;
}

// Fix-min item 4: unverified connections only for admitted members, at most kMaxPendingPerSource per
// address, kMaxPendingPerMember per member and kMaxPendingTotal in all; kicked members never.
bool case_pendingcap()
{
	bool ok = true;
	const int64_t s = 1000000000LL;
	CHECK(pending_source_key("10.1.2.3:5000") == "10.1.2.3", "direct key");
	CHECK(pending_source_key("127.0.0.1:5000").empty(), "loopback key");
	CHECK(pending_source_key("relay 10.0.0.1:47201") == "relay 10.0.0.1:47201", "relay key");
	CHECK(pending_source_key("mem:peer2") == "mem:peer2", "test transport key");
	HostParams hp;
	hp.mode = Mode::Battle2v2;
	HostRig r(hp);
	for (uint32_t id = 2; id <= 7; ++id) r.h.add_member(id, "M" + std::to_string(id), false, 0x1000u + id, 1 * s);
	// Not admitted: refused at once.
	r.h.on_connected(100, 42, 1 * s, "10.0.0.9");
	CHECK(r.env.dropped(100), "a connection for an unknown member id was kept");
	// One address: two pending, the third is let go.
	r.h.on_connected(101, 2, 1 * s, "10.0.0.5");
	r.h.on_connected(102, 3, 1 * s, "10.0.0.5");
	r.h.on_connected(103, 4, 1 * s, "10.0.0.5");
	CHECK(!r.env.dropped(101) && !r.env.dropped(102) && r.env.dropped(103), "per-address cap (101 %d, 102 %d, 103 %d)",
	      (int)r.env.dropped(101), (int)r.env.dropped(102), (int)r.env.dropped(103));
	// One member: two pending (from anywhere), the third is let go.
	r.h.on_connected(104, 2, 1 * s, "10.0.0.6");
	r.h.on_connected(105, 2, 1 * s, "10.0.0.7");
	CHECK(!r.env.dropped(104) && r.env.dropped(105), "per-member cap");
	// In all: four pending, then the next is let go (8 transport slots stay free enough for real members).
	r.h.on_connected(106, 5, 1 * s, "10.0.0.8");
	r.h.on_connected(107, 6, 1 * s, "10.0.0.10");
	CHECK(!r.env.dropped(106) && r.env.dropped(107), "total cap (106 %d, 107 %d)", (int)r.env.dropped(106), (int)r.env.dropped(107));
	// A real member's Hello frees its pending place; the next member can connect.
	r.hello(102, 3, 0x1003, 2 * s);
	CHECK(r.member(3).flags & wire::kMemConnected, "member 3 was not welcomed");
	r.h.on_connected(108, 6, 2 * s, "10.0.0.10");
	CHECK(!r.env.dropped(108), "a real member could not connect after a place was freed");
	// Silent ones go after 5 s; then a fresh one is taken.
	r.h.tick(8 * s);
	CHECK(r.env.dropped(101) && r.env.dropped(104) && r.env.dropped(106) && r.env.dropped(108), "silent pending connections were kept");
	r.h.on_connected(109, 4, 9 * s, "10.0.0.5");
	CHECK(!r.env.dropped(109), "member 4 could not connect once the address had no pending connection");
	r.hello(109, 4, 0x1004, 9 * s);
	// Loopback is not capped per address.
	r.h.on_connected(110, 5, 10 * s, "");
	r.h.on_connected(111, 6, 10 * s, "");
	r.h.on_connected(112, 7, 10 * s, "");
	CHECK(!r.env.dropped(110) && !r.env.dropped(111) && !r.env.dropped(112), "loopback connections were capped per address");
	r.hello(110, 5, 0x1005, 10 * s);
	r.hello(111, 6, 0x1006, 10 * s);
	// A kicked member: its pending connection goes, no new one is taken, MemberJoined again is ignored.
	r.h.kick(7, 11 * s);
	CHECK(r.env.dropped(112), "a kicked member's pending connection was kept");
	r.h.on_connected(113, 7, 11 * s, "10.0.0.11");
	CHECK(r.env.dropped(113), "a kicked member got a pending connection");
	r.h.add_member(7, "M7", false, 0x1007, 12 * s);
	r.h.on_connected(114, 7, 12 * s, "10.0.0.11");
	CHECK(r.env.dropped(114) && r.member(7).id == 0xFFFFFFFFu, "a kicked member came back through MemberJoined");
	// A member the lobby says was banned is refused too.
	r.h.remove_member(6, wire::kDropKicked, 13 * s);
	r.h.on_connected(115, 6, 13 * s, "10.0.0.12");
	CHECK(r.env.dropped(115), "a banned member got a pending connection");
	return ok;
}

// Fix-min item 6: the host's Remove. In the lobby the member goes and its seat opens; after START (2v2,
// seat 3 removed at tick 1500) the seat is dropped with one E everywhere, the removed peer ends with
// kEndKicked and the others play on.
bool case_kick()
{
	bool ok = true;
	const int64_t s = 1000000000LL;
	{
		HostParams hp;
		HostRig r(hp);
		r.join(10, 2, 1 * s, "V");
		CHECK(r.member(2).slot == 1, "V not seated (%d)", r.member(2).slot);
		const uint32_t g = r.h.lobby_view().generation;
		r.h.kick(2, 2 * s);
		CHECK(r.env.dropped(10), "the removed member's connection was kept");
		CHECK(r.member(2).id == 0xFFFFFFFFu, "the removed member is still in the lobby");
		CHECK(r.h.seat_member(1) == 0xFFFFFFFFu && r.h.lobby_view().generation != g, "seat 1 did not open");
		r.join(11, 3, 3 * s, "W");
		CHECK(r.member(3).slot == 1, "the next member did not get the open seat (%d)", r.member(3).slot);
	}
	netsim::Options o;
	o.mode       = Mode::Battle2v2;
	o.ticks      = 3000;
	o.kickAt     = 1500;
	o.kickMember = 3;
	o.verbose    = g_verbose && std::getenv("NETSIM_TRACE");
	for (int i = 0; i < 4; ++i) {
		netsim::PeerSpec sp;
		sp.impair   = wan((uint32_t)i + 111);
		sp.loads[0] = 100000000;
		o.peers.push_back(sp);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "after START: did not finish");
	std::string why;
	CHECK(same_streams(r, why, 3), "after START: %s", why.c_str());
	uint32_t e = 0;
	for (int i = 0; i < 3; ++i) {
		const auto& p = r.peers[i];
		CHECK(p.consumed >= o.ticks, "after START: peer %d consumed %u", i, p.consumed);
		auto it = p.drops.find(3);
		CHECK(it != p.drops.end(), "after START: peer %d saw no drop of seat 3", i);
		if (it == p.drops.end()) continue;
		if (e == 0) e = it->second;
		CHECK(it->second == e, "after START: peer %d drop tick %u, peer 0 %u", i, it->second, e);
		CHECK(p.neutralFromDrop, "after START: peer %d: seat 3 not neutral from E", i);
	}
	CHECK(e > 1500 && e < 1520, "after START: E = %u", e);
	CHECK(r.peers[3].endReason == kEndKicked, "after START: the removed peer ended with %d (want kicked)", r.peers[3].endReason);
	CHECK(r.peers[3].consumed < 1600, "after START: the removed peer ran on to tick %u", r.peers[3].consumed);
	return ok;
}

// Fix-min item 6: a spectator whose checksum differs gets DESYNC with seat 255 and stops; the players
// go on to the end.
bool case_specdesync()
{
	bool ok = true;
	netsim::Options o;
	o.ticks = 2000;
	for (int i = 0; i < 3; ++i) {
		netsim::PeerSpec sp;
		sp.impair    = wan((uint32_t)i + 121);
		sp.spectator = i == 2;
		sp.loads[0]  = 100000000;
		if (i == 2) sp.desyncAt = 900;
		o.peers.push_back(sp);
	}
	netsim::Result r = netsim::run(o);
	if (g_verbose) print_peers(r);
	CHECK(r.finished, "did not finish");
	const auto& sp = r.peers[2];
	CHECK(sp.endReason == kEndDesync, "spectator end reason %d (want desync)", sp.endReason);
	CHECK(sp.desyncTick == 900 && sp.desyncSlot == kNoSlot, "spectator desync tick %u slot %u (want 900, 255)", sp.desyncTick, sp.desyncSlot);
	CHECK(sp.consumed < 960, "the spectator ran on to tick %u", sp.consumed);
	for (int i = 0; i < 2; ++i) {
		CHECK(r.peers[i].consumed >= o.ticks, "player %d stopped at tick %u", i, r.peers[i].consumed);
		CHECK(r.peers[i].endReason != kEndDesync && r.peers[i].desyncTick == 0, "player %d desynced too (reason %d, tick %u)", i,
		      r.peers[i].endReason, r.peers[i].desyncTick);
	}
	CHECK(r.peers[0].streamHash == r.peers[1].streamHash, "the players' streams differ");
	return ok;
}

struct Case {
	const char* name;
	std::function<bool()> fn;
};

} // namespace

int main(int argc, char** argv)
{
	std::vector<std::string> only;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "-v")) g_verbose = true;
		else only.push_back(argv[i]);
	}
	const bool quick = std::getenv("P2NET_SELFTEST_QUICK") != nullptr;
	const uint32_t longTicks = quick ? 6000 : 18000;
	std::vector<Case> cases = {
		{ "wire", case_wire },
		{ "delay", case_delay },
		{ "sync", case_sync },
		{ "lockstep2", [=] { return lockstep(2, longTicks, 0, 1.0, "lockstep2"); } },
		{ "lockstep4", [=] { return lockstep(4, longTicks, 0, 1.0, "lockstep4"); } },
		{ "loss", [=] { return lockstep(2, longTicks / 2, 20000, 3.0, "loss"); } },
		{ "barrier", case_barrier },
		{ "drop", case_drop },
		{ "desync", case_desync },
		{ "spectator", case_spectator },
		{ "delaychange", case_delaychange },
		{ "lobby", case_lobby },
		{ "hijack", case_hijack },
		{ "reqslot", case_reqslot },
		{ "leave", case_leave },
		{ "stuck", case_stuck },
		{ "chunk", case_chunk },
		{ "modeswitch", case_modeswitch },
		{ "hostleft", case_hostleft },
		{ "utf8", case_utf8 },
		{ "pendingcap", case_pendingcap },
		{ "kick", case_kick },
		{ "specdesync", case_specdesync },
	};
	for (const Case& c : cases) {
		if (!only.empty() && std::find(only.begin(), only.end(), c.name) == only.end()) continue;
		g_why.clear();
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = c.fn();
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("%s %s (%.1f s)%s%s\n", ok ? "PASS" : "FAIL", c.name, s, ok ? "" : ": ", ok ? "" : g_why.c_str());
		std::fflush(stdout);
		if (!ok) ++g_fails;
	}
	std::printf("p2net_selftest: %s\n", g_fails ? "FAIL" : "PASS");
	return g_fails ? 1 : 0;
}
