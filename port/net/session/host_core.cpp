// M5.3b host state machine (see host_core.hpp).
#include "host_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

namespace mppm::session {

namespace {

constexpr int64_t kKeepaliveNs   = 33000000;   // 30 Hz before START and while nothing completes
constexpr int64_t kDupNs         = 16700000;   // duplicate send half a tick later
constexpr int64_t kInputTimeout  = 8000000000LL;  // no packet at all from a seated member
constexpr int64_t kStuckNs       = 10000000000LL; // a seat holds up every bundle while the host has its own input
constexpr int64_t kGoneStuckNs   = 1000000000LL;  // ... for a seat whose member said BYE after its last tick
constexpr int64_t kLoadNs        = 30000000000LL; // a seat still loading this long after the host reached the barrier
constexpr int64_t kStartLoadNs   = 60000000000LL; // ... the start barrier (every peer boots its game first)
constexpr int64_t kHelloWaitNs   = 5000000000LL;
constexpr int64_t kLingerNs      = 10000000000LL;
constexpr int64_t kDesyncWaitNs  = 500000000;
constexpr int kInputRateLimit   = 1000;         // INPUT datagrams per second from one member
constexpr int64_t kSampleGapNs   = 2000000;    // time-sync samples at most every 2 ms per member
constexpr int64_t kLobbyGoneGraceNs = 1000000000LL; // a member the lobby says left: no P2P packet for this long
constexpr int64_t kBlameWindow   = 3000000000LL;
constexpr int kInvalidLimit      = 50;
constexpr uint32_t kTagWindow    = 64;
constexpr size_t kCksKeep        = 128;

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

const char* drop_reason_name(uint8_t r)
{
	switch (r) {
	case wire::kDropLeft: return "left";
	case wire::kDropTimeout: return "timeout";
	case wire::kDropKicked: return "kicked";
	default: return "host";
	}
}

// "id@ver,id@ver" -> id -> ver
std::map<std::string, std::string> parse_mods(const std::string& list)
{
	std::map<std::string, std::string> out;
	size_t i = 0;
	while (i < list.size()) {
		size_t j = list.find(',', i);
		if (j == std::string::npos) j = list.size();
		std::string item = list.substr(i, j - i);
		size_t at        = item.find('@');
		if (!item.empty()) out[item.substr(0, at)] = at == std::string::npos ? "" : item.substr(at + 1);
		i = j + 1;
	}
	return out;
}

} // namespace

std::string content_note(const std::string& name, const std::string& hostMods, const std::string& peerMods, bool buildSame,
                         bool contentSame)
{
	if (buildSame && contentSame) return {};
	if (!buildSame) return name + " has a different build of mppm.";
	if (hostMods == peerMods) return name + " has different game files.";
	const auto h = parse_mods(hostMods), p = parse_mods(peerMods);
	std::string diff;
	auto add = [&diff](const std::string& s) {
		if (!diff.empty()) diff += ", ";
		diff += s;
	};
	for (const auto& [id, ver] : p) {
		auto it = h.find(id);
		if (it == h.end()) add(id + " " + ver + " (host has none)");
		else if (it->second != ver) add(id + " " + ver + " (host " + it->second + ")");
	}
	for (const auto& [id, ver] : h)
		if (!p.count(id)) add("no " + id + " (host " + ver + ")");
	if (diff.empty()) return name + " has the mods in another order.";
	std::string note = name + " has different mods: " + diff + ".";
	if (note.size() > wire::kMaxNote) note = wire::utf8_cut(note, wire::kMaxNote - 3) + "...";
	return note;
}

uint32_t synced_hash_of(const std::string& json)
{
	nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
	std::vector<std::pair<std::string, std::string>> lines;
	if (j.is_object()) {
		for (const auto& [key, value] : j.items()) lines.emplace_back(key, key + "=" + value.dump() + "\n");
	}
	std::sort(lines.begin(), lines.end());
	uint32_t h = 2166136261u;
	for (const auto& kl : lines) {
		for (const unsigned char ch : kl.second) {
			h ^= ch;
			h *= 16777619u;
		}
	}
	return h;
}

HostCore::HostCore(CoreEnv& env, const HostParams& p) : m_env(env), m_p(p)
{
	m_seats = mode_seats(p.mode);
	Member me;
	me.id        = 0;
	me.name      = wire::clean_name(p.name); // bounded like every other name (the server's rule)
	if (me.name.empty()) me.name = "Player";
	me.known     = true;
	me.connected = true;
	me.self      = true;
	me.ready     = p.hostReady;
	m_members.push_back(me);
	int slot = p.hostSlot;
	if (slot < 0 || slot >= m_seats) slot = 0;
	m_members[0].slot = slot;
	m_hostSlot        = slot;
}

HostCore::Member* HostCore::by_id(uint32_t id)
{
	for (Member& m : m_members)
		if (m.id == id) return &m;
	return nullptr;
}
const HostCore::Member* HostCore::by_id(uint32_t id) const
{
	for (const Member& m : m_members)
		if (m.id == id) return &m;
	return nullptr;
}
HostCore::Member* HostCore::by_peer(uint32_t peer)
{
	if (peer == 0) return nullptr;
	for (Member& m : m_members)
		if (m.peer == peer && !m.self) return &m;
	return nullptr;
}
HostCore::Member& HostCore::self() { return m_members[0]; }

std::string HostCore::member_name(uint32_t id) const
{
	const Member* m = by_id(id);
	return m ? m->name : std::string("Player");
}

uint32_t HostCore::seat_member(int seat) const
{
	if (seat < 0 || seat >= kMaxSeats) return 0xFFFFFFFFu;
	if (m_started) return m_cfg.slotMember[seat];
	for (const Member& m : m_members)
		if (m.slot == seat && !m.gone) return m.id;
	return 0xFFFFFFFFu;
}

int HostCore::connected_count() const
{
	int n = 0;
	for (const Member& m : m_members)
		if (m.connected && !m.gone) ++n;
	return n;
}

uint32_t HostCore::rtt_us_max() const
{
	uint32_t r = 0;
	for (const Member& m : m_members)
		if (!m.self && m.connected && m.sync.valid()) r = std::max(r, m.sync.rtt_us());
	return r;
}

int HostCore::delay_of_seat(int seat) const
{
	if (seat == m_hostSlot) return m_delay.current();
	if (!m_started || seat < 0 || seat >= m_seats) return 0;
	const Member* m = by_id(m_cfg.slotMember[seat]);
	return m ? m->delay : 0;
}

// ---- lobby ------------------------------------------------------------------------------------

void HostCore::add_member(uint32_t id, const std::string& name, bool spectator, uint64_t auth, int64_t now)
{
	if (id == 0 || m_banned.count(id)) return;
	Member* m = by_id(id);
	if (!m) {
		m_members.emplace_back();
		m = &m_members.back();
		m->id = id;
	}
	m->name      = name.empty() ? "Player" : name;
	m->spectator = spectator;
	m->auth      = auth;
	m->known     = true;
	changed();
	// Replay the Hellos that came before this MemberJoined (a wrong one is refused, the right one binds).
	std::vector<uint32_t> waiting;
	for (const auto& [peer, pp] : m_pending)
		if (pp.member == id && !pp.hello.empty()) waiting.push_back(peer);
	for (const uint32_t peer : waiting) pending_hello(peer, now);
}

void HostCore::remove_member(uint32_t id, uint8_t reason, int64_t now)
{
	if (reason == wire::kDropKicked && id != 0) m_banned.insert(id); // the lobby says kicked or banned
	Member* m = by_id(id);
	if (!m || m->self) return;
	if (m_started && reason != wire::kDropKicked && m->connected && !m->gone && m->peer && now - m->lastPacket < kLobbyGoneGraceNs) {
		// After START a member's lobby WebSocket can stall (server silence timeout, an idle-TCP NAT)
		// while its P2P link plays on: tick drops it only once that link is silent too (a killed game
		// stops sending at once; a real leave sends BYE first).
		if (!m->lobbyGoneAt) m_env.log(fmt("lobby says member %u %s; dropped once its connection is silent", m->id, reason == wire::kDropTimeout ? "timed out" : "left"));
		m->lobbyGoneAt     = now;
		m->lobbyGoneReason = reason;
		return;
	}
	member_lost(*m, reason, now);
}

void HostCore::set_member_link(uint32_t id, uint8_t link)
{
	if (Member* m = by_id(id)) {
		if (m->link != link) {
			m->link = link;
			changed();
		}
	}
}

void HostCore::member_lost(Member& m, uint8_t reason, int64_t now)
{
	if (m.gone) return;
	const uint32_t peer = m.peer;
	if (m_started && !m.spectator && m.slot >= 0 && !m_seat[m.slot].dropped && m_cfg.slotMember[m.slot] == m.id && !m.byeDone) {
		drop_seat(m.slot, reason, now);
	}
	if (m_started) {
		m.gone      = true;
		m.connected = false;
	} else {
		m.slot      = -1;
		m.ready     = false;
		m.connected = false;
		m.gone      = true;
	}
	m.peer = 0;
	if (peer) {
		m_env.disconnect(peer, reason);
		CoreNote n;
		n.kind   = CoreNote::PeerGone;
		n.member = m.id;
		m_env.note(n);
	}
	if (!m_started) {
		// Erase lobby members that left before START: their id is never reused.
		const uint32_t id = m.id;
		m_members.erase(std::remove_if(m_members.begin(), m_members.end(), [id](const Member& x) { return x.id == id && !x.self; }),
		                m_members.end());
	}
	changed();
}

std::string pending_source_key(const std::string& remote)
{
	if (remote.rfind("127.", 0) == 0 || remote.rfind("[::1]", 0) == 0) return {};
	// A direct UDP peer "a.b.c.d:port": the address without the port. Anything else (an ICE or relay
	// path, which the host sets up per member, or the in-process test transport) stays whole.
	const size_t colon = remote.rfind(':');
	if (colon == std::string::npos || colon == 0) return remote;
	for (size_t i = 0; i < colon; ++i)
		if (!(std::isdigit((unsigned char)remote[i]) || remote[i] == '.')) return remote;
	return remote.substr(0, colon);
}

void HostCore::on_connected(uint32_t peer, uint32_t connectData, int64_t now, const std::string& source)
{
	// Section 10: an unverified connection holds a transport slot for up to 5 s, so only members the
	// lobby has admitted (MemberJoined, not kicked or banned, not gone) get one, and only a few at a
	// time, per source and in all. The others are let go at once.
	const Member* m = by_id(connectData);
	const char* why = nullptr;
	if (connectData == 0) why = "no member id";
	else if (m_banned.count(connectData)) why = "the member was removed";
	else if (!m || !m->known || m->gone || m->self) why = "the lobby has not admitted that member";
	else if (m_pending.size() >= kMaxPendingTotal) why = "too many unverified connections";
	else {
		size_t fromSource = 0, forMember = 0;
		for (const auto& [p, pp] : m_pending) {
			if (!source.empty() && pp.source == source) ++fromSource;
			if (pp.member == connectData) ++forMember;
		}
		if (fromSource >= kMaxPendingPerSource) why = "too many unverified connections from one address";
		else if (forMember >= kMaxPendingPerMember) why = "too many unverified connections for one member";
	}
	if (why) {
		m_env.log(fmt("connection claiming member %u refused: %s", connectData, why));
		m_env.disconnect(peer, 0);
		return;
	}
	// Nothing is bound before the peer's Hello carries the member's join secret (pending_hello).
	PendingPeer& pp = m_pending[peer];
	pp.member       = connectData;
	pp.at           = now;
	pp.source       = source;
	pp.hello.clear();
}

void HostCore::on_disconnected(uint32_t peer, const std::string& reason, int64_t now)
{
	if (m_pending.erase(peer)) return;
	Member* m = by_peer(peer);
	if (!m) return;
	m_env.log(fmt("peer %u disconnected reason=%s", m->id, reason.c_str()));
	m->peer = 0;
	member_lost(*m, reason.find("timeout") != std::string::npos ? wire::kDropTimeout : wire::kDropLeft, now);
}

void HostCore::drop_pending(uint32_t peer, uint8_t reason, const std::string& text)
{
	auto it = m_pending.find(peer);
	if (it == m_pending.end()) return;
	const uint32_t claimed = it->second.member;
	m_pending.erase(it);
	if (reason != 0) {
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Reject{ reason, text });
		send_control(peer, d);
	}
	m_env.log(fmt("connection claiming member %u refused: %s", claimed, text.c_str()));
	m_env.disconnect(peer, reason);
}

void HostCore::pending_hello(uint32_t peer, int64_t now)
{
	auto it = m_pending.find(peer);
	if (it == m_pending.end()) return;
	const std::vector<uint8_t> bytes = it->second.hello;
	wire::Hello h;
	const wire::Result r = wire::decode(bytes.data(), bytes.size(), h);
	if (r != wire::Result::Ok) return drop_pending(peer, wire::kRejectProtocol, "Bad hello.");
	if (h.proto != wire::kProto)
		return drop_pending(peer, wire::kRejectProtocol, "This game uses another version of the online protocol.");
	Member* m = by_id(it->second.member);
	if (m && m->gone) return drop_pending(peer, 0, "the member has left");
	if (!m || !m->known) return; // MemberJoined not seen yet: add_member replays it (tick drops it after 5 s)
	if (h.member != m->id || h.secret != m->auth) return drop_pending(peer, wire::kRejectAuth, "The join secret is wrong.");
	if (m->connected) {
		// The member is already in: a second connection with its secret replaces the first in the
		// lobby; after START the seat keeps its connection.
		if (m_started) return drop_pending(peer, wire::kRejectStarted, "The game has already started.");
		m_pending.erase(it);
		if (m->peer && m->peer != peer) m_env.disconnect(m->peer, 0);
		m->peer       = peer;
		m->helloAt    = now;
		m->lastPacket = now;
		// The new connection may come from another build or mod set: check its hashes again, so the
		// start rule (build and content hashes match) cannot be passed by an earlier Hello.
		const bool buildSame   = std::memcmp(h.build, m_p.build, 32) == 0;
		const bool contentSame = std::memcmp(h.content, m_p.content, 32) == 0;
		m->contentOk = buildSame && contentSame;
		m->note      = content_note(m->name, m_p.mods, h.mods, buildSame, contentSame);
		if (!m->contentOk) m->ready = false;
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Welcome{ m->id, (uint8_t)(m->spectator ? 2 : 1) });
		send_control(peer, d);
		m_env.log(fmt("member %u reconnected", m->id));
		changed();
		return;
	}
	m_pending.erase(it);
	if (m->peer && m->peer != peer) m_env.disconnect(m->peer, 0);
	m->peer       = peer;
	m->helloAt    = now;
	m->lastPacket = now;
	handle_hello(*m, bytes.data(), bytes.size(), now);
}

void HostCore::send_control(uint32_t peer, const std::vector<uint8_t>& d)
{
	if (!peer) return;
	m_bytesOut += d.size();
	m_env.send(peer, true, d, true);
}

void HostCore::send_all_control(const std::vector<uint8_t>& d, bool spectatorsToo)
{
	for (Member& m : m_members) {
		if (m.self || !m.connected || m.gone || !m.peer) continue;
		if (!spectatorsToo && m.spectator) continue;
		send_control(m.peer, d);
	}
}

void HostCore::invalid(Member& m, int64_t now)
{
	if (now - m.invalidAt > 1000000000LL) {
		m.invalidAt  = now;
		m.invalidWin = 0;
	}
	if (++m.invalidWin > kInvalidLimit) {
		m_env.log(fmt("peer %u sent more than %d bad datagrams in a second; disconnecting it", m.id, kInvalidLimit));
		member_lost(m, wire::kDropByHost, now);
	}
}

void HostCore::malformed(Member& m, int64_t now)
{
	m_env.log(fmt("peer %u sent a malformed datagram; disconnecting it", m.id));
	member_lost(m, wire::kDropByHost, now);
}

void HostCore::handle_hello(Member& m, const uint8_t* p, size_t n, int64_t now)
{
	wire::Hello h;
	wire::Result r = wire::decode(p, n, h);
	if (r == wire::Result::Malformed) {
		malformed(m, now);
		return;
	}
	auto reject = [&](uint8_t reason, const std::string& text) {
		std::vector<uint8_t> d;
		wire::encode(d, 0, wire::Reject{ reason, text });
		send_control(m.peer, d);
		m_env.log(fmt("peer %u refused: %s", m.id, text.c_str()));
		const uint32_t peer = m.peer;
		m.peer              = 0;
		m_env.disconnect(peer, reason);
		if (!m.known) {
			const uint32_t id = m.id;
			m_members.erase(std::remove_if(m_members.begin(), m_members.end(), [id](const Member& x) { return x.id == id && !x.self; }),
			                m_members.end());
		}
	};
	if (r != wire::Result::Ok) {
		reject(wire::kRejectProtocol, "Bad hello.");
		return;
	}
	if (h.proto != wire::kProto) {
		reject(wire::kRejectProtocol, "This game uses another version of the online protocol.");
		return;
	}
	if (h.member != m.id || h.secret != m.auth) {
		reject(wire::kRejectAuth, "The join secret is wrong.");
		return;
	}
	if (m_started) {
		reject(wire::kRejectStarted, "The game has already started.");
		return;
	}
	if (connected_count() >= m_p.maxMembers) {
		reject(wire::kRejectFull, "The lobby is full.");
		return;
	}
	m.connected  = true;
	m.spectator  = h.spectator || m.spectator;
	if (m.name == "Player") {
		// The lobby server's name wins; its default "Player" takes the Hello's name, cleaned like the
		// server does, and not one another member already shows.
		const std::string want = wire::clean_name(h.name);
		bool taken             = want.empty() || want == "Player";
		for (const Member& o : m_members) taken = taken || (o.id != m.id && o.name == want);
		if (!taken) m.name = want;
	}
	const bool buildSame   = std::memcmp(h.build, m_p.build, 32) == 0;
	const bool contentSame = std::memcmp(h.content, m_p.content, 32) == 0;
	m.contentOk = buildSame && contentSame;
	m.note      = content_note(m.name, m_p.mods, h.mods, buildSame, contentSame);
	if (!m.spectator && m.slot < 0) {
		for (int s = 0; s < m_seats; ++s) {
			if (seat_member(s) == 0xFFFFFFFFu) {
				m.slot = s;
				break;
			}
		}
	}
	std::vector<uint8_t> d;
	wire::encode(d, 0, wire::Welcome{ m.id, (uint8_t)(m.spectator ? 2 : 1) });
	send_control(m.peer, d);
	m_env.log(fmt("member %u (%s) joined the lobby as a %s%s", m.id, m.name.c_str(), m.spectator ? "spectator" : "player",
	              m.contentOk ? "" : " (content differs)"));
	CoreNote note;
	note.kind  = CoreNote::Notice;
	note.kind2 = 0;
	note.title = m.name + " joined";
	m_env.note(note);
	changed();
}

wire::Lobby HostCore::lobby_view() const
{
	wire::Lobby l;
	l.generation  = m_generation;
	l.phase       = m_started ? wire::kPhaseStarted : wire::kPhaseLobby;
	l.mode        = (uint8_t)m_p.mode;
	l.stage       = (uint8_t)m_p.stage;
	l.seats       = (uint8_t)m_seats;
	l.handicap[0] = m_p.handicap[0];
	l.handicap[1] = m_p.handicap[1];
	for (const Member& m : m_members) {
		if (!m.connected && !m.self) continue;
		if (m.gone) continue;
		wire::LobbyMember mm;
		mm.id    = m.id;
		mm.slot  = m.slot >= 0 ? (uint8_t)m.slot : kNoSlot;
		mm.flags = (m.ready ? wire::kMemReady : 0) | (m.spectator ? wire::kMemSpectator : 0) | (m.contentOk ? wire::kMemContentOk : 0) |
		           (m.self ? wire::kMemHost : 0) | (m.connected ? wire::kMemConnected : 0);
		mm.pingMs = m.self ? 0 : (m.sync.valid() ? (uint16_t)std::min<uint32_t>(m.sync.rtt_us() / 1000, 0xFFFE) : 0xFFFF);
		mm.delay  = m.self ? (uint8_t)m_delay.current() : 0;
		mm.link   = m.self ? wire::kLinkHost : m.link;
		mm.name   = m.name;
		mm.note   = m.note;
		l.members.push_back(mm);
	}
	return l;
}

void HostCore::broadcast_lobby()
{
	m_lobbyDirty = false;
	std::vector<uint8_t> d;
	wire::encode(d, m_started ? m_cfg.sessionTag : 0, lobby_view());
	send_all_control(d);
	CoreNote n;
	n.kind = CoreNote::LobbyChanged;
	m_env.note(n);
}

bool HostCore::can_start() const
{
	if (m_started || m_ended) return false;
	for (int s = 0; s < m_seats; ++s) {
		const uint32_t id = seat_member(s);
		const Member* m   = id == 0xFFFFFFFFu ? nullptr : by_id(id);
		if (!m || !m->connected || !m->ready || !m->contentOk) return false;
	}
	return true;
}

std::string HostCore::status_line() const
{
	if (m_started) return {}; // the in-game Online window says the game is running
	for (int s = 0; s < m_seats; ++s)
		if (seat_member(s) == 0xFFFFFFFFu) return m_seats == 2 ? "Waiting for another player." : "Waiting for more players.";
	for (int s = 0; s < m_seats; ++s) {
		const Member* m = by_id(seat_member(s));
		if (m && !m->contentOk) return m->note.empty() ? m->name + " has different content." : m->note;
	}
	for (int s = 0; s < m_seats; ++s) {
		const Member* m = by_id(seat_member(s));
		if (m && !m->ready) return m->self ? std::string("Get ready to start.") : "Waiting for " + m->name + " to get ready.";
	}
	return "Everyone is ready.";
}

void HostCore::set_config(Mode mode, int stage, int h0, int h1)
{
	if (m_started) return;
	const int maxStage = mode == Mode::Challenge ? 29 : 254;
	m_p.mode        = mode;
	m_p.stage       = std::clamp(stage, 0, maxStage);
	m_p.handicap[0] = (uint8_t)(mode == Mode::Battle ? std::clamp(h0, 0, 10) : 0);
	m_p.handicap[1] = (uint8_t)(mode == Mode::Battle ? std::clamp(h1, 0, 10) : 0);
	const int seats = mode_seats(mode);
	if (seats != m_seats) {
		m_seats = seats;
		// Members whose seat is gone (2v2 -> 2 seats) move to the first free seat, or watch when there is
		// none; every seat keeps one member. The host always plays: with no free seat it takes the
		// highest seat of a member that was seated automatically (else the highest seat), and that
		// member watches.
		const auto watching = [&](const Member& m) {
			m_env.log(fmt("member %u (%s) has no seat in %s; it watches", m.id, m.name.c_str(), mode_name(mode)));
			CoreNote note;
			note.kind  = CoreNote::Notice;
			note.kind2 = 0;
			note.title = m.name + " is watching";
			note.text  = "There's no free seat for them in this mode.";
			m_env.note(note);
		};
		std::vector<size_t> moved;
		for (size_t i = 0; i < m_members.size(); ++i) {
			Member& m = m_members[i];
			if (m.slot >= m_seats) {
				m.slot  = -1;
				m.ready = false;
				m.chose = false;
				moved.push_back(i);
			}
		}
		const auto first_free = [this]() {
			for (int s = 0; s < m_seats; ++s)
				if (seat_member(s) == 0xFFFFFFFFu) return s;
			return -1;
		};
		if (self().slot < 0) {
			int s = first_free();
			if (s < 0) {
				Member* bump = nullptr;
				for (Member& m : m_members) {
					if (m.self || m.gone || m.slot < 0) continue;
					if (!bump || (bump->chose && !m.chose) || (bump->chose == m.chose && m.slot > bump->slot)) bump = &m;
				}
				if (bump) {
					s           = bump->slot;
					bump->slot  = -1;
					bump->ready = false;
					bump->chose = false;
					watching(*bump);
				}
			}
			self().slot = s < 0 ? 0 : s;
		}
		for (const size_t i : moved) {
			Member& m = m_members[i];
			if (m.self || m.gone || m.spectator || !m.connected) continue;
			m.slot = first_free();
			if (m.slot < 0) watching(m);
		}
		m_hostSlot = self().slot;
	}
	for (Member& m : m_members)
		if (!m.self) m.ready = false; // a changed match asks everyone again
	changed();
}

void HostCore::set_ready(bool ready)
{
	if (m_started) return;
	if (self().slot < 0) ready = false;
	if (self().ready != ready) {
		self().ready = ready;
		changed();
	}
}

void HostCore::take_slot(int slot)
{
	if (m_started) return;
	if (slot < 0 || slot >= m_seats) return; // the host always plays
	if (seat_member(slot) != 0xFFFFFFFFu) return;
	self().slot  = slot;
	self().ready = false;
	m_hostSlot   = slot;
	changed();
}

void HostCore::kick(uint32_t id, int64_t now)
{
	Member* m = by_id(id);
	if (!m || m->self) return;
	m_banned.insert(id);
	// Its unverified connections go too.
	std::vector<uint32_t> pend;
	for (const auto& [peer, pp] : m_pending)
		if (pp.member == id) pend.push_back(peer);
	for (const uint32_t peer : pend) drop_pending(peer, wire::kRejectBanned, "The host removed you from the game.");
	if (m->peer) {
		std::vector<uint8_t> d;
		wire::encode(d, m_started ? m_cfg.sessionTag : 0, wire::End{ (uint8_t)kEndKicked, "The host removed you from the game." });
		send_control(m->peer, d);
	}
	member_lost(*m, wire::kDropKicked, now);
}

void HostCore::maybe_auto_start(int64_t now)
{
	if (!m_p.autoStart || m_started || m_ended || !can_start()) return;
	const int expect = m_p.expect > 0 ? m_p.expect : m_seats;
	if (connected_count() < expect) return;
	// Warm clocks first (8.2: the 30 Hz exchange before START): every seated member has reported its
	// downlink and has been connected for 1.5 s, so the first delays come from measured paths.
	for (const Member& m : m_members) {
		if (m.self || !m.connected || m.gone || m.slot < 0) continue;
		if (!m.downP99 || now - m.helloAt < 1500000000LL) return;
	}
	start(now);
}

bool HostCore::start(int64_t now)
{
	if (!can_start()) return false;
	m_cfg             = SessionConfig{};
	m_cfg.sessionTag  = m_p.sessionTag ? m_p.sessionTag : 1;
	m_cfg.seed        = m_p.seed;
	m_cfg.mode        = m_p.mode;
	m_cfg.stage       = (uint8_t)m_p.stage;
	m_cfg.seats       = (uint8_t)m_seats;
	m_cfg.slotMask    = (uint8_t)((1u << m_seats) - 1);
	for (int s = 0; s < kMaxSeats; ++s) m_cfg.slotMember[s] = s < m_seats ? seat_member(s) : 0xFFFFFFFFu;
	m_cfg.handicap[0] = m_p.handicap[0];
	m_cfg.handicap[1] = m_p.handicap[1];
	m_cfg.tickOrder   = m_p.tickOrder;
	m_cfg.simK        = m_p.simK;
	m_cfg.cksInterval = kCksInterval;
	m_cfg.maxDelay    = kMaxDelay;
	m_cfg.syncedJson  = m_p.syncedJson;
	m_cfg.syncedHash  = m_p.syncedHash;
	std::memcpy(m_cfg.build, m_p.build, 32);
	std::memcpy(m_cfg.content, m_p.content, 32);
	m_cfg.mods = m_p.mods;
	m_started  = true;
	m_hostSlot = self().slot;
	for (int s = 0; s < kMaxSeats; ++s) {
		m_seat[s]        = Seat{};
		m_seat[s].used   = s < m_seats;
		m_seat[s].member = m_cfg.slotMember[s];
	}
	// The host's own delay: from the measured paths (the others' downlinks), 2 when unknown.
	int d0 = m_p.initialDelay;
	if (d0 <= 0) {
		bool known = false;
		int64_t path = 0;
		for (const Member& m : m_members)
			if (!m.self && m.connected && !m.spectator && m.slot >= 0 && m.downP99) {
				known = true;
				path  = std::max<int64_t>(path, m.downP99 * 100);
			}
		d0 = known ? delay_for_path(path) : 2;
	}
	m_delay.init(d0, now);
	m_lastDelayEval = now;
	m_lastTag       = 0;
	m_prevInput     = Input{};
	for (uint32_t t = 1; t <= (uint32_t)d0; ++t) store_input(m_hostSlot, t, Input{}, now);
	m_lastTag = (uint32_t)d0;
	m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=init", m_hostSlot, d0, max_down_us()));
	{
		std::lock_guard<std::mutex> lk(m_env.link().mu);
		m_env.link().config    = m_cfg;
		m_env.link().localSeat = m_hostSlot;
	}
	std::vector<uint8_t> d;
	wire::encode(d, 0, m_cfg);
	send_all_control(d);
	N2G st;
	st.kind = N2G::Start;
	m_env.to_game(st);
	CoreNote n;
	n.kind = CoreNote::Started;
	m_env.note(n);
	std::string seats;
	for (int s = 0; s < m_seats; ++s) seats += fmt("%s%d:%u", s ? "," : "", s, m_cfg.slotMember[s]);
	m_env.log(fmt("start mode=%s stage=%u seats=%s seed=%u tag=%08x", mode_name(m_cfg.mode), (unsigned)m_cfg.stage, seats.c_str(), m_cfg.seed,
	              m_cfg.sessionTag));
	changed();
	return true;
}

// ---- in game ----------------------------------------------------------------------------------

void HostCore::store_input(int seat, uint32_t tag, const Input& in, int64_t now)
{
	if (seat < 0 || seat >= kMaxSeats) return;
	Seat& s = m_seat[seat];
	if (tag <= m_completed || tag > m_completed + kTagWindow) return;
	const size_t i = tag % s.in.size();
	if (s.tagOf[i] == tag) return; // already here (redundancy)
	s.in[i]    = in;
	s.tagOf[i] = tag;
	s.at[i]    = now;
	while (true) {
		const uint32_t next = s.contig + 1;
		if (next <= m_completed) {
			s.contig = m_completed; // completed bundles had this seat's input
			continue;
		}
		if (s.tagOf[next % s.in.size()] == next) s.contig = next;
		else break;
	}
}

int64_t HostCore::due_of(uint32_t tick) const
{
	if (m_due.empty()) return 0;
	for (auto it = m_due.rbegin(); it != m_due.rend(); ++it)
		if (it->first == tick) return it->second;
	const auto& last = m_due.back();
	if (tick > last.first) return last.second + (int64_t)(tick - last.first) * kTickNs;
	return 0;
}

int64_t HostCore::max_down_us() const
{
	int64_t mx = 0;
	for (const Member& m : m_members) {
		if (m.self || !m.connected || m.gone || m.spectator || m.slot < 0) continue;
		if (m_started && (m_seat[m.slot].dropped || m_cfg.slotMember[m.slot] != m.id)) continue;
		mx = std::max<int64_t>(mx, (int64_t)m.downP99 * 100);
	}
	return mx;
}

void HostCore::try_complete(int64_t now)
{
	if (!m_started || m_ended) return;
	const uint32_t before = m_completed;
	while (true) {
		const uint32_t x = m_completed + 1;
		std::array<Input, kMaxSeats> b{};
		bool ok        = true;
		int lastSeat   = -1;
		int64_t lastAt = 0;
		uint8_t wait   = 0;
		for (int s = 0; s < m_seats; ++s) {
			Seat& seat = m_seat[s];
			if (seat.dropped && seat.dropE <= x) continue; // neutral
			const size_t i = x % seat.in.size();
			if (seat.tagOf[i] != x) {
				ok = false;
				wait |= (uint8_t)(1u << s);
				continue;
			}
			b[s] = seat.in[i];
			if (seat.at[i] >= lastAt) {
				lastAt   = seat.at[i];
				lastSeat = s;
			}
		}
		if (!ok) {
			m_waitMask = wait;
			break;
		}
		m_waitMask = 0;
		m_bundles.push_back(b);
		m_completed = x;
		// Blame: the bundle is late when it cannot reach the farthest seat by the host's due time.
		// Not right after a barrier release: the ticks around it are due at the release time, which the
		// host's own due times do not show yet.
		const int64_t due = due_of(x);
		bool late         = due != 0 && (m_released == 0 || x > m_releaseTick + 2 * kMaxDelay) && now + max_down_us() * 1000 > due - 1000000;
		if (late && lastSeat == m_hostSlot && m_hostReadLate) late = false; // the host's own long tick (a load), not its link
		for (int s = 0; s < m_seats; ++s) {
			Seat& seat = m_seat[s];
			seat.blame.emplace_back(now, late && s == lastSeat && m_seats > 1);
			while (!seat.blame.empty() && now - seat.blame.front().first > kBlameWindow) seat.blame.pop_front();
		}
		N2G g;
		g.kind = N2G::Bundle;
		g.tick = x;
		g.mask = m_cfg.slotMask;
		for (int s = 0; s < kMaxSeats; ++s) g.seat[s] = b[s];
		m_env.to_game(g);
	}
	m_env.link().waitMask.store(m_waitMask, std::memory_order_relaxed);
	if (m_completed != before) {
		m_blockSince.fill(0); // progress: nobody has been holding the stream up
		for (Member& m : m_members) {
			if (m.self || !m.connected || m.gone || !m.peer || m.desynced) continue;
			send_bundle(m, now);
		}
	}
}

int HostCore::blame_permille(int seat, int64_t now)
{
	if (seat < 0 || seat >= kMaxSeats) return 0;
	Seat& s = m_seat[seat];
	while (!s.blame.empty() && now - s.blame.front().first > kBlameWindow) s.blame.pop_front();
	if (s.blame.empty()) return 0;
	int n = 0;
	for (const auto& b : s.blame) n += b.second ? 1 : 0;
	return (int)((int64_t)n * 1000 / (int64_t)s.blame.size());
}

void HostCore::send_bundle(Member& m, int64_t now)
{
	wire::BundlePkt b;
	b.tSendUs = us32(now);
	b.echoUs  = m.lastRecvTSend;
	b.holdUs  = m.lastRecvAt ? (uint32_t)((now - m.lastRecvAt) / 1000) : 0;
	if (!m_due.empty()) {
		b.hostTick  = m_due.back().first;
		b.hostDueUs = us32(m_due.back().second);
	}
	b.slotMask = m_started ? m_cfg.slotMask : 0;
	if (m_started && m_completed > m.ackBundle) {
		uint32_t first = m.ackBundle + 1;
		uint32_t count = std::min<uint32_t>(m_completed - m.ackBundle, kMaxWindow);
		b.firstTick    = first;
		b.count        = (uint8_t)count;
		for (uint32_t i = 0; i < count; ++i)
			for (int s = 0; s < kMaxSeats; ++s) b.bundles[i][s] = m_bundles[first + i - 1][s];
		for (const wire::Event& e : m_events) {
			if (e.tick >= first && e.tick < first + count && b.nEvents < wire::kMaxEvents) b.events[b.nEvents++] = e;
		}
	} else {
		b.firstTick = m_completed + 1;
	}
	if (m_released) {
		b.flags |= wire::kBundleRelease;
		b.releaseTick   = m_releaseTick;
		b.releaseHostUs = us32(m_releaseNs);
		b.releaseCount  = m_released;
	}
	b.waitMask = m_waitMask;
	for (int s = 0; s < kMaxSeats; ++s) {
		if (!m_started || s >= m_seats) continue;
		const Member* sm = by_id(m_cfg.slotMember[s]);
		if (!sm) continue;
		if (sm->self) {
			b.upP99[s] = 0;
			b.downP99[s] = 0;
		} else {
			Member* mm = by_id(sm->id);
			b.upP99[s]   = mm->up.valid() ? (uint8_t)std::clamp<int32_t>((mm->up.p99(now) + 999) / 1000, 0, 254) : 255;
			b.downP99[s] = mm->downP99 ? (uint8_t)std::clamp<int>((mm->downP99 + 9) / 10, 0, 254) : 255;
		}
		b.blame[s] = (uint8_t)std::min(255, blame_permille(s, now) / 4);
	}
	std::vector<uint8_t> d;
	wire::encode(d, m_started ? m_cfg.sessionTag : 0, b);
	m_bytesOut += d.size();
	m_env.send(m.peer, false, d, true);
	m.lastSend = now;
	m.dupAt    = now + kDupNs;
}

void HostCore::host_input(uint32_t t, const Input& in, int64_t now)
{
	if (!m_started || m_ended) return;
	const uint32_t desired = t + (uint32_t)m_delay.current();
	if (desired > m_lastTag) {
		for (uint32_t g = m_lastTag + 1; g < desired; ++g) store_input(m_hostSlot, g, m_prevInput, now);
		store_input(m_hostSlot, desired, in, now);
		m_lastTag = desired;
	}
	m_prevInput = in;
	try_complete(now);
}

uint8_t HostCore::loading_mask(uint16_t n) const
{
	uint8_t loading = 0;
	for (int s = 0; s < m_seats; ++s) {
		if (m_seat[s].dropped) continue;
		const Member* m = by_id(m_cfg.slotMember[s]);
		if (!m || m->gone) continue;
		if (m->loadCount < n) loading |= (uint8_t)(1u << s);
	}
	return loading;
}

// Seats whose game is stuck while their net thread still runs (its keepalive INPUTs keep the
// connection and the 8 s packet timeout alive: a hung game thread, the crash screen, a boot that
// never reaches the inject). Judged by progress, not by packets:
// - mid-match, a seat whose input the host's own game has been waiting for 10 s (it holds for the
//   tick and the bundle lacks only other seats' input; a host that is loading or in a long tick of
//   its own waits for nothing, so nobody is timed then), unless the host waits at a load barrier;
// - at a load barrier, a seat still loading 30 s (the start barrier: 60 s) after the host got there.
//   (At the start barrier nobody has sent the first tags yet, so the first rule would misjudge it.)
// A seat whose member already said BYE after its last tick (--exit-after-ticks, not dropped then)
// goes after 1 s of holding the stream up. Not while the host lingers after its own last tick.
void HostCore::check_stuck(int64_t now)
{
	if (!m_started || m_ended || m_lingering || m_desyncEndAt) return;
	const uint8_t hostBit = (m_hostSlot >= 0 && m_hostSlot < kMaxSeats) ? (uint8_t)(1u << m_hostSlot) : 0;
	const uint16_t n      = (uint16_t)(m_released + 1);
	const auto at         = m_barrierAt.find(n); // the host waits at barrier n: the load rule below judges
	const bool atBarrier  = at != m_barrierAt.end();
	// The host's game waits for a bundle it cannot get: it read the tick (the fallback hold) or holds
	// in the stall loop before the read (the usual case: the late-sampling wait needs the tick ready).
	const uint32_t want   = std::max(m_hostRead, m_env.link().gameWait.load(std::memory_order_relaxed));
	for (int s = 0; s < m_seats; ++s) {
		const uint8_t bit = (uint8_t)(1u << s);
		if (s == m_hostSlot || m_seat[s].dropped || !(m_waitMask & bit) || (m_waitMask & hostBit) || want <= m_completed ||
		    atBarrier) {
			m_blockSince[s] = 0;
			continue;
		}
		if (!m_blockSince[s]) {
			m_blockSince[s] = now;
			continue;
		}
		Member* m       = by_id(m_cfg.slotMember[s]);
		const bool gone = !m || m->gone;
		if (now - m_blockSince[s] <= (gone ? kGoneStuckNs : kStuckNs)) continue;
		m_blockSince[s] = 0;
		if (gone) {
			m_env.log(fmt("seat %d ended its session before the host's last tick", s));
			drop_seat(s, wire::kDropLeft, now);
		} else {
			m_env.log(fmt("peer %u made no progress for %d s", m->id, (int)(kStuckNs / 1000000000LL)));
			member_lost(*m, wire::kDropTimeout, now);
		}
		return; // one per pass: the drop changed the masks
	}
	if (!atBarrier) return; // the host itself has not reached it
	const int64_t limit = n == 1 ? kStartLoadNs : kLoadNs;
	if (now - at->second <= limit) return;
	const uint8_t loading = loading_mask(n);
	for (int s = 0; s < m_seats; ++s) {
		if (!(loading & (1u << s)) || s == m_hostSlot) continue;
		Member* m = by_id(m_cfg.slotMember[s]);
		if (!m) continue;
		m_env.log(fmt("peer %u did not finish loading within %d s", m->id, (int)(limit / 1000000000LL)));
		member_lost(*m, wire::kDropTimeout, now);
		return;
	}
}

void HostCore::try_release(int64_t now)
{
	if (!m_started || m_ended) return;
	while (true) {
		const uint16_t n = (uint16_t)(m_released + 1);
		auto it          = m_barrierTick.find(n);
		if (it == m_barrierTick.end()) return; // the host itself has not reached it
		const uint8_t loading = loading_mask(n);
		m_env.link().loadingMask.store(loading, std::memory_order_relaxed);
		if (loading) return;
		const int64_t lead = std::clamp<int64_t>(max_down_us() * 1500 + 10000000, 30000000, 300000000);
		m_released    = n;
		m_releaseTick = it->second;
		m_releaseNs   = now + lead;
		std::vector<uint8_t> d;
		wire::encode(d, m_cfg.sessionTag, wire::Release{ m_releaseTick, n, (uint64_t)m_releaseNs });
		send_all_control(d);
		N2G g;
		g.kind  = N2G::Release;
		g.tick  = m_releaseTick;
		g.count = n;
		g.ns    = m_releaseNs;
		m_env.to_game(g);
		if (m_delay.boundary() == DelayPolicy::Boundary)
			m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=boundary", m_hostSlot, m_delay.current(), max_down_us()));
	}
}

void HostCore::drop_seat(int seat, uint8_t reason, int64_t now)
{
	if (!m_started || seat < 0 || seat >= m_seats || m_seat[seat].dropped) return;
	if (seat == m_hostSlot) return;
	Seat& s       = m_seat[seat];
	const uint32_t e = std::max(s.contig, m_completed) + 1;
	s.dropped     = true;
	s.dropE       = e;
	wire::Event ev;
	ev.tick = e;
	ev.id   = 1;
	ev.arg  = (uint8_t)seat;
	ev.seq  = ++m_eventSeq;
	m_events.push_back(ev);
	std::vector<uint8_t> d;
	wire::encode(d, m_cfg.sessionTag, wire::Drop{ (uint8_t)seat, e, reason });
	send_all_control(d);
	m_env.log(fmt("drop slot=%d tick=%u reason=%s", seat, e, drop_reason_name(reason)));
	N2G g;
	g.kind = N2G::Drop;
	g.tick = e;
	g.mask = (uint8_t)seat;
	m_env.to_game(g);
	CoreNote n;
	n.kind  = CoreNote::Notice;
	n.kind2 = 2;
	n.title = member_name(s.member) + " left the game";
	n.text  = "Their captain stands still for the rest of the match.";
	m_env.note(n);
	try_complete(now);
	try_release(now);
}

void HostCore::drop_slot(int slot, int64_t now)
{
	if (!m_started || slot < 0 || slot >= m_seats || slot == m_hostSlot) return;
	Member* m = by_id(m_cfg.slotMember[slot]);
	if (m && !m->gone) {
		if (m->peer) {
			std::vector<uint8_t> d;
			wire::encode(d, m_cfg.sessionTag, wire::End{ (uint8_t)kEndKicked, "The host removed you from the game." });
			send_control(m->peer, d);
		}
		member_lost(*m, wire::kDropByHost, now);
	} else {
		drop_seat(slot, wire::kDropByHost, now);
	}
}

void HostCore::end(int reason, const std::string& logReason, const std::string& text)
{
	if (m_ended) return;
	m_ended = true;
	{
		std::lock_guard<std::mutex> lk(m_env.link().mu);
		EndInfo& e  = m_env.link().end;
		if (reason == kEndDesync) e = m_pendingEnd;
		e.reason    = reason;
		e.logReason = logReason;
		if (e.text.empty()) e.text = text;
	}
	N2G g;
	g.kind = N2G::End;
	m_env.to_game(g);
	CoreNote n;
	n.kind = CoreNote::Ended;
	m_env.note(n);
}

void HostCore::leave(int64_t now)
{
	(void)now;
	if (m_ended) return;
	std::vector<uint8_t> d;
	wire::encode(d, m_started ? m_cfg.sessionTag : 0, wire::End{ (uint8_t)kEndHostLeft, "The host left the game." });
	send_all_control(d);
	end(kEndLeft, "left", "You left the game.");
}

void HostCore::compare_cks(Member& m, int64_t now)
{
	if (m.cksTick == 0 || m.cksTick == m.cksDoneTick || m.desynced) return;
	auto it = m_ownCks.find(m.cksTick);
	if (it == m_ownCks.end()) return; // the host has not computed it yet
	m.cksDoneTick = m.cksTick;
	if (it->second != m.cksTotal) desync(m, m.cksTick, it->second, m.cksTotal, now);
}

void HostCore::desync(Member& m, uint32_t tick, uint32_t hostTotal, uint32_t peerTotal, int64_t now)
{
	const bool spectator = m.spectator || m.slot < 0 || m_cfg.slotMember[m.slot] != m.id;
	wire::Desync ds{ tick, spectator ? kNoSlot : (uint8_t)m.slot, hostTotal, peerTotal };
	std::vector<uint8_t> d;
	wire::encode(d, m_cfg.sessionTag, ds);
	if (spectator) {
		m.desynced = true;
		send_control(m.peer, d);
		m_env.log(fmt("spectator %u (%s) desynced at tick %u (host %08x, spectator %08x); it stops", m.id, m.name.c_str(), tick, hostTotal, peerTotal));
		return;
	}
	if (m_desyncEndAt) return;
	send_all_control(d);
	std::string hostLine;
	for (const auto& l : m_ownLines)
		if (l.first == tick) hostLine = l.second;
	if (!hostLine.empty()) {
		std::vector<uint8_t> c;
		wire::encode(c, m_cfg.sessionTag, wire::CksLine{ tick, hostLine });
		send_all_control(c);
	}
	m_pendingEnd            = EndInfo{};
	m_pendingEnd.desyncTick = tick;
	m_pendingEnd.desyncSlot = (uint8_t)m.slot;
	m_pendingEnd.hostTotal  = hostTotal;
	m_pendingEnd.peerTotal  = peerTotal;
	m_pendingEnd.hostLine   = hostLine;
	m_pendingEnd.text       = fmt("Desync at tick %u. The match was stopped on every machine.", tick);
	m_desyncEndAt           = now + kDesyncWaitNs;
	for (Member& o : m_members) o.desynced = o.desynced || !o.self;
}

void HostCore::handle_input(Member& m, const wire::InputPkt& in, int64_t recvNs, int64_t now)
{
	// Well-formed INPUTs are capped too: a client sends about 120 a second (one per tick and a
	// duplicate), each over the cap counts as invalid, and the time-sync estimators (windowed, their
	// cost grows with the sample rate) take at most one sample every 2 ms.
	if (now - m.inputWinAt > 1000000000LL) {
		m.inputWinAt = now;
		m.inputWin   = 0;
	}
	if (++m.inputWin > kInputRateLimit) {
		invalid(m, now);
		return;
	}
	m.lastPacket = now;
	const bool sample = now - m.lastSampleAt >= kSampleGapNs;
	if (sample) m.lastSampleAt = now;
	if (sample && in.echoUs != 0) m.sync.add(now, in.echoUs, in.tSendUs - in.holdUs, in.tSendUs, us32(recvNs));
	m.lastRecvTSend = in.tSendUs;
	m.lastRecvAt    = recvNs;
	if (sample && m.sync.valid()) m.up.add(now, us_diff(us32(recvNs), m.sync.to_local(in.tSendUs)));
	if (in.downP99) m.downP99 = in.downP99;
	if (!m_started) return;
	if (in.slot != kNoSlot) m.delay = in.delay;
	const uint32_t ack = std::min(in.ackBundle, m_completed);
	const bool behind  = ack < m_completed && ack <= m.ackBundle;
	if (ack > m.ackBundle) m.ackBundle = ack;
	if (in.loadCount > m.loadCount) {
		m.loadCount = in.loadCount;
		try_release(now);
	}
	if (in.cksTick && (in.cksTick != m.cksTick || in.cksTotal != m.cksTotal)) {
		m.cksTick  = in.cksTick;
		m.cksTotal = in.cksTotal;
		compare_cks(m, now);
	}
	if (in.count > 0) {
		const bool seated = !m.spectator && m.slot >= 0 && m.slot == in.slot && m_cfg.slotMember[m.slot] == m.id && !m_seat[m.slot].dropped;
		if (!seated) {
			invalid(m, now);
			return;
		}
		m.lastInput = now;
		for (int i = 0; i < in.count; ++i) {
			const uint32_t tag = in.firstTag + (uint32_t)i;
			if (tag > m_completed + kTagWindow) {
				invalid(m, now);
				break;
			}
			store_input(m.slot, tag, in.inputs[i], now);
		}
		try_complete(now);
	} else if (!m.spectator) {
		m.lastInput = now;
	}
	if (behind && m.peer && !m.desynced && now - m.lastSend > 8000000) send_bundle(m, now);
}

void HostCore::on_datagram(uint32_t peer, const uint8_t* p, size_t n, int64_t recvNs, int64_t now)
{
	Member* m = by_peer(peer);
	if (!m) {
		// A peer whose Hello is not verified yet: only a Hello is read from it.
		auto it = m_pending.find(peer);
		if (it == m_pending.end()) return;
		m_bytesIn += n;
		wire::Header ph;
		const wire::Result pr = wire::decode_header(p, n, ph);
		if (pr == wire::Result::Malformed) return drop_pending(peer, wire::kRejectProtocol, "Bad hello.");
		if (pr != wire::Result::Ok || ph.type != wire::HELLO || !it->second.hello.empty()) return;
		it->second.hello.assign(p, p + n);
		pending_hello(peer, now);
		return;
	}
	m_bytesIn += n;
	wire::Header h;
	wire::Result r = wire::decode_header(p, n, h);
	if (r == wire::Result::Malformed) {
		malformed(*m, now);
		return;
	}
	if (r != wire::Result::Ok) {
		invalid(*m, now);
		return;
	}
	if (h.tag != 0 && (!m_started || h.tag != m_cfg.sessionTag)) return; // another session's datagram
	m->lastPacket = now;
	switch (h.type) {
	case wire::HELLO:
		if (m->connected) {
			invalid(*m, now);
			return;
		}
		handle_hello(*m, p, n, now);
		return;
	case wire::REQ: {
		wire::Req q;
		r = wire::decode(p, n, q);
		if (r == wire::Result::Malformed) return malformed(*m, now);
		if (r != wire::Result::Ok || !m->connected) return invalid(*m, now);
		if (q.what == wire::kReqLeave) {
			member_lost(*m, wire::kDropLeft, now);
			return;
		}
		if (m_started) return;
		if (q.what == wire::kReqSlot) {
			if (m->spectator) return;
			// A refused or no-op request changes nothing and must not bump the lobby: a client with a
			// --slot wish repeats its REQ on every LOBBY it gets, so a broadcast per refusal would loop.
			bool moved = false;
			if (q.value < 0) {
				moved    = m->slot >= 0 || m->chose || m->ready;
				m->slot  = -1;
				m->chose = false;
				m->ready = false;
			} else if (q.value < m_seats && q.value != m->slot) {
				const uint32_t occ = seat_member(q.value);
				Member* o          = occ == 0xFFFFFFFFu ? nullptr : by_id(occ);
				if (!o) {
					m->slot  = q.value;
					m->chose = true;
					m->ready = false;
					moved    = true;
				} else if (!o->self && !o->chose && !o->ready) {
					// The occupant was seated automatically: swap (several --slot requests in any
					// join order would otherwise block each other).
					o->slot  = m->slot;
					o->ready = false;
					m->slot  = q.value;
					m->chose = true;
					m->ready = false;
					moved    = true;
				}
			}
			if (moved) changed();
		} else if (q.what == wire::kReqReady) {
			const bool ready = q.value != 0 && m->slot >= 0 && m->contentOk;
			if (ready != m->ready) {
				m->ready = ready;
				changed();
			}
		}
		return;
	}
	case wire::CKSLINE: {
		wire::CksLine c;
		r = wire::decode(p, n, c);
		if (r == wire::Result::Malformed) return malformed(*m, now);
		if (r != wire::Result::Ok) return invalid(*m, now);
		if (m_desyncEndAt && c.tick == m_pendingEnd.desyncTick)
			m_pendingEnd.peerLines.push_back(fmt("%s (seat %d): %s", m->name.c_str(), m->slot, c.line.c_str()));
		return;
	}
	case wire::BYE: {
		wire::Bye b;
		r = wire::decode(p, n, b);
		if (r == wire::Result::Malformed) return malformed(*m, now);
		m_env.log(fmt("peer %u said bye (reason %u)", m->id, (unsigned)b.reason));
		if (b.reason == wire::kByeDone) m->byeDone = true; // it reached its last tick: the session is over for it
		member_lost(*m, wire::kDropLeft, now);
		return;
	}
	case wire::INPUT: {
		wire::InputPkt in;
		r = wire::decode(p, n, in);
		if (r == wire::Result::Malformed) return malformed(*m, now);
		if (r != wire::Result::Ok || !m->connected) return invalid(*m, now);
		handle_input(*m, in, recvNs, now);
		return;
	}
	default:
		invalid(*m, now); // a host-only message type from a member
		return;
	}
}

void HostCore::on_game(const G2N& g, int64_t now)
{
	switch (g.kind) {
	case G2N::LocalInput:
		host_input(g.tick, g.input, now);
		break;
	case G2N::TickDue:
		m_hostRead = std::max(m_hostRead, g.tick);
		if (g.ns != 0) {
			m_due.emplace_back(g.tick, g.ns);
			while (m_due.size() > 64) m_due.pop_front();
		}
		m_hostReadLate = g.ns == 0 || now > g.ns;
		break;
	case G2N::Checksum: {
		m_ownCks[g.tick] = g.total;
		while (m_ownCks.size() > kCksKeep) m_ownCks.erase(m_ownCks.begin());
		m_ownLines.emplace_back(g.tick, std::string(g.line));
		while (m_ownLines.size() > 64) m_ownLines.pop_front();
		for (Member& m : m_members)
			if (!m.self && m.connected && !m.gone) compare_cks(m, now);
		break;
	}
	case G2N::Loaded:
		self().loadCount = (uint16_t)g.count;
		m_barrierTick[(uint16_t)g.count] = g.tick;
		m_barrierAt.emplace((uint16_t)g.count, now);
		try_release(now);
		break;
	case G2N::Begun:
		break;
	case G2N::Leave:
		leave(now);
		break;
	case G2N::Done:
		if (!m_lingering) {
			m_lingering   = true;
			m_lingerTick  = g.tick;
			m_lingerUntil = now + kLingerNs;
		}
		break;
	}
}

void HostCore::tick(int64_t now)
{
	if (m_desyncEndAt && now >= m_desyncEndAt && !m_ended) {
		m_desyncEndAt = 0;
		end(kEndDesync, "desync", m_pendingEnd.text);
	}
	// Connections whose verified Hello did not come within 5 s (none, or no MemberJoined for it).
	std::vector<uint32_t> late;
	for (const auto& [peer, pp] : m_pending)
		if (now - pp.at > kHelloWaitNs) late.push_back(peer);
	for (const uint32_t peer : late) drop_pending(peer, wire::kRejectAuth, "no verified hello within 5 s");
	if (m_lobbyDirty) broadcast_lobby();
	if (!m_started) {
		maybe_auto_start(now);
		if (now - m_lastKeepalive >= kKeepaliveNs) {
			m_lastKeepalive = now;
			for (Member& m : m_members)
				if (!m.self && m.connected && !m.gone && m.peer) send_bundle(m, now);
		}
		return;
	}
	if (m_ended) return;
	for (Member& m : m_members) {
		if (m.self || !m.connected || m.gone || !m.peer || m.desynced) continue;
		if ((m.dupAt && now >= m.dupAt) || now - m.lastSend >= kKeepaliveNs) {
			send_bundle(m, now);
			if (m.dupAt && now - m.lastSend < kDupNs) m.dupAt = 0; // one duplicate per send
		}
		if (!m.spectator && m.slot >= 0 && m.lastInput && now - m.lastInput > kInputTimeout && !m_seat[m.slot].dropped &&
		    m_cfg.slotMember[m.slot] == m.id) {
			m_env.log(fmt("peer %u sent no input for 8 s", m.id));
			member_lost(m, wire::kDropTimeout, now);
			break;
		}
	}
	for (Member& m : m_members) {
		if (m.lobbyGoneAt && !m.gone && now - m.lastPacket >= kLobbyGoneGraceNs) {
			member_lost(m, m.lobbyGoneReason, now);
			break;
		}
	}
	check_stuck(now);
	// Our own delay, once per second: path = the slowest other seat's downlink.
	if (now - m_lastDelayEval >= 1000000000LL) {
		m_lastDelayEval = now;
		const int before = m_delay.current();
		const DelayPolicy::Change c = m_delay.evaluate(now, max_down_us(), blame_permille(m_hostSlot, now));
		if (c != DelayPolicy::None && m_delay.current() != before)
			m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=%s", m_hostSlot, m_delay.current(), max_down_us(),
			              c == DelayPolicy::Stall ? "stall" : "path"));
		if (m_delay.capped() && !m_cappedNoted) { // D13: the wish above 8 is shown as a warning, once
			m_cappedNoted = true;
			m_env.log(fmt("delay slot=%d d=%d path_us=%" PRId64 " reason=path capped", m_hostSlot, m_delay.current(), max_down_us()));
			CoreNote n;
			n.kind  = CoreNote::Notice;
			n.kind2 = 2; // online::Notice::Warning
			n.title = "A connection is slow";
			n.text  = "The input delay is held at its limit of 8 ticks, so the game may pause now and then.";
			m_env.note(n);
		}
	}
	if (m_lingering && !m_lingerDone) {
		bool all = true;
		for (const Member& m : m_members)
			if (!m.self && m.connected && !m.gone && m.peer && !m.desynced && m.ackBundle < m_lingerTick) all = false;
		if (all || now >= m_lingerUntil) {
			m_lingerDone = true;
			std::vector<uint8_t> d;
			wire::encode(d, m_cfg.sessionTag, wire::End{ (uint8_t)kEndDone, "The match is over." });
			send_all_control(d);
			if (!all) m_env.log("linger: not every peer acknowledged the last tick within 10 s");
		}
	}
}

} // namespace mppm::session
