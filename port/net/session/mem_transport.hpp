// M5.3b in-process transport (docs/online/M5_3B_LOCKSTEP.md "Test bots and netsim"): a hub of
// endpoints that implements mppm::net::Transport with the same send-side impairment model as the real
// shim (delay + U(0, jitter), loss, duplication, seeded xorshift32; the control channel stays
// reliable and ordered, as ENet's). Real time (the test bots) or a virtual clock (p2net_selftest).
#pragma once

#include "../transport.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace mppm::session {

class MemTransport;

class MemHub {
public:
	// clock: virtual steady ns (self-test); empty = std::chrono::steady_clock (real time).
	explicit MemHub(std::function<int64_t()> clock = {});
	~MemHub();
	std::unique_ptr<MemTransport> endpoint(const std::string& name, const net::Impair& impair);
	int64_t now() const;
	bool virtual_clock() const { return (bool)m_clock; }
	// The endpoint stops sending and receiving; its peers see Disconnected ("timeout") after afterNs.
	void vanish(const std::string& name, int64_t afterNs);
	// Earliest pending delivery (any endpoint), INT64_MAX when none.
	int64_t next_release();
	int64_t resetTimeoutNs = 1000000000LL; // a reset or vanished peer's remote sees Disconnected after this

private:
	friend class MemTransport;
	struct Packet {
		int64_t release = 0;
		uint64_t seq    = 0;
		net::Event ev;
		bool operator>(const Packet& o) const { return release != o.release ? release > o.release : seq > o.seq; }
	};
	struct Box {
		MemTransport* owner = nullptr;
		std::priority_queue<Packet, std::vector<Packet>, std::greater<Packet>> inbox;
		bool dead = false;
	};
	void post(const std::string& to, int64_t release, net::Event ev);
	std::function<int64_t()> m_clock;
	mutable std::mutex m_mu;
	std::condition_variable m_cv;
	std::map<std::string, Box> m_boxes;
	uint64_t m_seq = 0;
};

class MemTransport : public net::Transport {
public:
	MemTransport(MemHub& hub, const std::string& name, const net::Impair& impair);
	~MemTransport() override;

	std::string local_address() const override { return "mem:" + m_name; }
	std::vector<std::string> local_addresses() const override { return { local_address() }; }
	// ipPort: "mem:<name>" (or "<name>").
	net::PeerId connect(const std::string& ipPort, uint32_t connectData) override;
	void punch(const std::string&, int) override { }
	void relay_register(const std::string&, const uint8_t[16]) override { }
	net::PeerId ice_create(bool, uint32_t) override { return net::kNoPeer; }
	std::string ice_local_description(net::PeerId) override { return {}; }
	bool ice_set_remote_description(net::PeerId, const std::string&) override { return false; }
	bool ice_add_remote_candidate(net::PeerId, const std::string&) override { return false; }
	void ice_set_remote_gathering_done(net::PeerId) override { }
	bool ice_gather(net::PeerId) override { return false; }
	bool send(net::PeerId peer, net::Channel channel, const uint8_t* data, size_t size, bool flush) override;
	void disconnect(net::PeerId peer, uint32_t reason) override;
	void reset(net::PeerId peer) override;
	void service(uint32_t timeoutUs, std::vector<net::Event>& out) override;
	void wake() override;
	bool stats(net::PeerId peer, net::PeerStats& out) const override;

	const std::string& name() const { return m_name; }
	// Test only: this endpoint's steady clock runs `ns` ahead of the hub's (recvNs is in its clock).
	void set_clock_offset(int64_t ns) { m_clockOffset = ns; }
	void set_impair(const net::Impair& impair);

private:
	friend class MemHub;
	struct Peer {
		std::string remote;          // remote endpoint name
		net::PeerId remotePeer = 0;  // the remote's id for us (0 until it accepted)
		bool connected = false;
		int64_t lastControlRelease = 0;
		net::PeerStats st;
	};
	uint32_t rnd();
	int64_t delay_draw();
	MemHub& m_hub;
	std::string m_name;
	net::Impair m_impair;
	uint32_t m_rng;
	net::PeerId m_nextPeer = 1;
	std::map<net::PeerId, Peer> m_peers;
	bool m_woken = false;
	int64_t m_clockOffset = 0;
};

} // namespace mppm::session
