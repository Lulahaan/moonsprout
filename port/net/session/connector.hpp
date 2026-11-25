// M5.3b connector (D6; docs/online/M5_3B_LOCKSTEP.md "Connector"): joins a lobby member to the host's
// transport peer, directly (the host's addresses from an "addr" signal, 1.5 s each), through ICE
// (libjuice agents, SDP and candidates over lobby signals) or through the builtin relay of mppm-server.
// Auto: ICE when the server lists ICE servers; else, when it offers the relay, direct (LAN, then the
// public addresses with punches) and then the relay; else direct. Net thread only; time passed in.
#pragma once

#include "../lobby_client.hpp"
#include "../transport.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mppm::session {

enum class NetMode : uint8_t { Auto, Direct, Ice, Relay };
const char* net_mode_name(NetMode m);
bool parse_net_mode(const std::string& s, NetMode& out);

class Connector {
public:
	using SignalFn = std::function<void(uint32_t to, const std::string& kind, const std::string& data)>;
	using LogFn    = std::function<void(const std::string&)>;

	Connector(net::Transport& t, NetMode mode, bool host, uint32_t self, SignalFn signal, LogFn log);
	void set_lobby(const std::vector<net::IceServer>& ice, const net::RelayInfo& relay);

	// Host.
	void host_created(int64_t now);
	void member_joined(uint32_t member, int64_t now);
	void member_left(uint32_t member);
	// Client: Joined.
	void client_joined(int64_t now);

	void on_signal(uint32_t from, const std::string& kind, const std::string& data, int64_t now);
	void on_event(const net::Event& e, int64_t now);
	void tick(int64_t now);

	// Client: no connection within 15 s.
	bool timed_out() const { return m_timedOut; }
	bool connected() const { return m_connectedPeer != 0; }
	uint32_t connected_peer() const { return m_connectedPeer; }
	const char* last_link_name() const { return m_lastLink; } // the link of the newest connection
	// "direct" | "ice" | "relay" for a connected peer.
	const char* link_name(uint32_t peer) const;
	uint8_t link_kind(uint32_t peer) const; // wire::LinkKind
	NetMode effective_mode() const { return m_effective; }

private:
	struct Attempt {
		std::string addr;
		bool punch = false;
	};
	void start_client_path(int64_t now);
	void try_next_attempt(int64_t now);
	void start_ice_client();

	net::Transport& m_t;
	NetMode m_mode;
	NetMode m_effective;
	bool m_host;
	uint32_t m_self;
	SignalFn m_signal;
	LogFn m_log;
	std::vector<net::IceServer> m_ice;
	net::RelayInfo m_relay;
	bool m_relayOffered = false;
	std::string m_publicAddr;       // ours, from RelayRegistered aux
	std::string m_relayPort;        // client: our relay port "<ip>:<port>"
	// client
	int64_t m_joinedAt = 0;
	bool m_joined = false;
	std::vector<Attempt> m_attempts;
	size_t m_next = 0;
	uint32_t m_attemptPeer = 0;
	int64_t m_attemptAt = 0;
	bool m_directDone = false;       // direct addresses exhausted
	bool m_relayPending = false;     // want to connect through the relay once registered
	bool m_haveHostAddrs = false;
	uint32_t m_connectedPeer = 0;
	bool m_everConnected = false;    // no new attempts once the host link existed (no reconnection in part 1)
	bool m_timedOut = false;
	uint32_t m_icePeer = 0;
	// host: ICE peer -> member
	std::map<uint32_t, uint32_t> m_icePeers;
	std::set<uint32_t> m_punched; // host: members whose "addr public" was punched (once each)
	std::map<uint32_t, uint8_t> m_links; // peer -> link kind
	const char* m_lastLink = "direct";
};

} // namespace mppm::session
