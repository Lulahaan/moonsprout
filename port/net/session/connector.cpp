// M5.3b connector (see connector.hpp).
#include "connector.hpp"
#include "wire.hpp"

#include <cstdio>

namespace mppm::session {

namespace {
constexpr int64_t kAttemptNs = 1500000000LL;
constexpr int64_t kTimeoutNs = 15000000000LL;

std::vector<std::string> split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i <= s.size()) {
		size_t j = s.find(sep, i);
		if (j == std::string::npos) j = s.size();
		if (j > i) out.push_back(s.substr(i, j - i));
		i = j + 1;
	}
	return out;
}
} // namespace

const char* net_mode_name(NetMode m)
{
	switch (m) {
	case NetMode::Direct: return "direct";
	case NetMode::Ice: return "ice";
	case NetMode::Relay: return "relay";
	default: return "auto";
	}
}

bool parse_net_mode(const std::string& s, NetMode& out)
{
	if (s == "auto") out = NetMode::Auto;
	else if (s == "direct") out = NetMode::Direct;
	else if (s == "ice") out = NetMode::Ice;
	else if (s == "relay") out = NetMode::Relay;
	else return false;
	return true;
}

Connector::Connector(net::Transport& t, NetMode mode, bool host, uint32_t self, SignalFn signal, LogFn log)
    : m_t(t), m_mode(mode), m_effective(mode), m_host(host), m_self(self), m_signal(std::move(signal)), m_log(std::move(log))
{
}

void Connector::set_lobby(const std::vector<net::IceServer>& ice, const net::RelayInfo& relay)
{
	m_ice          = ice;
	m_relay        = relay;
	m_relayOffered = !relay.ipPort.empty();
	if (m_mode == NetMode::Auto) {
		if (!m_ice.empty()) m_effective = NetMode::Ice;
		else m_effective = NetMode::Auto; // direct, then the relay when offered
	}
}

void Connector::host_created(int64_t now)
{
	(void)now;
	// The host always registers with the builtin relay when one is offered (D6).
	if (m_relayOffered) m_t.relay_register(m_relay.ipPort, m_relay.token);
}

void Connector::member_joined(uint32_t member, int64_t now)
{
	(void)now;
	if (m_effective == NetMode::Ice || m_effective == NetMode::Relay) return;
	std::string list;
	for (const std::string& a : m_t.local_addresses()) list += (list.empty() ? "" : ";") + a;
	m_signal(member, "addr", "direct " + list);
}

void Connector::member_left(uint32_t member)
{
	for (auto it = m_icePeers.begin(); it != m_icePeers.end();) {
		if (it->second == member) {
			// An agent that never carried a connection is released here (the host core disconnects
			// the ones that did); otherwise it would hold its socket and agent id until the lobby ends.
			if (!m_links.count(it->first)) m_t.reset(it->first);
			it = m_icePeers.erase(it);
		} else {
			++it;
		}
	}
}

void Connector::client_joined(int64_t now)
{
	m_joined   = true;
	m_joinedAt = now;
	start_client_path(now);
}

void Connector::start_ice_client()
{
	m_icePeer = m_t.ice_create(true, m_self);
	if (!m_icePeer) {
		m_log("ice: could not create an agent");
		return;
	}
	m_signal(0, "desc", m_t.ice_local_description(m_icePeer));
	m_t.ice_gather(m_icePeer);
}

void Connector::start_client_path(int64_t now)
{
	(void)now;
	switch (m_effective) {
	case NetMode::Ice:
		start_ice_client();
		break;
	case NetMode::Relay:
		if (!m_relayOffered) {
			m_log("relay: the lobby server offers no relay");
			break;
		}
		m_relayPending = true;
		m_t.relay_register(m_relay.ipPort, m_relay.token);
		break;
	default:
		// Direct (and auto): wait for the host's "addr direct" signal; with a relay offered, register
		// now so the public address and the relay port are known when the direct addresses fail.
		if (m_effective == NetMode::Auto && m_relayOffered) m_t.relay_register(m_relay.ipPort, m_relay.token);
		break;
	}
}

void Connector::try_next_attempt(int64_t now)
{
	if (m_attemptPeer) {
		m_t.reset(m_attemptPeer);
		m_attemptPeer = 0;
	}
	if (m_next < m_attempts.size()) {
		const Attempt& a = m_attempts[m_next++];
		if (a.punch) m_t.punch(a.addr, 10);
		m_attemptPeer = m_t.connect(a.addr, m_self);
		m_attemptAt   = now;
		return;
	}
	m_directDone = true;
	if (m_effective == NetMode::Auto && m_relayOffered) {
		m_relayPending = true;
		if (!m_relayPort.empty()) {
			m_attemptPeer = m_t.connect(m_relayPort, m_self);
			m_attemptAt   = now;
			m_relayPending = false;
		}
	}
}

void Connector::on_signal(uint32_t from, const std::string& kind, const std::string& data, int64_t now)
{
	if (m_host) {
		if (kind == "desc") {
			uint32_t peer = 0;
			for (const auto& [p, m] : m_icePeers)
				if (m == from) peer = p;
			if (!peer) {
				peer = m_t.ice_create(false, from);
				if (!peer) return;
				m_icePeers[peer] = from;
			}
			m_t.ice_set_remote_description(peer, data);
			m_signal(from, "desc", m_t.ice_local_description(peer));
			m_t.ice_gather(peer);
		} else if (kind == "cand" || kind == "done") {
			for (const auto& [p, m] : m_icePeers) {
				if (m != from) continue;
				if (kind == "cand") m_t.ice_add_remote_candidate(p, data);
				else m_t.ice_set_remote_gathering_done(p);
			}
		} else if (kind == "addr" && data.rfind("public ", 0) == 0) {
			// A client behind NAT: punch towards its public address, answer with ours. Once per member
			// (section 10: a member cannot make the host send datagrams to addresses of its choosing
			// over and over).
			if (!m_punched.insert(from).second) return;
			m_t.punch(data.substr(7), 10);
			if (!m_publicAddr.empty()) m_signal(from, "addr", "public " + m_publicAddr);
		}
		return;
	}
	// Client (only the host signals us).
	if (from != 0) return;
	if (kind == "addr" && data.rfind("direct ", 0) == 0) {
		if (m_haveHostAddrs || m_connectedPeer) return;
		m_haveHostAddrs = true;
		for (const std::string& a : split(data.substr(7), ';')) m_attempts.push_back({ a, false });
		if (m_effective == NetMode::Direct || m_effective == NetMode::Auto) try_next_attempt(now);
	} else if (kind == "addr" && data.rfind("public ", 0) == 0) {
		if (!m_connectedPeer) m_attempts.push_back({ data.substr(7), true });
		if (m_directDone && !m_connectedPeer && !m_attemptPeer) try_next_attempt(now);
	} else if (kind == "desc" && m_icePeer) {
		m_t.ice_set_remote_description(m_icePeer, data);
	} else if (kind == "cand" && m_icePeer) {
		m_t.ice_add_remote_candidate(m_icePeer, data);
	} else if (kind == "done" && m_icePeer) {
		m_t.ice_set_remote_gathering_done(m_icePeer);
	}
}

void Connector::on_event(const net::Event& e, int64_t now)
{
	using K = net::Event::Kind;
	switch (e.kind) {
	case K::IceCandidate: {
		uint32_t to = 0;
		if (m_host) {
			auto it = m_icePeers.find(e.peer);
			if (it == m_icePeers.end()) return;
			to = it->second;
		}
		m_signal(to, "cand", e.text);
		break;
	}
	case K::IceGatheringDone: {
		uint32_t to = 0;
		if (m_host) {
			auto it = m_icePeers.find(e.peer);
			if (it == m_icePeers.end()) return;
			to = it->second;
		}
		m_signal(to, "done", "");
		break;
	}
	case K::IceState:
		if (e.text == "failed") {
			m_log("ice: connection failed for peer " + std::to_string(e.peer));
			// Host: release the failed agent unless it carries a connection (its Disconnected does that).
			if (m_host && !m_links.count(e.peer) && m_icePeers.erase(e.peer)) m_t.reset(e.peer);
		}
		break;
	case K::RelayRegistered:
		m_publicAddr = e.aux;
		if (e.text != "host") {
			m_relayPort = e.text;
			if (!m_host && !m_connectedPeer) {
				if (m_effective == NetMode::Auto && !m_publicAddr.empty()) m_signal(0, "addr", "public " + m_publicAddr);
				if (m_relayPending && !m_attemptPeer) {
					m_relayPending = false;
					m_attemptPeer  = m_t.connect(m_relayPort, m_self);
					m_attemptAt    = now;
				}
			}
		}
		break;
	case K::RelayFailed:
		m_log("relay: registration failed: " + e.text);
		break;
	case K::Connected: {
		net::PeerStats st;
		uint8_t link = wire::kLinkDirect;
		std::string remote;
		if (m_t.stats(e.peer, st)) {
			link   = st.link == net::Link::Ice ? wire::kLinkIce : st.link == net::Link::Relay ? wire::kLinkRelay : wire::kLinkDirect;
			remote = st.remote;
		}
		if (!m_host) {
			if (m_connectedPeer) {
				if (e.peer != m_connectedPeer) m_t.reset(e.peer); // a late attempt: keep the first connection
				return;
			}
			m_connectedPeer = e.peer;
			m_everConnected = true;
			if (m_attemptPeer && m_attemptPeer != e.peer) m_t.reset(m_attemptPeer);
			m_attemptPeer = 0;
		}
		m_links[e.peer] = link;
		m_lastLink      = link_name(e.peer);
		const uint32_t member = m_host ? e.connectData : 0;
		m_log("peer " + std::to_string(member) + " connected link=" + link_name(e.peer) + " remote=" + (remote.empty() ? "?" : remote));
		break;
	}
	case K::Disconnected:
		if (!m_host && e.peer == m_attemptPeer) {
			m_attemptPeer = 0; // the attempt failed: the next one starts in tick()
		}
		if (e.peer == m_connectedPeer) m_connectedPeer = 0;
		m_links.erase(e.peer);
		break;
	default:
		break;
	}
}

void Connector::tick(int64_t now)
{
	if (m_host || !m_joined || m_connectedPeer || m_everConnected || m_timedOut) return;
	if (now - m_joinedAt > kTimeoutNs) {
		m_timedOut = true;
		if (m_attemptPeer) m_t.reset(m_attemptPeer);
		m_attemptPeer = 0;
		m_log("could not connect to the host within 15 s");
		return;
	}
	const bool directish = m_effective == NetMode::Direct || m_effective == NetMode::Auto;
	if (directish && m_haveHostAddrs && (!m_attemptPeer || now - m_attemptAt > kAttemptNs) && (!m_directDone || m_next < m_attempts.size()))
		try_next_attempt(now);
}

const char* Connector::link_name(uint32_t peer) const
{
	switch (link_kind(peer)) {
	case wire::kLinkIce: return "ice";
	case wire::kLinkRelay: return "relay";
	default: return "direct";
	}
}

uint8_t Connector::link_kind(uint32_t peer) const
{
	auto it = m_links.find(peer);
	return it == m_links.end() ? (uint8_t)wire::kLinkUnknown : it->second;
}

} // namespace mppm::session
