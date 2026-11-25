// M5.3b netsim (see netsim.hpp).
#include "netsim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace netsim {

namespace {

constexpr int64_t kMarginNs = 1000000; // late sampling: read 1 ms before the tick's retrace

struct Grid {
	int64_t next        = 0; // true time of the next retrace
	uint32_t nextCount  = 1;
	int64_t slew        = 0;
	int64_t lastFiredAt = 0;
	uint32_t lastFired  = 0;
	void advance(int64_t now)
	{
		while (now >= next) {
			lastFired   = nextCount;
			lastFiredAt = next;
			++nextCount;
			const int64_t step = std::clamp<int64_t>(slew, -1000000, 1000000);
			slew -= step;
			next += kRetraceNs + step;
		}
	}
	int64_t time_of(uint32_t c) const
	{
		if ((int32_t)(c - nextCount) >= 0) return next + (int64_t)(int32_t)(c - nextCount) * kRetraceNs;
		return lastFiredAt - (int64_t)(int32_t)(lastFired - c) * kRetraceNs;
	}
	uint32_t jump(int64_t at, int64_t now)
	{
		next = std::max(at, now);
		slew = 0;
		return nextCount;
	}
};

uint64_t fnv(uint64_t h, const void* p, size_t n)
{
	const uint8_t* b = (const uint8_t*)p;
	for (size_t i = 0; i < n; ++i) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

struct Peer;

struct Hooks : GameHooks {
	Peer* p = nullptr;
	int64_t* trueNow = nullptr;
	int64_t now() override;
	uint32_t grid_jump(int64_t localNs) override;
	void align_tick(uint32_t count) override;
	int64_t retrace_time(uint32_t count) override;
	int64_t next_retrace_estimate() override;
	int64_t margin_ns() override { return kMarginNs; }
	void log(const std::string& line) override;
};

struct Peer {
	int index = 0;
	PeerSpec spec;
	uint32_t member = 0;
	int seat = -1;
	std::unique_ptr<MemTransport> tr;
	std::unique_ptr<Endpoint> ep;
	std::unique_ptr<GameSide> side;
	Hooks hooks;
	Grid grid;
	bool started = false, inLoad = false, ended = false, vanished = false, done = false;
	int64_t loadEnd = 0;
	uint32_t t = 0;
	uint32_t dueSent = 0;
	uint32_t nextCount = 0;
	bool nextValid = false;
	uint32_t alignCount = 0;
	bool alignSet = false;
	uint64_t hash = 1469598103934665603ull;
	PeerResult res;
	bool verbose = false;
	int lastDelay = 0;
};

int64_t Hooks::now() { return *trueNow + p->spec.clockOffsetNs; }
uint32_t Hooks::grid_jump(int64_t localNs) { return p->grid.jump(localNs - p->spec.clockOffsetNs, *trueNow); }
void Hooks::align_tick(uint32_t count)
{
	p->alignCount = count;
	p->alignSet   = true;
}
int64_t Hooks::retrace_time(uint32_t count) { return p->grid.time_of(count) + p->spec.clockOffsetNs; }
int64_t Hooks::next_retrace_estimate() { return p->grid.next + p->spec.clockOffsetNs; }
void Hooks::log(const std::string& line)
{
	if (p->verbose) std::fprintf(stderr, "  [%d] %s\n", p->index, line.c_str());
	if (p->res.logs.size() < 4000) p->res.logs.push_back(line);
}

} // namespace

Input gen_input(int seat, uint32_t t)
{
	uint32_t x = (uint32_t)seat * 0x9E3779B9u ^ ((t / 7) * 0x85EBCA6Bu);
	x ^= x >> 13;
	x *= 0xC2B2AE35u;
	x ^= x >> 16;
	Input in;
	in.b[0] = (uint8_t)(x & 0x1F);
	in.b[1] = (uint8_t)((x >> 5) & 0x1F);
	in.b[2] = (uint8_t)(x >> 8);
	in.b[3] = (uint8_t)(x >> 16);
	in.b[4] = (uint8_t)(x >> 24);
	return in;
}

Result run(const Options& o)
{
	Result result;
	int64_t trueNow = 1000000000000LL; // 1000 s
	MemHub hub([&trueNow] { return trueNow; });
	hub.resetTimeoutNs = 1500000000LL;
	const int n = (int)o.peers.size();
	std::vector<std::unique_ptr<Peer>> peers;
	int nextSeat = 1;
	for (int i = 0; i < n; ++i) {
		auto p    = std::make_unique<Peer>();
		p->index  = i;
		p->spec   = o.peers[i];
		p->member = (uint32_t)i;
		p->verbose = o.verbose;
		p->tr     = hub.endpoint(i == 0 ? "host" : "peer" + std::to_string(i), p->spec.impair);
		p->tr->set_clock_offset(p->spec.clockOffsetNs);
		p->hooks.p       = p.get();
		p->hooks.trueNow = &trueNow;
		Peer* raw        = p.get();
		Endpoint::Hooks eh;
		eh.clock   = [raw, &trueNow] { return trueNow + raw->spec.clockOffsetNs; };
		eh.log     = [raw](const std::string& l) { raw->hooks.log(l); };
		eh.slewGet = [raw] { return raw->grid.slew; };
		eh.slewSet = [raw](int64_t v) { raw->grid.slew = v; };
		p->ep      = std::make_unique<Endpoint>(p->tr.get(), eh);
		p->side    = std::make_unique<GameSide>(p->ep->link(), p->hooks);
		p->grid.next = trueNow + p->spec.phaseNs + kRetraceNs;
		if (i == 0) {
			HostParams hp;
			hp.mode       = o.mode;
			hp.hostSlot   = 0;
			hp.hostReady  = true;
			hp.autoStart  = true;
			hp.expect     = n;
			hp.name       = "Host";
			hp.seed       = 20261122;
			if (o.longStart) {
				hp.syncedJson = "{\"game.long\":\"" + std::string(1900, 'x') + "\"}";
				for (int k = 0; k < 60; ++k) hp.mods += (k ? "," : "") + std::string("mod") + std::to_string(k) + "@1.0.0";
			}
			hp.sessionTag = 0x5EED1234;
			p->ep->make_host(hp);
			p->seat = 0;
		} else {
			ClientParams cp;
			cp.member    = (uint32_t)i;
			cp.secret    = p->spec.wrongSecret ? 999u : 1000u + (uint32_t)i;
			cp.content[0] ^= p->spec.contentXor;
			cp.name      = "P" + std::to_string(i);
			cp.spectator = p->spec.spectator;
			cp.wantSlot  = p->spec.spectator ? -1 : nextSeat;
			cp.autoReady = true;
			if (!p->spec.spectator) p->seat = nextSeat++;
			p->ep->make_client(cp);
		}
		peers.push_back(std::move(p));
	}
	for (int i = 1; i < n; ++i) {
		peers[0]->ep->host()->add_member((uint32_t)i, "P" + std::to_string(i), peers[i]->spec.spectator, 1000u + (uint32_t)i, trueNow);
		peers[i]->tr->connect("mem:host", (uint32_t)i);
	}
	const int64_t maxTime = o.maxTimeNs ? o.maxTimeNs : (int64_t)o.ticks * kTickNs * 3 / 2 + 60000000000LL;
	const int64_t startTime = trueNow;
	bool changed1 = false, changed2 = false;
	int64_t lastDbg = 0;
	while (trueNow - startTime < maxTime) {
		if (o.verbose && trueNow - lastDbg > 1000000000LL && !peers[0]->ep->host()->started()) {
			lastDbg = trueNow;
			auto l = peers[0]->ep->host()->lobby_view();
			std::fprintf(stderr, "  lobby: %s;", peers[0]->ep->host()->status_line().c_str());
			for (auto& m : l.members) std::fprintf(stderr, " %u:slot%d:f%02x", m.id, (int)(int8_t)m.slot, m.flags);
			std::fprintf(stderr, "\n");
		}
		if (o.impairPeer >= 0 && !changed1 && o.impairChangeAt && trueNow - startTime >= o.impairChangeAt) {
			changed1 = true;
			peers[o.impairPeer]->tr->set_impair(o.impairNew);
			if (o.impairPeer != 0) peers[0]->tr->set_impair(o.impairNew); // both directions of that path
		}
		if (o.impairPeer >= 0 && !changed2 && o.impairChange2At && trueNow - startTime >= o.impairChange2At) {
			changed2 = true;
			peers[o.impairPeer]->tr->set_impair(peers[o.impairPeer]->spec.impair);
			if (o.impairPeer != 0) peers[0]->tr->set_impair(peers[0]->spec.impair);
		}
		bool allDone = true;
		for (auto& pp : peers) {
			Peer& p = *pp;
			if (p.vanished) continue;
			p.grid.advance(trueNow);
			p.ep->step(0);
			if (p.done) continue;
			GameSide& s = *p.side;
			s.poll();
			const int d = p.ep->host() ? p.ep->host()->my_delay() : (p.ep->client()->started() ? p.ep->client()->my_delay() : 0);
			if (d != p.lastDelay) {
				p.lastDelay = d;
				p.res.delayLog.emplace_back(trueNow - startTime, d);
			}
			if (!p.started) {
				if (s.started()) {
					p.started = true;
					s.begun();
					p.t       = 0;
					auto it   = p.spec.loads.find(0);
					p.inLoad  = true;
					p.loadEnd = trueNow + (it != p.spec.loads.end() ? it->second : 0);
					if (p.spec.hangLoadAt == 0) {
						p.done = true; // stuck in the start load: only the endpoint keeps stepping
						continue;
					}
				} else {
					if (p.ep->host() ? p.ep->host()->ended() : p.ep->client()->ended()) p.done = true;
					allDone = allDone && p.done;
					continue;
				}
			}
			if (p.inLoad) {
				if (trueNow >= p.loadEnd) {
					p.inLoad    = false;
					p.nextValid = false;
					s.loaded(p.t);
				}
				allDone = false;
				continue;
			}
			const uint32_t T = p.t + 1;
			if (s.over(T)) {
				p.done           = true;
				p.ended          = true;
				p.res.endTick    = p.t;
				std::lock_guard<std::mutex> lk(p.ep->link().mu);
				p.res.endReason  = p.ep->link().end.reason;
				p.res.desyncTick = p.ep->link().end.desyncTick;
				p.res.desyncSlot = p.ep->link().end.desyncSlot;
				continue;
			}
			const int64_t dueLocal = p.nextValid ? p.grid.time_of(p.nextCount) + p.spec.clockOffsetNs : 0;
			const int64_t localNow = trueNow + p.spec.clockOffsetNs;
			if (p.nextValid && localNow < dueLocal - kMarginNs) {
				allDone = false;
				continue;
			}
			// Read tick T: as in the game (online_exchange), the due time goes out before the wait for
			// the bundle, once per tick.
			if (p.dueSent != T) {
				p.dueSent = T;
				s.tick_due(T, dueLocal, p.grid.slew);
			}
			if (!s.ready(T)) {
				allDone = false;
				continue;
			}
			if (p.seat >= 0) s.input(T, gen_input(p.seat, T));
			Input b[kMaxSeats];
			uint8_t mask = 0;
			s.take(T, b, mask);
			for (const auto& [seat, e] : s.drops())
				if (T >= e && b[seat] != Input{}) p.res.neutralFromDrop = false;
			if (T <= o.ticks) {
				p.hash = fnv(p.hash, &T, 4);
				p.hash = fnv(p.hash, &mask, 1);
				p.hash = fnv(p.hash, b, sizeof(b));
				if (T % 1000 == 0) p.res.hashAt.push_back(p.hash);
			}
			const bool barrierRead = !p.nextValid;
			const int64_t waited   = p.nextValid ? std::max<int64_t>(0, localNow - dueLocal) : 0;
			s.note_read(T, waited, barrierRead);
			// The tick runs at the first retrace message the wait accepts.
			uint32_t required = p.nextValid ? p.nextCount : (p.alignSet ? p.alignCount : p.grid.nextCount);
			if (p.alignSet && (int32_t)(p.alignCount - required) > 0) required = p.alignCount;
			uint32_t crun = (int32_t)(p.grid.nextCount - required) <= 0 ? required : p.grid.nextCount;
			if (p.alignSet) {
				p.res.releaseRun.push_back(p.grid.time_of(crun));
				p.res.releaseTicks.push_back(T);
			}
			p.alignSet  = false;
			p.nextCount = crun + 2;
			p.nextValid = true;
			p.t         = T;
			p.res.consumed = T;
			if (T % kCksInterval == 0) {
				uint32_t total = (uint32_t)p.hash ^ (uint32_t)(p.hash >> 32);
				if (p.spec.desyncAt && T >= p.spec.desyncAt) total ^= 1;
				char line[64];
				std::snprintf(line, sizeof(line), "C %u %08x", T, total);
				s.checksum(T, total, line);
			}
			if (p.spec.lobbyHostLeftAt && T == p.spec.lobbyHostLeftAt && p.ep->client()) p.ep->client()->lobby_host_left(localNow);
			if (o.kickAt && T == o.kickAt && p.ep->host()) p.ep->host()->kick(o.kickMember, localNow);
			if (p.spec.leaveAt && T == p.spec.leaveAt) {
				s.leave(1); // wire::kByeLeft
				p.done = true;
				continue;
			}
			if (p.spec.doneAt && T == p.spec.doneAt) {
				s.leave(2); // wire::kByeDone
				p.done = true;
				continue;
			}
			if (p.spec.crashAt && T == p.spec.crashAt) {
				p.ep->link().leaveReq.store(3); // wire::kByeError, as p2_net_crash_stop does
				p.done = true;
				continue;
			}
			if (p.spec.freezeAt && T == p.spec.freezeAt) {
				p.done = true; // a hung game thread: only the endpoint keeps stepping (keepalives)
				continue;
			}
			if (p.spec.vanishAt && T == p.spec.vanishAt) {
				hub.vanish(p.tr->name(), p.spec.vanishNoticeNs);
				p.vanished = true;
				p.done     = true;
				continue;
			}
			auto it = p.spec.loads.find(T);
			if (it != p.spec.loads.end()) {
				p.inLoad  = true;
				p.loadEnd = trueNow + it->second;
			}
			if (p.spec.hangLoadAt > 0 && T == (uint32_t)p.spec.hangLoadAt) {
				p.done = true;
				continue;
			}
			if (T >= o.ticks) {
				p.done = true;
				if (p.ep->host()) s.done(T);
				continue;
			}
			allDone = false;
		}
		if (allDone) {
			// Let the host finish its linger (a few more steps), then stop.
			result.finished = true;
			break;
		}
		trueNow += o.stepNs;
	}
	result.endTimeNs   = trueNow - startTime;
	result.hostStarted = peers[0]->ep->host()->started();
	for (const auto& m : peers[0]->ep->host()->lobby_view().members) result.notes.push_back(m.note);
	for (auto& pp : peers) {
		Peer& p          = *pp;
		p.res.streamHash = p.hash;
		p.res.stats      = p.side->stats();
		p.res.drops      = p.side->drops();
		{
			std::lock_guard<std::mutex> lk(p.ep->link().mu);
			p.res.syncedLen = p.ep->link().config.syncedJson.size();
			p.res.mods      = p.ep->link().config.mods;
		}
		if (p.ep->client()) {
			p.res.phaseAbsUs = p.ep->client()->phase_samples();
			p.res.delay      = p.ep->client()->my_delay();
		} else {
			p.res.delay = p.ep->host()->my_delay();
		}
		if (!p.ended) {
			std::lock_guard<std::mutex> lk(p.ep->link().mu);
			if (p.ep->link().end.reason != kEndNone && p.res.endReason == kEndNone) {
				p.res.endReason  = p.ep->link().end.reason;
				p.res.desyncTick = p.ep->link().end.desyncTick;
				p.res.desyncSlot = p.ep->link().end.desyncSlot;
			}
		}
		result.peers.push_back(p.res);
	}
	return result;
}

} // namespace netsim
