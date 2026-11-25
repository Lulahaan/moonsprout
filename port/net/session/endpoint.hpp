// M5.3b endpoint: one pure core (host or client) wired to a Transport and a GameLink. The net thread
// (session.cpp), the test bots (bots.cpp) and the self-test's netsim all drive cores through this.
#pragma once

#include "../transport.hpp"
#include "client_core.hpp"
#include "host_core.hpp"
#include "link.hpp"

#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mppm::session {

class Endpoint : public CoreEnv {
public:
	struct Hooks {
		std::function<void(const std::string&)> log;          // a "mppm: net: " line without the prefix
		std::function<int64_t()> slewGet;
		std::function<void(int64_t)> slewSet;
		std::function<void(int)> tickShift;                    // whole-tick grid moves (empty: slew only)
		std::function<void(const CoreNote&)> note;
		std::function<void(const net::Event&)> transportEvent; // every transport event, before the core sees it
		std::function<int64_t()> clock;                        // steady ns (a virtual clock in the self-test)
	};

	// `link`: the game link to use (owned by the caller, outliving this); nullptr = an own one.
	Endpoint(net::Transport* transport, Hooks hooks, GameLink* link = nullptr);
	~Endpoint() override;

	void make_host(const HostParams& p);
	void make_client(const ClientParams& p);
	HostCore* host() { return m_host.get(); }
	ClientCore* client() { return m_client.get(); }
	net::Transport* transport() { return m_transport; }

	// One pass: service the transport (waiting up to waitUs), feed its events and the game's
	// messages to the core, run the core's timers.
	void step(uint32_t waitUs);
	// Only the game's messages and the timers (no transport service).
	void step_game();
	int64_t now() const;

	// CoreEnv
	bool send(uint32_t peer, bool control, const std::vector<uint8_t>& data, bool flush) override;
	void disconnect(uint32_t peer, uint32_t reason) override;
	void to_game(const N2G& m) override;
	void note(const CoreNote& n) override;
	int64_t slew_get() override;
	void slew_set(int64_t ns) override;
	bool tick_shift(int ticks) override;
	void log(const std::string& line) override;
	GameLink& link() override { return *m_link; }

	uint64_t datagrams_dropped() const { return m_sendFailures; }

private:
	void dispatch(const net::Event& e, int64_t now);
	void flush_overflow();

	net::Transport* m_transport;
	Hooks m_hooks;
	GameLink m_ownLink;
	GameLink* m_link;
	std::unique_ptr<HostCore> m_host;
	std::unique_ptr<ClientCore> m_client;
	std::vector<net::Event> m_events;
	std::deque<N2G> m_overflow;
	struct Reassembly {
		std::vector<uint8_t> data;
		int next = 0;
	};
	std::map<uint32_t, Reassembly> m_chunks; // peer -> a control message in parts
	uint64_t m_sendFailures = 0;
	int64_t m_slewLocal = 0;
};

} // namespace mppm::session
