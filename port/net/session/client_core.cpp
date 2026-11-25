// M5.3b client state machine (see client_core.hpp).
#include "client_core.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mppm::session {

namespace {

constexpr int64_t kKeepaliveNs  = 33000000;
constexpr int64_t kDupNs        = 16700000;
constexpr int64_t kHostTimeout  = 10000000000LL;
constexpr int64_t kLobbyGoneGraceNs = 1000000000LL; // the lobby says the host left: no P2P packet for this long (host_core.cpp)
constexpr int64_t kDesyncWaitNs = 500000000;
constexpr int64_t kPhaseSettleNs = 1000000000LL;
constexpr int64_t kShiftSettleNs = 400000000LL; // a grid move reaches the dues within a tick or a few

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

const char* end_text(int reason)
{
	switch (reason) {
	case kEndHostLeft: return "The match can't go on without the host.";
	case kEndKicked: return "The host removed you from the game.";
	case kEndConnectionLost: return "You were disconnected from the game.";
	case kEndRefused: return "The host refused the connection.";
	default: return "The game ended.";
	}
}

} // namespace

ClientCore::ClientCore(CoreEnv& env, const ClientParams& p) : m_env(env), m_p(p)
{
	m_spectator = p.spectator;
}

void ClientCore::on_connected(uint32_t peer, int64_t now)
{
	if (m_peer != 0 || m_ended) return; // one host connection; the endpoint resets extra ones
	m_peer       = peer;
	m_lastPacket = now;
	wire::Hello h;
	h.member    = m_p.member;
	h.secret    = m_p.secret;
	h.spectator = m_p.spectator;
	h.name      = m_p.name;
	std::memcpy(h.build, m_p.build, 32);
	std::memcpy(h.content, m_p.content, 32);
	h.mods = m_p.mods;
	std::vector<uint8_t> d;
	wire::encode(d, 0, h);
	m_bytesOut += d.size();
	m_env.send(peer, true, d, true);
	m_helloSent = true;
}

void ClientCore::on_disconnected(uint32_t peer, const std::string& reason, int64_t now)
{
	(void)now;
	if (peer != m_peer) return;
	m_peer = 0;
	m_env.log(fmt("peer 0 disconnected reason=%s", reason.c_str()));
	if (m_ended) return;
	if (m_started) {
		// Keep the bundles we have: the game ends when it needs one that cannot come.
		m_hostGone = true;
		std::lock_guard<std::mutex> lk(m_env.link().mu);
		if (m_env.link().end.reason == kEndNone) {
			m_env.link().end.reason    = kEndHostLeft;
			m_env.link().end.logReason = "host_left";
			m_env.link().end.text      = end_text(kEndHostLeft);
		}
		return;
	}
	end(m_welcomed ? kEndHostLeft : kEndConnectionLost, m_welcomed ? "host_left" : "error",
	    m_welcomed ? "The host closed the lobby." : "Couldn't connect to the host.");
}

void ClientCore::lost(int reason, const std::string& logReason, const std::string& text)
{
	if (m_ended) return;
	if (m_started && (reason == kEndHostLeft || reason == kEndConnectionLost)) {
		m_hostGone = true;
		std::lock_guard<std::mutex> lk(m_env.link().mu);
		if (m_env.link().end.reason == kEndNone) {
			m_env.link().end.reason    = reason;
			m_env.link().end.logReason = logReason;
			m_env.link().end.text      = text;
		}
		return;
	}
	end(reason, logReason, text);
}

void ClientCore::lobby_host_left(int64_t now)
{
	if (m_ended) return;
	if (!m_started) {
		end(kEndHostLeft, "host_left", "The host closed the lobby.");
		return;
	}
	if (m_peer && !m_hostGone && now - m_lastPacket < kLobbyGoneGraceNs) {
		if (!m_lobbyGoneAt) m_env.log("lobby says the host left; the match ends once its connection is silent");
		m_lobbyGoneAt = now;
		return;
	}
	lost(kEndHostLeft, "host_left", "The match can't go on without the host.");
}

void ClientCore::end(int reason, const std::string& logReason, const std::string& text)
{
	if (m_ended) return;
	m_ended = true;
	{
		std::lock_guard<std::mutex> lk(m_env.link().mu);
		EndInfo& e = m_env.link().end;
		if (reason == kEndDesync) e = m_pendingEnd;
		e.reason    = reason;
		e.logReason = logReason;
		if (e.text.empty() || reason == kEndDesync) e.text = text;
	}
	N2G g;
	g.kind = N2G::End;
	m_env.to_game(g);
	CoreNote n;
	n.kind = CoreNote::Ended;
	m_env.note(n);
}

void ClientCore::send_req(uint8_t what, int8_t value)
{
	if (!m_peer || !m_welcomed) return;
	std::vector<uint8_t> d;
	wire::encode(d, m_started ? m_cfg.sessionTag : 0, wire::Req{ what, value });
	m_bytesOut += d.size();
	m_env.send(m_peer, true, d, true);
}

void ClientCore::set_ready(bool ready)
{
	m_p.autoReady = false;
	send_req(wire::kReqReady, ready ? 1 : 0);
}

void ClientCore::take_slot(int slot)
{
	m_p.wantSlot = -1;
	send_req(wire::kReqSlot, (int8_t)slot);
}

void ClientCore::leave(int64_t now, uint8_t byeReason)
{
	(void)now;
	if (m_peer) {
		std::vector<uint8_t> d;
		if (!m_started) {
			wire::encode(d, 0, wire::Req{ wire::kReqLeave, 0 });
			m_env.send(m_peer, true, d, true);
			d.clear();
		}
		wire::encode(d, m_started ? m_cfg.sessionTag : 0, wire::Bye{ byeReason });
		m_env.send(m_peer, true, d, true);
	}
	if (byeReason == wire::kByeLeft) end(kEndLeft, "left", "You left the game.");
}

void ClientCore::request_lobby_wishes(int64_t now)
{
	if (m_started || !m_welcomed || m_spectator) return;
	const wire::LobbyMember* me = nullptr;
	for (const auto& mm : m_lobby.members)
		if (mm.id == m_p.member) me = &mm;
	if (!me) return;
	if (m_p.wantSlot >= 0 && me->slot != m_p.wantSlot) {
		// The host decides: it seats us there when the seat is open or held by someone it seated
		// automatically (a swap), else the request is ignored and repeated at most every 250 ms (a
		// LOBBY from someone else's change must not turn into a REQ storm).
		if (m_slotReqAt == INT64_MIN || now - m_slotReqAt >= 250000000) {
			m_slotReqAt = now;
			send_req(wire::kReqSlot, (int8_t)m_p.wantSlot);
		}
		return;
	}
	if (m_p.autoReady && me->slot != kNoSlot && !(me->flags & wire::kMemReady) && (me->flags & wire::kMemContentOk)) {
		send_req(wire::kReqReady, 1);
	}
}

void ClientCore::store_tag(uint32_t tag, const Input& in)
{
	m_tagIn[tag % m_tagIn.size()] = in;
	m_tagOf[tag % m_tagIn.size()] = tag;
}

int64_t ClientCore::due_local(uint32_t tick, int64_t& slewAt) const
{
	slewAt = 0;
	if (m_due.empty()) return 0;
	for (auto it = m_due.rbegin(); it != m_due.rend(); ++it) {
		if ((uint32_t)(*it)[0] == tick) {
			slewAt = (*it)[2];
			return (*it)[1];
		}
	}
	const auto& last = m_due.back();
	if (tick > (uint32_t)last[0] && tick - (uint32_t)last[0] <= 16) {
		slewAt = last[2];
		return last[1] + (int64_t)(tick - (uint32_t)last[0]) * kTickNs;
	}
	return 0;
}

void ClientCore::send_input(int64_t now)
{
	if (!m_peer) return;
	wire::InputPkt in;
	in.tSendUs   = us32(now);
	in.echoUs    = m_lastRecvTSend;
	in.holdUs    = m_lastRecvAt ? (uint32_t)((now - m_lastRecvAt) / 1000) : 0;
	in.ackBundle = m_ack;
	in.slot      = (m_started && m_seat >= 0) ? (uint8_t)m_seat : kNoSlot;
	in.delay     = (uint8_t)(m_started && m_seat >= 0 ? m_delay.current() : 0);
	in.loadedTick = m_loadedTick;
	in.loadCount  = m_loadCount;
	in.cksTick    = m_cksTick;
	in.cksTotal   = m_cksTotal;
	in.downP99    = m_down.valid() ? (uint16_t)std::clamp<int32_t>((m_down.p99(now) + 99) / 100, 1, 0xFFFF) : 0;
	in.stallPermille = (uint16_t)std::min<uint32_t>(m_stallPermille, 1000);
	if (m_started && m_seat >= 0 && m_lastTag > m_ack) {
		const uint32_t first = m_ack + 1;
		const uint32_t count = std::min<uint32_t>(m_lastTag - m_ack, kMaxWindow);
		uint32_t n           = 0;
		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t tag = first + i;
			if (m_tagOf[tag % m_tagIn.size()] != tag) break;
			in.inputs[n++] = m_tagIn[tag % m_tagIn.size()];
		}
		in.firstTag = first;
		in.count    = (uint8_t)n;
		if (n == 0) in.firstTag = 0;
	}
	std::vector<uint8_t> d;
	wire::encode(d, m_started ? m_cfg.sessionTag : 0, in);
	m_bytesOut += d.size();
	m_env.send(m_peer, false, d, true);
	m_lastSend = now;
	m_dupAt    = now + kDupNs;
}

void ClientCore::evaluate_delay(int64_t now)
{
	if (!m_started || m_seat < 0) return;
	if (now - m_lastDelayEval < 1000000000LL) return;
	m_lastDelayEval = now;
	int64_t path = -1;
	if (m_up[m_seat] != 255) {
		int64_t worst = 0;
		// Every seated peer needs this input in a bundle, this one included (its own bundle comes
		// back over its own downlink): max over all seated j, not only the others (D12 says "other";
		// with it a 2-player client's delay would ignore its own round trip and stall every tick).
		for (int s = 0; s < m_cfg.seats; ++s) {
			if (m_downT[s] == 255) continue;
			worst = std::max<int64_t>(worst, m_downT[s]);
		}
		path = ((int64_t)m_up[m_seat] + worst) * 1000;
	}
	const int before            = m_delay.current();
	const DelayPolicy::Change c = m_delay.evaluate(now, path, m_blame[m_seat] * 4);
	if (c != DelayPolicy::None && m_delay.current() != before)
		m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=%s", m_seat, m_delay.current(), path < 0 ? 0 : path,
		              c == DelayPolicy::Stall ? "stall" : "path"));
	if (m_delay.capped() && c != DelayPolicy::None)
		m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=path capped", m_seat, m_delay.current(), path < 0 ? 0 : path));
	if (m_delay.capped() && !m_cappedNoted) { // D13: the wish above 8 is shown as a warning, once
		m_cappedNoted = true;
		CoreNote n;
		n.kind  = CoreNote::Notice;
		n.kind2 = 2; // online::Notice::Warning
		n.title = "Your connection is slow";
		n.text  = "The input delay is held at its limit of 8 ticks, so the game may pause now and then.";
		m_env.note(n);
	}
	m_seatDelay[m_seat] = (uint8_t)m_delay.current();
}

void ClientCore::handle_release(uint32_t tick, uint16_t count, uint32_t hostUs, int64_t now)
{
	if (count <= m_releaseCount) return;
	m_releaseCount = count; // without a clock estimate yet (not after the 30 Hz warm-up) it releases now
	int64_t localNs = now;
	if (m_sync.valid()) {
		const uint32_t hostNow = m_sync.to_remote(us32(now));
		localNs                = now + (int64_t)us_diff(hostUs, hostNow) * 1000;
	}
	N2G g;
	g.kind  = N2G::Release;
	g.tick  = tick;
	g.count = count;
	g.ns    = localNs;
	m_env.to_game(g);
	m_releaseAt    = now;
	m_releasedTick = tick;
	m_haveRelease  = true;
	m_phase.reset();
	if (m_seat >= 0 && m_delay.boundary() == DelayPolicy::Boundary)
		m_env.log(fmt("delay slot=%d d=%d path_us=0 reason=boundary", m_seat, m_delay.current()));
}

void ClientCore::handle_bundle(const wire::BundlePkt& b, int64_t recvNs, int64_t now)
{
	if (b.echoUs != 0) m_sync.add(now, b.echoUs, b.tSendUs - b.holdUs, b.tSendUs, us32(recvNs));
	m_lastRecvTSend = b.tSendUs;
	m_lastRecvAt    = recvNs;
	if (m_sync.valid()) m_down.add(now, us_diff(us32(recvNs), m_sync.to_local(b.tSendUs)));
	if (!m_started) return;
	for (int s = 0; s < kMaxSeats; ++s) {
		m_up[s]    = b.upP99[s];
		m_downT[s] = b.downP99[s];
		m_blame[s] = b.blame[s];
	}
	m_waitMask = b.waitMask;
	m_env.link().waitMask.store(b.waitMask, std::memory_order_relaxed);
	if (b.slotMask != m_cfg.slotMask && b.count > 0) return; // cannot happen with a correct host
	for (int i = 0; i < b.count; ++i) {
		const uint32_t t = b.firstTick + (uint32_t)i;
		if (t <= m_ack || m_have.count(t)) continue;
		if (t > m_ack + 4096) break;
		m_have[t] = true;
		N2G g;
		g.kind = N2G::Bundle;
		g.tick = t;
		g.mask = b.slotMask;
		for (int s = 0; s < kMaxSeats; ++s) g.seat[s] = b.bundles[i][s];
		m_env.to_game(g);
	}
	while (!m_have.empty() && m_have.begin()->first == m_ack + 1) {
		m_have.erase(m_have.begin());
		++m_ack;
	}
	if (b.flags & wire::kBundleRelease) handle_release(b.releaseTick, b.releaseCount, b.releaseHostUs, now);
	// Phase error (clients only; the host never slews): e = dueLocal(hostTick) - hostDue (local clock).
	if (m_p.phaseControl && m_sync.valid() && b.hostTick != 0 && (!m_haveRelease || b.hostTick > m_releasedTick + 1)) {
		int64_t slewAt = 0;
		const int64_t dueLocal = due_local(b.hostTick, slewAt);
		if (dueLocal != 0) {
			const int32_t eUs = us_diff(us32(dueLocal), m_sync.to_local(b.hostDueUs));
			// Not wrapped into one tick: a long tick on one peer (the stage load runs inside sim ticks)
			// leaves the grids whole retraces or ticks apart, and a wrapped error hides a whole-tick lag
			// (the host then stalls on every tick) and flips sign at P/2 for an odd retrace.
			const int64_t e = (int64_t)eUs * 1000;
			if (e > kPhaseMaxNs || e < -kPhaseMaxNs) return;
			if (now < m_shiftUntil) return; // the game has not taken the grid move yet
			m_lastPhaseE = e;
			m_phase.add(now, e, slewAt);
			// The statistic is the steady-state error: samples taken while a catch-up of more than 2 ms
			// was pending (after a long tick) are left out.
			if (m_haveRelease && now - m_releaseAt >= kPhaseSettleNs && slewAt >= -2000000 && slewAt <= 2000000 && m_phaseAbs.size() < 2000000)
				m_phaseAbs.push_back((int32_t)std::min<int64_t>(e < 0 ? -e : e, INT32_MAX) / 1000);
			int64_t newSlew = 0;
			if (m_phase.update(now, m_env.slew_get(), newSlew)) {
				// Whole ticks move the game's tick grid at once (a lagging client catches up one tick
				// per retrace, a leading one holds once); the slew keeps the sub-tick rest. At 1 ms per
				// retrace a lag of 100 ms after a long tick took 2 s, and the others stalled on its
				// late inputs meanwhile.
				// < 0: this client is late. From one whole tick on, rounded (the rest is within half a tick).
				const int64_t ticks = (newSlew >= kTickNs || newSlew <= -kTickNs) ? (newSlew + (newSlew < 0 ? -kTickNs / 2 : kTickNs / 2)) / kTickNs : 0;
				if (ticks != 0 && m_env.tick_shift((int)-ticks)) {
					newSlew -= ticks * kTickNs;
					m_phase.reset();
					m_shiftUntil = now + kShiftSettleNs;
					m_env.log(fmt("phase shift ticks=%d rest_us=%lld", (int)-ticks, (long long)(newSlew / 1000)));
				}
				m_env.slew_set(newSlew);
			}
		}
	}
}

void ClientCore::on_datagram(uint32_t peer, const uint8_t* p, size_t n, int64_t recvNs, int64_t now)
{
	if (peer != m_peer || m_ended) return;
	m_bytesIn += n;
	wire::Header h;
	if (wire::decode_header(p, n, h) != wire::Result::Ok) return;
	if (h.tag != 0 && (!m_started || h.tag != m_cfg.sessionTag)) return;
	m_lastPacket = now;
	switch (h.type) {
	case wire::WELCOME: {
		wire::Welcome w;
		if (wire::decode(p, n, w) != wire::Result::Ok) return;
		m_welcomed  = true;
		m_spectator = w.role == 2;
		CoreNote note;
		note.kind = CoreNote::Welcomed;
		m_env.note(note);
		return;
	}
	case wire::REJECT: {
		wire::Reject r;
		if (wire::decode(p, n, r) != wire::Result::Ok) r.text = "The host refused the connection.";
		m_env.log(fmt("the host refused the connection: %s", r.text.c_str()));
		end(kEndRefused, "error", r.text.empty() ? "The host refused the connection." : r.text);
		return;
	}
	case wire::LOBBY: {
		wire::Lobby l;
		if (wire::decode(p, n, l) != wire::Result::Ok) return;
		if (l.generation < m_lobby.generation && m_lobby.generation != 0) return;
		const auto slot_in = [this](const wire::Lobby& v) {
			for (const auto& mm : v.members)
				if (mm.id == m_p.member) return mm.slot;
			return kNoSlot;
		};
		const bool lostSeat = !m_spectator && m_lobby.seats != 0 && l.seats < m_lobby.seats && slot_in(m_lobby) != kNoSlot &&
		                      slot_in(l) == kNoSlot;
		m_lobby = l;
		CoreNote note;
		note.kind = CoreNote::LobbyChanged;
		m_env.note(note);
		if (lostSeat) {
			// The host picked a mode with fewer seats and none was free for us (HostCore::set_config).
			CoreNote w;
			w.kind  = CoreNote::Notice;
			w.kind2 = 0;
			w.title = "You're watching";
			w.text  = "The new mode has no free seat for you.";
			m_env.note(w);
		}
		request_lobby_wishes(now);
		return;
	}
	case wire::START: {
		if (m_started) return;
		SessionConfig c;
		if (wire::decode(p, n, c) != wire::Result::Ok) {
			m_env.log("bad START from the host");
			return;
		}
		m_cfg     = c;
		m_started = true;
		m_seat    = -1;
		for (int s = 0; s < c.seats; ++s)
			if (c.slotMember[s] == m_p.member && !m_spectator) m_seat = s;
		if (m_seat >= 0) {
			int d0 = m_p.initialDelay;
			if (d0 <= 0) {
				int64_t path = -1;
				if (m_down.valid() && m_sync.valid()) path = (int64_t)m_down.p99(now) + (int64_t)m_sync.rtt_us() / 2;
				d0 = path >= 0 ? delay_for_path(path) : 2;
			}
			m_delay.init(d0, now);
			m_lastDelayEval = now;
			for (uint32_t t = 1; t <= (uint32_t)d0; ++t) store_tag(t, Input{});
			m_lastTag   = (uint32_t)d0;
			m_prevInput = Input{};
			m_seatDelay[m_seat] = (uint8_t)d0;
			m_env.log(fmt("delay slot=%d d=%d path_us=%u reason=init", m_seat, d0,
			              (unsigned)(m_down.valid() ? m_down.p99(now) + (int32_t)m_sync.rtt_us() / 2 : 0)));
		}
		{
			std::lock_guard<std::mutex> lk(m_env.link().mu);
			m_env.link().config    = m_cfg;
			m_env.link().localSeat = m_seat;
		}
		N2G g;
		g.kind = N2G::Start;
		m_env.to_game(g);
		CoreNote note;
		note.kind = CoreNote::Started;
		m_env.note(note);
		send_input(now);
		return;
	}
	case wire::RELEASE: {
		wire::Release r;
		if (wire::decode(p, n, r) != wire::Result::Ok) return;
		handle_release(r.barrierTick, r.count, us32((int64_t)r.releaseHostNs), now);
		return;
	}
	case wire::DROP: {
		wire::Drop d;
		if (wire::decode(p, n, d) != wire::Result::Ok) return;
		m_env.log(fmt("drop slot=%d tick=%u reason=%s", (int)d.slot, d.effectiveTick,
		              d.reason == wire::kDropLeft ? "left" : d.reason == wire::kDropTimeout ? "timeout" : d.reason == wire::kDropKicked ? "kicked" : "host"));
		if ((int)d.slot == m_seat) {
			end(kEndConnectionLost, "dropped", "You were disconnected from the game.");
			return;
		}
		N2G g;
		g.kind = N2G::Drop;
		g.tick = d.effectiveTick;
		g.mask = d.slot;
		m_env.to_game(g);
		std::string name = "A player";
		for (const auto& mm : m_lobby.members)
			if (mm.slot == d.slot) name = mm.name;
		CoreNote note;
		note.kind  = CoreNote::Notice;
		note.kind2 = 2;
		note.title = name + " left the game";
		note.text  = "Their captain stands still for the rest of the match.";
		m_env.note(note);
		return;
	}
	case wire::DESYNC: {
		wire::Desync ds;
		if (wire::decode(p, n, ds) != wire::Result::Ok || m_desyncEndAt) return;
		std::string mine;
		for (const auto& l : m_lines)
			if (l.first == ds.tick) mine = l.second;
		if (!mine.empty()) {
			std::vector<uint8_t> d;
			wire::encode(d, m_cfg.sessionTag, wire::CksLine{ ds.tick, mine });
			m_env.send(m_peer, true, d, true);
		}
		m_pendingEnd            = EndInfo{};
		m_pendingEnd.desyncTick = ds.tick;
		m_pendingEnd.desyncSlot = ds.slot;
		m_pendingEnd.hostTotal  = ds.hostTotal;
		m_pendingEnd.peerTotal  = ds.peerTotal;
		// Slot 255: only this spectator's game differs (HostCore::desync); the players go on.
		m_pendingEnd.text = ds.slot == kNoSlot ? fmt("Your game went out of sync at tick %u. The players go on without you.", ds.tick)
		                                       : fmt("Desync at tick %u. The match was stopped on every machine.", ds.tick);
		m_desyncEndAt     = now + kDesyncWaitNs;
		return;
	}
	case wire::CKSLINE: {
		wire::CksLine c;
		if (wire::decode(p, n, c) != wire::Result::Ok) return;
		if (m_desyncEndAt && c.tick == m_pendingEnd.desyncTick) m_pendingEnd.hostLine = c.line;
		else if (!m_desyncEndAt) {
			m_pendingEnd.desyncTick = c.tick; // a line that came before its DESYNC
			m_pendingEnd.hostLine   = c.line;
		}
		return;
	}
	case wire::END: {
		wire::End e;
		if (wire::decode(p, n, e) != wire::Result::Ok) return;
		if (e.reason == kEndDone) {
			m_hostGone = true; // the bundles we have may still carry us to our own last tick
			std::lock_guard<std::mutex> lk(m_env.link().mu);
			if (m_env.link().end.reason == kEndNone) {
				m_env.link().end.reason    = kEndHostLeft;
				m_env.link().end.logReason = "host_left";
				m_env.link().end.text      = end_text(kEndHostLeft);
			}
			return;
		}
		const int reason = (e.reason == kEndKicked) ? kEndKicked : kEndHostLeft;
		if (m_started && reason == kEndHostLeft) {
			lost(kEndHostLeft, "host_left", end_text(kEndHostLeft));
			return;
		}
		end(reason, reason == kEndKicked ? "dropped" : "host_left", e.text.empty() ? end_text(reason) : e.text);
		return;
	}
	case wire::BYE:
		if (m_started) lost(kEndHostLeft, "host_left", end_text(kEndHostLeft));
		else end(kEndHostLeft, "host_left", "The host closed the lobby.");
		return;
	case wire::BUNDLE: {
		wire::BundlePkt b;
		if (wire::decode(p, n, b) != wire::Result::Ok) return;
		handle_bundle(b, recvNs, now);
		return;
	}
	default:
		return;
	}
}

void ClientCore::on_game(const G2N& g, int64_t now)
{
	switch (g.kind) {
	case G2N::LocalInput: {
		if (!m_started || m_seat < 0 || m_ended) return;
		evaluate_delay(now);
		const uint32_t desired = g.tick + (uint32_t)m_delay.current();
		if (desired > m_lastTag) {
			for (uint32_t t = m_lastTag + 1; t < desired; ++t) store_tag(t, m_prevInput);
			store_tag(desired, g.input);
			m_lastTag = desired;
		}
		m_prevInput = g.input;
		send_input(now);
		return;
	}
	case G2N::TickDue:
		m_lastTickRead = g.tick;
		if (g.ns != 0) {
			m_due.push_back({ (int64_t)g.tick, g.ns, g.slewNs });
			while (m_due.size() > 64) m_due.pop_front();
		}
		if (m_seat < 0 && m_started) send_input(now); // spectators: ack and sync once per tick
		return;
	case G2N::Checksum:
		m_cksTick  = g.tick;
		m_cksTotal = g.total;
		m_lines.emplace_back(g.tick, std::string(g.line));
		while (m_lines.size() > 64) m_lines.pop_front();
		return;
	case G2N::Loaded:
		m_loadedTick = g.tick;
		m_loadCount  = (uint16_t)g.count;
		send_input(now);
		return;
	case G2N::Begun:
		return;
	case G2N::Leave:
		leave(now, (uint8_t)g.count);
		return;
	case G2N::Done:
		return;
	}
}

void ClientCore::tick(int64_t now)
{
	if (m_desyncEndAt && now >= m_desyncEndAt && !m_ended) {
		m_desyncEndAt = 0;
		end(kEndDesync, "desync", m_pendingEnd.text);
		return;
	}
	if (m_lobbyGoneAt && !m_ended && !m_hostGone && (!m_peer || now - m_lastPacket >= kLobbyGoneGraceNs)) {
		m_lobbyGoneAt = 0;
		m_env.log("the host's connection is silent too");
		lost(kEndHostLeft, "host_left", "The match can't go on without the host.");
	}
	if (m_ended || !m_peer) return;
	if (!m_hostGone && m_helloSent && now - m_lastPacket > kHostTimeout) {
		m_env.log("no packet from the host for 10 s");
		if (m_started) lost(kEndConnectionLost, "error", end_text(kEndConnectionLost));
		else end(kEndConnectionLost, "error", "Lost the connection to the host.");
		return;
	}
	if (m_welcomed && !m_started && now - m_reqAt > 500000000) {
		m_reqAt = now; // repeat the lobby wishes (a REQ may race a LOBBY)
		request_lobby_wishes(now);
	}
	if (m_welcomed && ((m_dupAt && now >= m_dupAt) || now - m_lastSend >= kKeepaliveNs)) {
		const bool dup = m_dupAt && now >= m_dupAt;
		send_input(now);
		if (dup) m_dupAt = 0;
	}
	evaluate_delay(now);
	if (m_p.phaseControl && m_started && m_sync.valid() && now - m_lastSyncLog >= 1000000000LL) {
		m_lastSyncLog = now;
		m_env.log(fmt("sync tick=%u offset_us=%d rtt_us=%u phase_us=%d slew_us=%d", m_lastTickRead, (int32_t)m_sync.offset_us(),
		              m_sync.rtt_us(), (int)(m_lastPhaseE / 1000), (int)(m_env.slew_get() / 1000)));
	}
}

} // namespace mppm::session
