// M5.3b test bots (D29; docs/online/M5_3B_LOCKSTEP.md "Test bots"): --net-test-bots N. N in-process
// clients take seats 1..N of a host that runs without a lobby server, over MemTransport (with the
// impairment of --net-impair, one seed per bot). Each bot follows the host game's reads (it reads
// tick t once the host's game has read it), plays the --net-input records of its seat, acks bundles
// and reports every load barrier as soon as the host's game reported it (MPPM_NET_TEST_BOT_LOAD_MS
// later). Net thread only.
#pragma once

#include "endpoint.hpp"
#include "mem_transport.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mppm::session {

struct InputLog {
	std::vector<uint8_t> records; // 48 bytes per tick
	uint32_t count = 0;
	bool load(const std::string& path, std::string& err);
	// The record of `seat` for tick t (record t - 1); neutral after the log.
	Input at(int seat, uint32_t t) const;
};

class Bots {
public:
	struct Shared {                       // written by the host's game thread
		std::atomic<uint32_t> hostRead{ 0 };
		std::atomic<uint32_t> loadTick{ 0 };
		std::atomic<uint32_t> loadCount{ 0 };
	};
	Bots(MemHub& hub, int n, const net::Impair& impair, const InputLog* log, const uint8_t build[32], const uint8_t content[32],
	     const std::string& mods, Shared& shared, std::function<void(const std::string&)> log2);
	~Bots();
	// The host core learns the bots as lobby members (ids 1..N).
	void register_with(HostCore& host, int64_t now);
	void step();
	int count() const { return (int)m_bots.size(); }

private:
	struct Bot {
		int seat = 0;
		std::unique_ptr<MemTransport> tr;
		std::unique_ptr<Endpoint> ep;
		bool started = false, done = false;
		uint32_t t = 0;
		uint32_t loadSeen = 0;
		int64_t loadAt = 0;
		uint32_t loadReported = 0;
	};
	std::vector<std::unique_ptr<Bot>> m_bots;
	const InputLog* m_log;
	Shared& m_shared;
	int64_t m_loadDelayNs = 0;
};

} // namespace mppm::session
