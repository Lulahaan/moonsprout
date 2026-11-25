// M5.3b test bots (see bots.hpp).
#include "bots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mppm::session {

namespace {
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
int64_t steady_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr uint64_t kBotSecret = 0xB07B07B07ull;
} // namespace

bool InputLog::load(const std::string& path, std::string& err)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> d;
	uint8_t buf[1 << 14];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		d.insert(d.end(), buf, buf + n);
		if (d.size() > (1u << 30)) break;
	}
	std::fclose(f);
	if (d.size() < 64 || le32(d.data()) != 0x4C4E504Du) {
		err = path + " is not an .mpnl input log";
		return false;
	}
	const uint16_t version = (uint16_t)(d[4] | (d[5] << 8));
	const uint32_t hdr     = (uint32_t)(d[6] | (d[7] << 8));
	if (version < 2 || version > 5 || hdr < 64 || hdr > d.size() || d[24] != 4 || d[25] != 48) {
		err = path + " has an unsupported header";
		return false;
	}
	uint32_t c       = le32(d.data() + 12);
	const uint32_t avail = (uint32_t)((d.size() - hdr) / 48);
	if (c == 0xFFFFFFFFu || c > avail) c = avail;
	records.assign(d.begin() + hdr, d.begin() + hdr + (size_t)c * 48);
	count = c;
	return true;
}

Input InputLog::at(int seat, uint32_t t) const
{
	Input in;
	if (t == 0 || t > count || seat < 0 || seat >= kMaxSeats) return in;
	std::memcpy(in.b, &records[(size_t)(t - 1) * 48 + (size_t)seat * 12], 8);
	return in;
}

Bots::Bots(MemHub& hub, int n, const net::Impair& impair, const InputLog* log, const uint8_t build[32], const uint8_t content[32],
           const std::string& mods, Shared& shared, std::function<void(const std::string&)> log2)
    : m_log(log), m_shared(shared)
{
	if (const char* v = std::getenv("MPPM_NET_TEST_BOT_LOAD_MS"); v && *v) m_loadDelayNs = std::atoll(v) * 1000000LL;
	for (int i = 1; i <= n; ++i) {
		auto b  = std::make_unique<Bot>();
		b->seat = i;
		net::Impair im = impair;
		im.seed        = impair.seed + (uint32_t)i;
		b->tr          = hub.endpoint("bot" + std::to_string(i), im);
		Endpoint::Hooks h;
		const std::string prefix = "bot" + std::to_string(i) + ": ";
		h.log                    = [log2, prefix](const std::string& l) { log2(prefix + l); };
		b->ep                    = std::make_unique<Endpoint>(b->tr.get(), h);
		ClientParams cp;
		cp.member       = (uint32_t)i;
		cp.secret       = kBotSecret + (uint64_t)i;
		cp.name         = "Bot " + std::to_string(i);
		cp.wantSlot     = i;
		cp.autoReady    = true;
		cp.phaseControl = false;
		std::memcpy(cp.build, build, 32);
		std::memcpy(cp.content, content, 32);
		cp.mods = mods;
		b->ep->make_client(cp);
		m_bots.push_back(std::move(b));
	}
}

Bots::~Bots() = default;

void Bots::register_with(HostCore& host, int64_t now)
{
	for (auto& b : m_bots) {
		host.add_member((uint32_t)b->seat, "Bot " + std::to_string(b->seat), false, kBotSecret + (uint64_t)b->seat, now);
		b->tr->connect("mem:host", (uint32_t)b->seat);
	}
}

void Bots::step()
{
	const uint32_t hostRead  = m_shared.hostRead.load(std::memory_order_acquire);
	const uint32_t loadCount = m_shared.loadCount.load(std::memory_order_acquire);
	const uint32_t loadTick  = m_shared.loadTick.load(std::memory_order_acquire);
	const int64_t now        = steady_ns();
	for (auto& bp : m_bots) {
		Bot& b = *bp;
		b.ep->step(0);
		N2G m;
		while (b.ep->link().n2g.pop(m)) {
			if (m.kind == N2G::Start) b.started = true;
			else if (m.kind == N2G::End) b.done = true;
		}
		if (!b.started || b.done) continue;
		// Load barriers: report the host's own as soon as it did (plus the test delay).
		if (loadCount > b.loadSeen) {
			b.loadSeen = loadCount;
			b.loadAt   = now;
		}
		if (b.loadSeen > b.loadReported && now - b.loadAt >= m_loadDelayNs) {
			b.loadReported = b.loadSeen;
			G2N g;
			g.kind  = G2N::Loaded;
			g.tick  = loadTick;
			g.count = b.loadSeen;
			b.ep->link().g2n.push(g);
		}
		// Reads: follow the host game's reads (the host's game holds at every barrier, so a bot never
		// reads past one before the release).
		while (b.t < hostRead && b.loadReported >= b.loadSeen) {
			++b.t;
			G2N g;
			g.kind  = G2N::LocalInput;
			g.tick  = b.t;
			g.input = m_log ? m_log->at(b.seat, b.t) : Input{};
			if (!b.ep->link().g2n.push(g)) {
				--b.t;
				break;
			}
		}
		b.ep->step_game();
	}
}

} // namespace mppm::session
