// M5.3b session (docs/online/M5_3B_LOCKSTEP.md): the net thread (owns the Transport, the LobbyClient,
// the connector, the core and the bots; a 1 ms loop), the command-line start (--host, --join,
// --net-test-bots), the launcher API mppm::online (port/include/p2_online.hpp), and the game-thread
// side used by the seam (session.hpp): START and END applied in p2_net_game_poll, readiness, the
// desync report, the end and summary lines.
#include "session.hpp"

#include "../lobby_client.hpp"
#include "../transport.hpp"
#include "bots.hpp"
#include "connector.hpp"
#include "endpoint.hpp"
#include "game_side.hpp"
#include "host_core.hpp"
#include "mem_transport.hpp"
#include "../../mods/pure/sha256.hpp"

#include "p2_mods.h"
#include "p2_mods.hpp"
#include "p2_net.h"
#include "p2_online.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

#ifndef MPPM_DEFAULT_SERVER_URL
#define MPPM_DEFAULT_SERVER_URL ""
#endif

extern "C" uint32_t VIGetRetraceCount(void);
extern "C" long long p2_host_late_sample_margin_ns(void); // port/main.cpp

namespace mppm::session {

namespace {

// ---- small helpers ------------------------------------------------------------------------------

int64_t steady_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

void logline(const std::string& s)
{
	std::fprintf(stderr, "mppm: net: %s\n", s.c_str());
	std::fflush(stderr);
}

[[noreturn]] void bad_flags(const std::string& why)
{
	std::fprintf(stderr, "mppm: net: %s; exiting 2\n", why.c_str());
	std::fflush(stderr);
	std::exit(2);
}

bool env_true(const char* name)
{
	const char* v = std::getenv(name);
	return v && *v && std::strcmp(v, "0") != 0;
}

std::string pref_dir()
{
	char* p = SDL_GetPrefPath(nullptr, "mppm");
	if (!p) return {};
	std::string s = p;
	SDL_free(p);
	return s;
}

std::string pretty_code(const std::string& c8) { return c8.size() == 8 ? c8.substr(0, 4) + "-" + c8.substr(4) : c8; }

uint32_t random_u32()
{
	std::random_device rd;
	uint32_t v = 0;
	while (v == 0) v = rd() ^ ((uint32_t)steady_ns() * 2654435761u);
	return v;
}

// ---- command line ---------------------------------------------------------------------------------

struct Cli {
	std::string server;
	bool host = false;
	std::string join;
	bool spectate = false;
	std::string name;
	std::string password;
	Mode mode = Mode::Battle;
	int stage = 0;
	int handicap[2] = { 0, 0 };
	int slot = -1;
	bool ready = false;
	int expect = 0;
	std::string codeFile;
	std::string netInput;
	std::string impair;
	NetMode netMode = NetMode::Auto;
	std::string bind;
	bool allowPlaintext = false;
	int bots = 0;
	bool iceRelayOnly = false;
	bool anyOnlineFlag = false;
	bool handicapGiven = false;
	std::string envError; // reported only when an online flag is given
};
Cli g_cli;
bool g_cliOnline = false;

std::mutex g_settingsMu;
online::Settings g_settings;

std::atomic<bool> g_gameBooted{ false };
uint32_t g_exitTick = 0; // --exit-after-ticks (game thread)

void read_cli_env(Cli& c)
{
	const char* v;
	if ((v = std::getenv("MPPM_SERVER_URL")) && *v) c.server = v;
	if ((v = std::getenv("MPPM_NET_NAME")) && *v) c.name = v;
	if ((v = std::getenv("MPPM_NET_INPUT")) && *v) c.netInput = v;
	if ((v = std::getenv("MPPM_NET_IMPAIR")) && *v) c.impair = v;
	if ((v = std::getenv("MPPM_NET_MODE")) && *v) {
		if (!parse_net_mode(v, c.netMode)) c.envError = fmt("bad MPPM_NET_MODE '%s' (auto, direct, ice or relay)", v);
	}
	if (env_true("MPPM_NET_FORCE_RELAY")) c.netMode = NetMode::Relay;
	if ((v = std::getenv("MPPM_NET_BIND")) && *v) c.bind = v;
	if (env_true("MPPM_NET_ALLOW_PLAINTEXT")) c.allowPlaintext = true;
	if (env_true("MPPM_NET_ICE_RELAY_ONLY")) c.iceRelayOnly = true;
}

// True when the command line asks for an online session (--host, --join, --net-test-bots).
bool cli_wants_online(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (!a || a[0] != '-' || a[1] != '-') continue;
		std::string name(a + 2);
		name = name.substr(0, name.find('='));
		if (name == "host" || name == "join" || name == "net-test-bots") return true;
	}
	return false;
}

// strict (an online command line): a bad or missing value exits 2. Otherwise (an offline run, whose
// own flags may share these names) a bad value is ignored, so offline behaviour does not change; the
// good ones still reach sessions started from the launcher (--server, --name, --net-mode, ...).
void read_cli_args(Cli& c, int argc, char** argv, bool strict)
{
	auto bad = [strict](const std::string& why) {
		if (strict) bad_flags(why);
	};
	auto get_int = [&](const char* v, int lo, int hi, const char* what, int& out) {
		char* end = nullptr;
		const long x = std::strtol(v ? v : "", &end, 10);
		if (!v || !*v || *end || x < lo || x > hi) {
			bad(fmt("bad %s '%s' (%d to %d)", what, v ? v : "", lo, hi));
			return false;
		}
		out = (int)x;
		return true;
	};
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (!a || a[0] != '-' || a[1] != '-') continue;
		std::string name(a + 2);
		std::string inl;
		const size_t eq     = name.find('=');
		const bool hasInline = eq != std::string::npos;
		if (hasInline) {
			inl  = name.substr(eq + 1);
			name = name.substr(0, eq);
		}
		auto take = [&]() -> const char* {
			if (hasInline) return inl.c_str();
			if (i + 1 < argc) return argv[++i];
			bad("--" + name + " needs a value");
			return nullptr;
		};
		const char* v = nullptr;
		int x         = 0;
		if (name == "server") {
			if ((v = take())) c.server = v;
		} else if (name == "host") {
			c.host          = true;
			c.anyOnlineFlag = true;
		} else if (name == "join") {
			if ((v = take())) c.join = v;
			c.anyOnlineFlag = true;
		} else if (name == "spectate") {
			c.spectate = true;
		} else if (name == "name") {
			if ((v = take())) c.name = v;
		} else if (name == "password") {
			if ((v = take())) c.password = v;
		} else if (name == "mode") {
			if (!(v = take())) continue;
			const std::string m = v;
			if (m == "battle") c.mode = Mode::Battle;
			else if (m == "2v2") c.mode = Mode::Battle2v2;
			else if (m == "challenge") c.mode = Mode::Challenge;
			else bad("bad --mode '" + m + "' (battle, 2v2 or challenge)");
		} else if (name == "stage") {
			if ((v = take()) && get_int(v, 0, 254, "--stage", x)) c.stage = x;
		} else if (name == "handicap") {
			if (!(v = take())) continue;
			const std::string h = v;
			const size_t comma  = h.find(',');
			int r = 0, b = 0;
			if (comma == std::string::npos) bad("bad --handicap '" + h + "' (R,B with 1 to 10)");
			else if (get_int(h.substr(0, comma).c_str(), 1, 10, "--handicap red", r) && get_int(h.substr(comma + 1).c_str(), 1, 10, "--handicap blue", b)) {
				c.handicap[0]   = r;
				c.handicap[1]   = b;
				c.handicapGiven = true;
			}
		} else if (name == "slot") {
			if ((v = take()) && get_int(v, 0, 3, "--slot", x)) c.slot = x;
		} else if (name == "ready") {
			c.ready = true;
		} else if (name == "expect") {
			if ((v = take()) && get_int(v, 1, 8, "--expect", x)) c.expect = x;
		} else if (name == "code-file") {
			if ((v = take())) c.codeFile = v;
		} else if (name == "net-input") {
			if ((v = take())) c.netInput = v;
		} else if (name == "net-impair") {
			if ((v = take())) c.impair = v;
		} else if (name == "net-mode") {
			if (!(v = take())) continue;
			const std::string m = v;
			NetMode nm;
			if (parse_net_mode(m, nm)) c.netMode = nm;
			else bad("bad --net-mode '" + m + "' (auto, direct, ice or relay)");
		} else if (name == "net-bind") {
			if ((v = take())) c.bind = v;
		} else if (name == "net-allow-plaintext") {
			c.allowPlaintext = !hasInline || inl != "0";
		} else if (name == "net-test-bots") {
			if ((v = take()) && get_int(v, 1, 3, "--net-test-bots", x)) c.bots = x;
			c.anyOnlineFlag = true;
		}
	}
}

// ---- real game hooks ------------------------------------------------------------------------------

struct RealHooks : GameHooks {
	int64_t now() override { return steady_ns(); }
	uint32_t grid_jump(int64_t localNs) override { return p2_vi_grid_jump(localNs); }
	void align_tick(uint32_t count) override { p2_host_align_tick(count); }
	int64_t retrace_time(uint32_t count) override { return p2_vi_retrace_time_ns(count); }
	int64_t next_retrace_estimate() override { return p2_vi_retrace_time_ns(VIGetRetraceCount() + 1); }
	int64_t margin_ns() override { return p2_host_late_sample_margin_ns(); }
	void log(const std::string& line) override { logline(line); }
};

const char* lobby_error_text(const std::string& code, const std::string& msg)
{
	if (code == "unreachable") return "Couldn't reach the lobby server.";
	if (code == "not_found") return "No game uses that code.";
	if (code == "full") return "That game is full.";
	if (code == "bad_password") return "This lobby has a password.";
	if (code == "rate_limited") return "Too many tries. Wait a minute and try again.";
	if (code == "version") return "The lobby server needs another version of mppm.";
	if (code == "forbidden") return "The host removed you from this game.";
	(void)msg;
	return "The lobby server sent something unexpected.";
}

online::Link to_online_link(uint8_t l)
{
	switch (l) {
	case wire::kLinkDirect:
	case wire::kLinkMem:
	case wire::kLinkHost: return online::Link::Direct;
	case wire::kLinkIce: return online::Link::Ice;
	case wire::kLinkRelay: return online::Link::Relay;
	default: return online::Link::Unknown;
	}
}


// Content hash and mod list when the game has not booted yet (a session from the standalone start
// menu): from the enabled mods in the folder, read now without mounting (plan 7b). The mount may
// still drop a mod over a disc-dependent check; game_poll compares again once the game has booted.
void premount_content(const mods::ScanArgs& args, uint8_t out[32], std::string& list)
{
	const mods::Scan sc = mods::scan(args);
	const mods::Hash h  = mods::content_hash(sc);
	std::memcpy(out, h.data(), 32);
	list.clear();
	if (sc.enabledValid <= 0) return;
	for (const mods::ModInfo& m : sc.mods) {
		if (!(m.enabled && m.present && m.valid)) continue;
		if (!list.empty()) list += ',';
		list += m.id + "@" + m.version;
	}
}

// ---- the session ------------------------------------------------------------------------------------

enum class Kind { Host, Join, Bots };

class Session {
public:
	Session(Kind kind, const Cli& cli, std::string server, std::string name, bool commandLine)
	    : m_kind(kind), m_cli(cli), m_server(std::move(server)), m_name(std::move(name)), m_commandLine(commandLine)
	{
	}
	~Session() { stop(0); }

	bool start(std::string& err);
	void stop(int64_t flushNs);
	void post(std::function<void()> fn)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_cmds.push_back(std::move(fn));
	}
	online::Snapshot snapshot()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		return m_snap;
	}
	std::vector<online::Notice> take_notices()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		std::vector<online::Notice> v;
		v.swap(m_notices);
		return v;
	}

	// game thread
	GameSide& side() { return m_side; }
	GameLink& link() { return m_link; }
	void capture_game_info();
	void game_poll(bool gameRunning);
	void shutdown(bool exitTickReached, uint32_t tick);
	bool started() const { return m_startApplied; }
	bool began() const { return m_began; }
	void set_began() { m_began = true; }
	bool ended() const { return m_endHandled; }
	int exit_code() const { return m_exitCode; }
	const SessionConfig& cfg() const { return m_cfg; }
	int seat() const { return m_seat; }
	// Game thread; m_code is written on the net thread (under m_mu, read there without it).
	std::string code() const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		return m_code.empty() ? std::string("local") : (m_code == "local" ? m_code : pretty_code(m_code));
	}
	void set_code(const std::string& c)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_code = c;
	}
	bool command_line() const { return m_commandLine; }
	Kind kind() const { return m_kind; }
	Bots::Shared& bots_shared() { return m_botsShared; }
	void leave_now(uint8_t reason);
	void ui_leave();
	bool left() const { return m_left; }
	bool thread_done() const { return m_threadDone.load(); }
	online::Stall stall_view();
	bool local_input(uint32_t t, Input& io) const
	{
		if (!m_haveInput) return false;
		io = m_input.at(m_seat, t);
		return true;
	}
	void set_net_input(InputLog&& log)
	{
		m_input     = std::move(log);
		m_haveInput = true;
	}
	// Launcher commands, run on the net thread.
	void cmd_ready(bool ready)
	{
		if (!m_ep) return;
		if (m_ep->host()) m_ep->host()->set_ready(ready);
		else if (m_ep->client()) m_ep->client()->set_ready(ready);
	}
	void cmd_slot(int slot)
	{
		if (!m_ep) return;
		if (m_ep->host()) m_ep->host()->take_slot(slot);
		else if (m_ep->client()) m_ep->client()->take_slot(slot);
	}
	void cmd_config(const online::Config& c)
	{
		if (m_ep && m_ep->host()) m_ep->host()->set_config((Mode)c.mode, c.stage, c.handicap[0], c.handicap[1]);
	}
	void cmd_start()
	{
		if (m_ep && m_ep->host()) m_ep->host()->start(steady_ns());
	}
	void cmd_kick(uint32_t id)
	{
		if (m_ep && m_ep->host()) m_ep->host()->kick(id, steady_ns());
		if (m_lobby) m_lobby->kick(id, false);
	}
	void cmd_drop(int slot)
	{
		if (m_ep && m_ep->host()) m_ep->host()->drop_slot(slot, steady_ns());
	}

private:
	void run();
	bool net_init();
	void on_lobby(const net::LobbyEvent& e);
	void on_note(const CoreNote& n);
	bool create_transport(const std::vector<net::IceServer>& ice, bool listen, std::string& err);
	void create_endpoint(net::Transport* t);
	void fail(int reason, const std::string& error, const std::string& message, int exitCode);
	void publish(int64_t now);
	void notice(int kind, const std::string& title, const std::string& body = {});
	void collect_summary();
	void apply_start();
	void handle_end();
	void write_report(const EndInfo& e, std::string& path);
	void print_summary();
	void log_end(const std::string& reason);
	Endpoint::Hooks hooks(bool slew);

	Kind m_kind;
	Cli m_cli;
	std::string m_server, m_name;
	bool m_commandLine;
	net::Impair m_impair;
	InputLog m_input;
	bool m_haveInput = false;

	GameLink m_link;

	// net thread
	std::thread m_thread;
	std::atomic<bool> m_stop{ false };
	std::atomic<bool> m_threadDone{ false };
	std::unique_ptr<MemHub> m_hub;
	std::unique_ptr<net::Transport> m_transport;
	std::atomic<net::Transport*> m_wakeT{ nullptr }; // m_transport for stop() on the game thread (set once, lives until ~Session)
	std::unique_ptr<net::LobbyClient> m_lobby;
	std::unique_ptr<Endpoint> m_ep;
	std::unique_ptr<Connector> m_conn;
	std::unique_ptr<Bots> m_bots;
	Bots::Shared m_botsShared;
	uint32_t m_selfId = 0;
	uint64_t m_secret = 0;
	uint8_t m_build[32]   = {};
	uint8_t m_content[32] = {};
	std::string m_mods;
	mods::ScanArgs m_scanArgs;             // taken in start() on the game thread
	bool m_preBoot = false;                // started before the game booted (the start menu)
	bool m_bootChecked = false;            // game thread: the content was compared after the boot
	uint8_t m_announced[32] = {};          // m_content as sent in the handshake (under m_mu)
	std::atomic<bool> m_lingerDone{ false };
	std::atomic<bool> m_gameInfoReady{ false };
	std::string m_syncedJson = "{}";       // written before m_gameInfoReady
	std::atomic<int64_t> m_quitAt{ 0 };     // the net thread leaves after this time (0 = not asked)
	int64_t m_lastPublish = 0;
	int m_failExit = 0;                     // net thread -> game thread through m_mu
	std::string m_code;                     // 8 canonical symbols, or "local"; written under m_mu (net thread)

	// shared (m_mu)
	mutable std::mutex m_mu;
	std::deque<std::function<void()>> m_cmds;
	online::Snapshot m_snap;
	std::vector<online::Notice> m_notices;
	bool m_snapDirty = true;
	online::Phase m_phase = online::Phase::Connecting;
	std::string m_error;
	int m_endReason = online::kEndNone;
	uint32_t m_desyncTick = 0;
	std::string m_reportPath;
	std::vector<int32_t> m_phaseAbs;
	std::string m_dList;
	int m_dMax = 0;
	uint32_t m_rttUs = 0;
	uint64_t m_bytesOut = 0, m_bytesIn = 0;
	std::string m_linkName = "host";
	int64_t m_netStartNs = 0, m_netEndNs = 0;
	bool m_summaryReady = false;

	// game thread
	RealHooks m_hooks;
	GameSide m_side{ m_link, m_hooks };
	bool m_startApplied = false;
	bool m_began = false;
	bool m_endHandled = false;
	bool m_left = false;
	bool m_infoCaptured = false;
	int m_exitCode = 0;
	std::string m_endLog;
	SessionConfig m_cfg;
	int m_seat = -1;
	bool m_summaryPrinted = false;
	bool m_endLogged = false;
};

Endpoint::Hooks Session::hooks(bool slew)
{
	Endpoint::Hooks h;
	h.log  = [](const std::string& l) { logline(l); };
	h.note = [this](const CoreNote& n) { on_note(n); };
	if (slew) {
		h.slewGet = [] { return (int64_t)p2_vi_phase_slew_get(); };
		h.slewSet = [](int64_t v) { p2_vi_phase_slew_set(v); };
		h.tickShift = [](int k) { p2_host_tick_shift(k); };
	} else {
		h.slewGet = [] { return (int64_t)0; };
		h.slewSet = [](int64_t) {};
	}
	h.transportEvent = [this](const net::Event& e) {
		const int64_t now = steady_ns();
		if (m_conn) m_conn->on_event(e, now);
		if (e.kind == net::Event::Kind::Connected && m_ep && m_ep->host() && m_conn)
			m_ep->host()->set_member_link(e.connectData, m_conn->link_kind(e.peer));
		if (e.kind == net::Event::Kind::Connected && m_ep && m_ep->host() && m_bots) {
			logline(fmt("peer %u connected link=direct remote=mem:bot%u", e.connectData, e.connectData));
			m_ep->host()->set_member_link(e.connectData, wire::kLinkMem);
		}
	};
	return h;
}

bool Session::start(std::string& err)
{
	std::string ierr;
	if (!net::parse_impair(m_cli.impair, m_impair, ierr)) {
		err = "bad --net-impair: " + ierr;
		return false;
	}
	if (!m_cli.netInput.empty()) {
		InputLog log;
		if (!log.load(m_cli.netInput, ierr)) {
			err = "--net-input: " + ierr;
			return false;
		}
		set_net_input(std::move(log));
	}
	if (m_kind != Kind::Bots) {
		net::LobbyConfig lc;
		lc.serverUrl      = m_server;
		lc.allowPlaintext = m_cli.allowPlaintext;
		lc.name           = m_name;
		m_lobby           = net::LobbyClient::create(lc, err);
		if (!m_lobby) return false;
	}
	if (!m_commandLine) capture_game_info();
	m_preBoot = !g_gameBooted.load();
	if (m_preBoot) m_scanArgs = mods::scan_args();
	m_netStartNs = steady_ns();
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_phase     = online::Phase::Connecting;
		m_snapDirty = true;
	}
	m_thread = std::thread([this] { run(); });
	return true;
}

void Session::capture_game_info()
{
	if (m_infoCaptured) return;
	m_infoCaptured = true;
	if (const char* j = p2_launcher_net_synced_json()) m_syncedJson = j;
	m_gameInfoReady.store(true, std::memory_order_release);
}

void Session::stop(int64_t flushNs)
{
	if (!m_thread.joinable()) return;
	if (flushNs > 0) {
		const int64_t until = steady_ns() + flushNs;
		const int64_t prev  = m_quitAt.load();
		if (prev == 0 || prev > until) m_quitAt = until;
	} else {
		m_stop = true;
	}
	if (net::Transport* t = m_wakeT.load(std::memory_order_acquire)) t->wake();
	m_thread.join();
}

void Session::notice(int kind, const std::string& title, const std::string& body)
{
	std::lock_guard<std::mutex> lk(m_mu);
	online::Notice n;
	n.kind  = kind;
	n.title = title;
	n.body  = body;
	if (m_notices.size() < 32) m_notices.push_back(n);
}

void Session::fail(int reason, const std::string& error, const std::string& message, int exitCode)
{
	if (exitCode == 9) logline(fmt("lobby failed error=%s msg=%s", error.c_str(), message.c_str()));
	{
		std::lock_guard<std::mutex> lk(m_link.mu);
		if (m_link.end.reason != kEndNone && m_link.end.reason != kEndHostLeft) return;
		m_link.end.reason    = reason;
		m_link.end.text      = message;
		m_link.end.logReason = exitCode == 9 ? "error" : (reason == kEndHostLeft ? "host_left" : reason == kEndKicked ? "dropped" : "error");
	}
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_failExit  = exitCode;
		m_phase     = online::Phase::Ended;
		m_error     = message;
		m_endReason = reason;
		m_snapDirty = true;
	}
	N2G g;
	g.kind = N2G::End;
	// Through the endpoint when there is one: it keeps the order behind bundles waiting in its overflow.
	if (m_ep) m_ep->to_game(g);
	else if (!m_link.n2g.push(g)) logline("game link full at the end");
}

bool Session::create_transport(const std::vector<net::IceServer>& ice, bool listen, std::string& err)
{
	net::TransportConfig tc;
	tc.listen       = listen;
	tc.maxPeers     = listen ? 8 : 2;
	tc.bindAddress  = m_cli.bind.empty() ? "0.0.0.0" : m_cli.bind;
	tc.impair       = m_impair;
	tc.iceServers   = ice;
	tc.iceRelayOnly = m_cli.iceRelayOnly;
	m_transport     = net::Transport::create(tc, err);
	m_wakeT.store(m_transport.get(), std::memory_order_release);
	return m_transport != nullptr;
}

void Session::create_endpoint(net::Transport* t)
{
	const bool client = m_kind == Kind::Join;
	m_ep              = std::make_unique<Endpoint>(t, hooks(client), &m_link);
	if (client) {
		ClientParams cp;
		cp.member    = m_selfId;
		cp.secret    = m_secret;
		cp.name      = m_name;
		cp.spectator = m_cli.spectate;
		std::memcpy(cp.build, m_build, 32);
		std::memcpy(cp.content, m_content, 32);
		cp.mods      = m_mods;
		cp.wantSlot  = m_cli.slot;
		cp.autoReady = m_cli.ready;
		m_ep->make_client(cp);
		return;
	}
	HostParams hp;
	hp.mode        = m_cli.mode;
	hp.stage       = m_cli.stage;
	hp.handicap[0] = (uint8_t)(m_cli.mode == Mode::Battle ? m_cli.handicap[0] : 0);
	hp.handicap[1] = (uint8_t)(m_cli.mode == Mode::Battle ? m_cli.handicap[1] : 0);
	hp.hostSlot    = m_cli.slot >= 0 ? m_cli.slot : 0;
	hp.hostReady   = m_cli.ready || m_kind == Kind::Bots;
	hp.autoStart   = m_cli.ready || m_kind == Kind::Bots;
	hp.expect      = m_cli.expect;
	hp.name        = m_name;
	std::memcpy(hp.build, m_build, 32);
	std::memcpy(hp.content, m_content, 32);
	hp.mods        = m_mods;
	hp.syncedJson  = m_syncedJson;
	hp.syncedHash  = synced_hash_of(m_syncedJson);
	hp.seed        = seam::seed() ? seam::seed() : random_u32();
	hp.sessionTag  = random_u32();
	hp.tickOrder   = (uint8_t)p2_net_tick_order();
	hp.simK        = 3.6f;
	m_ep->make_host(hp);
}

bool Session::net_init()
{
	// The Synced settings (and, for a command-line run, a booted game: p2_mods_mount has run) first.
	while (!m_stop && !m_quitAt.load() && !m_gameInfoReady.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	if (m_stop || m_quitAt.load()) return false;
	build_hash(m_build);
	if (g_gameBooted.load()) {
		p2_mods_content_hash(m_content);
		m_mods = p2_mods_list() ? p2_mods_list() : "";
	} else {
		premount_content(m_scanArgs, m_content, m_mods);
	}
	{
		std::lock_guard<std::mutex> lk(m_mu);
		std::memcpy(m_announced, m_content, 32);
	}
	const int64_t now = steady_ns();
	switch (m_kind) {
	case Kind::Bots: {
		m_hub       = std::make_unique<MemHub>();
		set_code("local");
		m_transport = m_hub->endpoint("host", m_impair);
		m_wakeT.store(m_transport.get(), std::memory_order_release);
		create_endpoint(m_transport.get());
		m_bots = std::make_unique<Bots>(*m_hub, m_cli.bots, m_impair, m_haveInput ? &m_input : nullptr, m_build, m_content, m_mods, m_botsShared,
		                                [](const std::string& l) { logline(l); });
		m_bots->register_with(*m_ep->host(), now);
		{
			std::lock_guard<std::mutex> lk(m_mu);
			m_phase     = online::Phase::Lobby;
			m_snapDirty = true;
		}
		break;
	}
	case Kind::Host:
		m_lobby->create_session(m_cli.password, 8);
		break;
	case Kind::Join:
		m_lobby->join_session(m_cli.join, m_cli.password, m_cli.spectate);
		break;
	}
	return true;
}

void Session::on_lobby(const net::LobbyEvent& e)
{
	using K           = net::LobbyEvent::Kind;
	const int64_t now = steady_ns();
	switch (e.kind) {
	case K::Created: {
		std::string c8;
		set_code(net::normalize_code(e.code, c8) ? c8 : e.code);
		logline(fmt("lobby created code=%s server=%s", pretty_code(m_code).c_str(), m_server.c_str()));
		if (!m_cli.codeFile.empty()) {
			const std::string tmp = m_cli.codeFile + ".tmp";
			if (FILE* f = std::fopen(tmp.c_str(), "w")) {
				std::fprintf(f, "%s\n", pretty_code(m_code).c_str());
				std::fclose(f);
				std::rename(tmp.c_str(), m_cli.codeFile.c_str());
			}
		}
		m_selfId = 0;
		std::string err;
		if (!create_transport(e.ice, true, err)) {
			fail(kEndServer, "transport", err, 9);
			return;
		}
		create_endpoint(m_transport.get());
		m_conn = std::make_unique<Connector>(*m_transport, m_cli.netMode, true, 0,
		                                     [this](uint32_t to, const std::string& k, const std::string& d) { m_lobby->signal(to, k, d); },
		                                     [](const std::string& l) { logline(l); });
		m_conn->set_lobby(e.ice, e.relay);
		m_conn->host_created(now);
		std::lock_guard<std::mutex> lk(m_mu);
		m_phase     = online::Phase::Lobby;
		m_snapDirty = true;
		return;
	}
	case K::Joined: {
		std::string c8;
		set_code(net::normalize_code(e.code, c8) ? c8 : e.code);
		m_selfId = e.self.id;
		m_secret = e.self.auth;
		logline(fmt("lobby joined code=%s member=%u server=%s", pretty_code(m_code).c_str(), m_selfId, m_server.c_str()));
		std::string err;
		if (!create_transport(e.ice, false, err)) {
			fail(kEndServer, "transport", err, 9);
			return;
		}
		create_endpoint(m_transport.get());
		m_conn = std::make_unique<Connector>(*m_transport, m_cli.netMode, false, m_selfId,
		                                     [this](uint32_t to, const std::string& k, const std::string& d) { m_lobby->signal(to, k, d); },
		                                     [](const std::string& l) { logline(l); });
		m_conn->set_lobby(e.ice, e.relay);
		m_conn->client_joined(now);
		return;
	}
	case K::Failed:
		fail(e.error == "unreachable" ? kEndServer : kEndRefused, e.error, lobby_error_text(e.error, e.message), 9);
		return;
	case K::MemberJoined:
		if (m_ep && m_ep->host()) {
			m_ep->host()->add_member(e.member.id, e.member.name, e.member.spectator, e.member.auth, now);
			if (m_conn) m_conn->member_joined(e.member.id, now);
		}
		return;
	case K::MemberLeft:
		if (m_ep && m_ep->host()) {
			const uint8_t r = e.message == "kicked" || e.message == "banned" ? wire::kDropKicked : e.message == "timeout" ? wire::kDropTimeout : wire::kDropLeft;
			m_ep->host()->remove_member(e.member.id, r, now);
			if (m_conn) m_conn->member_left(e.member.id);
		}
		return;
	case K::Signal:
		if (m_conn) m_conn->on_signal(e.from, e.signalKind, e.data, now);
		return;
	case K::Closed: {
		const bool startedNet = m_ep && (m_ep->host() ? m_ep->host()->started() : (m_ep->client() && m_ep->client()->started()));
		if (m_ep && m_ep->host()) {
			if (startedNet) logline("lobby closed (" + e.message + "); the game goes on");
			else fail(kEndServer, e.message, "The lobby server closed the lobby.", 9);
			return;
		}
		if (e.message == "kicked" || e.message == "banned") {
			fail(kEndKicked, e.message, "The host removed you from the game.", 8);
		} else if (e.message == "host_left") {
			// After START the P2P link decides (ClientCore::lobby_host_left), as on the host: the host's
			// lobby WebSocket can drop while its game, and its link to us, plays on.
			if (startedNet && m_ep->client()) {
				logline("lobby closed (host_left); the match goes on while the host's connection is alive");
				m_ep->client()->lobby_host_left(now);
			} else fail(kEndHostLeft, e.message, "The host closed the lobby.", 8);
		} else if (!startedNet) {
			fail(kEndServer, e.message, "Lost the connection to the lobby server.", 9);
		} else {
			logline("lobby connection closed (" + e.message + "); the game goes on");
		}
		return;
	}
	}
}

void Session::on_note(const CoreNote& n)
{
	switch (n.kind) {
	case CoreNote::LobbyChanged:
		break;
	case CoreNote::Started: {
		std::lock_guard<std::mutex> lk(m_mu);
		m_phase = online::Phase::Starting;
		break;
	}
	case CoreNote::Ended: {
		std::lock_guard<std::mutex> lkl(m_link.mu);
		std::lock_guard<std::mutex> lk(m_mu);
		m_phase      = online::Phase::Ended;
		m_error      = m_link.end.text;
		m_endReason  = m_link.end.reason;
		m_desyncTick = m_link.end.desyncTick;
		break;
	}
	case CoreNote::Notice:
		notice(n.kind2, n.title, n.text);
		break;
	case CoreNote::Welcomed: {
		std::lock_guard<std::mutex> lk(m_mu);
		if (m_phase == online::Phase::Connecting) m_phase = online::Phase::Lobby;
		break;
	}
	case CoreNote::PeerGone:
		break;
	}
	std::lock_guard<std::mutex> lk(m_mu);
	m_snapDirty = true;
}

void Session::publish(int64_t now)
{
	{
		std::lock_guard<std::mutex> lk(m_mu);
		if (!m_snapDirty && now - m_lastPublish < 500000000LL) return;
		m_snapDirty = false;
	}
	m_lastPublish = now;
	online::Snapshot s;
	wire::Lobby l;
	bool haveLobby = false;
	HostCore* h    = m_ep ? m_ep->host() : nullptr;
	ClientCore* c  = m_ep ? m_ep->client() : nullptr;
	if (h) {
		l         = h->lobby_view();
		haveLobby = true;
		s.role    = online::Role::Host;
		s.canStart = h->can_start();
		s.status  = h->status_line();
	} else if (c) {
		l         = c->lobby();
		haveLobby = c->welcomed();
		s.role    = m_cli.spectate ? online::Role::Spectator : online::Role::Player;
	}
	s.code      = m_code.empty() || m_code == "local" ? m_code : pretty_code(m_code);
	s.serverUrl = m_server;
	if (haveLobby) {
		s.config.mode        = (online::Mode)l.mode;
		s.config.stage       = l.stage;
		s.config.handicap[0] = l.handicap[0];
		s.config.handicap[1] = l.handicap[1];
		s.seats              = l.seats;
		std::vector<online::Member> seated, others;
		online::Member hostM;
		bool haveHost = false;
		for (const wire::LobbyMember& lm : l.members) {
			online::Member m;
			m.id          = lm.id;
			m.name        = lm.name;
			m.slot        = lm.slot == kNoSlot ? -1 : lm.slot;
			m.team        = m.slot < 0 ? -1 : (l.seats > 2 ? (m.slot & 1) : m.slot);
			m.host        = (lm.flags & wire::kMemHost) != 0;
			m.self        = lm.id == m_selfId;
			m.spectator   = (lm.flags & wire::kMemSpectator) != 0;
			m.ready       = (lm.flags & wire::kMemReady) != 0;
			m.connected   = (lm.flags & wire::kMemConnected) != 0;
			m.contentOk   = (lm.flags & wire::kMemContentOk) != 0;
			m.contentNote = lm.note;
			m.pingMs      = lm.pingMs == 0xFFFF ? -1 : lm.pingMs;
			m.delayTicks  = lm.delay;
			m.link        = to_online_link(lm.link);
			if (h && m.slot >= 0) m.delayTicks = h->delay_of_seat(m.slot);
			if (c && m.self) {
				m.delayTicks = c->started() ? c->my_delay() : 0;
				m.pingMs     = c->rtt_us() ? (int)(c->rtt_us() / 1000) : m.pingMs;
			}
			if (m.host) {
				hostM    = m;
				haveHost = true;
			} else if (m.slot >= 0) {
				seated.push_back(m);
			} else {
				others.push_back(m);
			}
		}
		std::sort(seated.begin(), seated.end(), [](const online::Member& a, const online::Member& b) { return a.slot < b.slot; });
		std::sort(others.begin(), others.end(), [](const online::Member& a, const online::Member& b) { return a.id < b.id; });
		if (haveHost) s.members.push_back(hostM);
		s.members.insert(s.members.end(), seated.begin(), seated.end());
		s.members.insert(s.members.end(), others.begin(), others.end());
		if (c) {
			const online::Member* me = nullptr;
			for (const auto& m : s.members)
				if (m.self) me = &m;
			bool open = false;
			for (int seat = 0; seat < s.seats; ++seat) {
				bool taken = false;
				for (const auto& m : s.members)
					if (m.slot == seat) taken = true;
				open = open || !taken;
			}
			std::string waitFor;
			for (const auto& m : s.members)
				if (m.slot >= 0 && !m.ready && !m.self && waitFor.empty()) waitFor = m.name;
			if (c->started()) s.status.clear(); // the in-game Online window says the game is running
			else if (me && !me->contentOk) s.status = me->contentNote.empty() ? "Your content differs from the host's." : me->contentNote;
			else if (me && me->slot >= 0 && !me->ready) s.status = "Get ready when you are.";
			else if (me && me->slot < 0 && !me->spectator) s.status = "Take a seat to play.";
			else if (open) s.status = s.seats == 2 ? "Waiting for another player." : "Waiting for more players.";
			else if (!waitFor.empty()) s.status = "Waiting for " + waitFor + " to get ready.";
			else s.status = "Waiting for the host to start the game.";
		}
	} else if (c) {
		s.status = "Connecting to the host…";
	} else {
		s.status = "Connecting to the lobby server…";
	}
	std::lock_guard<std::mutex> lk(m_mu);
	s.phase      = m_phase;
	s.error      = m_error;
	s.endReason  = m_endReason;
	s.desyncTick = m_desyncTick;
	s.reportPath = m_reportPath;
	s.generation = m_snap.generation + 1;
	m_snap       = std::move(s);
}

void Session::collect_summary()
{
	std::lock_guard<std::mutex> lk(m_mu);
	m_netEndNs = steady_ns();
	if (!m_ep) return;
	if (HostCore* h = m_ep->host()) {
		std::string d;
		int dmax = 0;
		for (int s = 0; s < h->config().seats; ++s) {
			const int v = h->delay_of_seat(s);
			d += fmt("%s%d:%d", d.empty() ? "" : ",", s, v);
			dmax = std::max(dmax, v);
		}
		m_dList    = d;
		m_dMax     = dmax;
		m_rttUs    = h->rtt_us_max();
		m_bytesOut = h->bytes_out();
		m_bytesIn  = h->bytes_in();
		m_linkName = "host";
	} else if (ClientCore* c = m_ep->client()) {
		m_phaseAbs = c->phase_samples();
		if (c->local_seat() >= 0) {
			m_dList = fmt("%d:%d", c->local_seat(), c->my_delay());
			m_dMax  = c->my_delay();
		} else {
			m_dList = "-";
		}
		m_rttUs    = c->rtt_us();
		m_bytesOut = c->bytes_out();
		m_bytesIn  = c->bytes_in();
		m_linkName = m_conn ? m_conn->last_link_name() : "direct";
	}
	m_summaryReady = true;
}

void Session::run()
{
	if (!net_init()) {
		m_threadDone = true;
		return;
	}
	std::vector<net::LobbyEvent> lev;
	bool stopping = false;
	while (true) {
		const int64_t now = steady_ns();
		const int64_t quitAt = m_quitAt.load();
		if (m_stop || (quitAt && now >= quitAt)) break;
		std::deque<std::function<void()>> cmds;
		{
			std::lock_guard<std::mutex> lk(m_mu);
			cmds.swap(m_cmds);
		}
		for (auto& f : cmds) f();
		if (m_lobby) {
			lev.clear();
			m_lobby->poll(lev);
			for (const auto& e : lev) on_lobby(e);
		}
		if (m_ep) {
			m_ep->step(1000);
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (m_bots) m_bots->step();
		if (m_conn) {
			m_conn->tick(steady_ns());
			if (m_conn->timed_out() && m_ep && m_ep->client() && !m_ep->client()->ended() && !stopping) {
				stopping = true;
				fail(kEndConnectionLost, "connect_timeout", "Couldn't connect to the host.", 9);
			}
		}
		if (m_ep && m_ep->host() && m_ep->host()->linger_done()) m_lingerDone = true;
		publish(steady_ns());
	}
	// Flush what is queued (BYE, END) for a moment, then report.
	collect_summary();
	m_threadDone = true;
}

void Session::leave_now(uint8_t reason)
{
	if (m_left) return;
	m_left = true;
	m_side.leave(reason);
}

void Session::ui_leave()
{
	if (!m_left && !m_endHandled) leave_now(wire::kByeLeft);
	m_left = true;
	if (m_startApplied) {
		// The session is over: unlock the settings and let p2_net_active() fall back (the launcher's
		// "Back to start menu" restart relaunches only when no session is active, restart.cpp).
		p2_launcher_net_unlock();
		p2_net_set_online(0);
		m_startApplied = false;
	}
	if (m_lobby) post([this] { if (m_lobby) m_lobby->leave(); });
	m_quitAt = steady_ns() + 200000000LL;
}

online::Stall Session::stall_view()
{
	online::Stall st;
	uint32_t tick  = 0;
	int64_t since  = 0;
	bool loading   = false;
	if (!m_side.waiting(tick, since, loading)) return st;
	const double secs = (double)(steady_ns() - since) / 1e9;
	if (secs < 0.3) return st;
	st.waiting = true;
	st.loading = loading;
	st.tick    = tick;
	st.seconds = secs;
	const uint32_t mask = loading ? m_link.loadingMask.load() : m_link.waitMask.load();
	const online::Snapshot snap = snapshot();
	for (int s = 0; s < kMaxSeats; ++s) {
		if (!(mask & (1u << s))) continue;
		if (s == m_seat) continue;
		std::string name = "Player";
		for (const auto& m : snap.members)
			if (m.slot == s) name = m.name;
		st.names.push_back(name + " (P" + std::to_string(s + 1) + ")");
		st.slots.push_back(s);
	}
	const bool host = m_kind != Kind::Join;
	if (!host && mask == 0) st.names.clear(); // the host itself is late
	// A load may differ by a few seconds between machines: at a barrier the button comes after 10 s.
	st.canDrop  = host && secs >= (loading ? 10.0 : 5.0) && !st.slots.empty();
	st.canLeave = !host && secs >= 10.0;
	return st;
}

// ---- game thread: START, END, report, summary ---------------------------------------------------------

void Session::apply_start()
{
	{
		std::lock_guard<std::mutex> lk(m_link.mu);
		m_cfg  = m_link.config;
		m_seat = m_link.localSeat;
	}
	m_startApplied = true;
	seam::online_on();
	if (!p2_launcher_net_lock(m_cfg.syncedJson.c_str())) {
		logline("the host's Synced settings could not be applied; leaving");
		leave_now(wire::kByeError);
		{
			std::lock_guard<std::mutex> lk(m_link.mu);
			m_link.end.reason    = kEndError;
			m_link.end.logReason = "error";
			m_link.end.text      = "This game's settings differ from the host's.";
		}
		handle_end();
		return;
	}
	p2_net_set_synced_hash(m_cfg.syncedHash);
	p2_net_set_online(1);
	logline(fmt("online start mode=%s stage=%u seats=%u seat=%d code=%s seed=%u", mode_name(m_cfg.mode), (unsigned)m_cfg.stage,
	            (unsigned)m_cfg.seats, m_seat, code().c_str(), m_cfg.seed));
	p2_launcher_online_started();
}

void Session::write_report(const EndInfo& e, std::string& path)
{
	const std::string dir = pref_dir() + "net/reports/";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	path    = dir + "desync-" + code() + "-t" + std::to_string(e.desyncTick) + ".txt";
	FILE* f = std::fopen(path.c_str(), "w");
	if (!f) {
		path += " (could not be written)";
		return;
	}
	uint8_t bh[32], ch[32];
	build_hash(bh);
	p2_mods_content_hash(ch);
	auto hex = [](const uint8_t* p) {
		std::string s;
		for (int i = 0; i < 32; ++i) s += fmt("%02x", p[i]);
		return s;
	};
	std::fprintf(f, "mppm desync report\n");
	std::fprintf(f, "session %s  tick %u  seat %d  (slot reported: %d)\n", code().c_str(), e.desyncTick, m_seat, (int)e.desyncSlot);
	std::fprintf(f, "seed %u\n", m_cfg.seed);
	std::fprintf(f, "host total %08x  peer total %08x\n", e.hostTotal, e.peerTotal);
	std::fprintf(f, "build hash %s\ncontent hash %s\nmods %s\n", hex(bh).c_str(), hex(ch).c_str(), m_cfg.mods.empty() ? "none" : m_cfg.mods.c_str());
	std::fprintf(f, "config mode=%s stage=%u seats=%u slotMask=%x members=%u,%u,%u,%u handicap=%u,%u tickOrder=%u simK=%.2f interval=%u synced=%08x\n",
	             mode_name(m_cfg.mode), (unsigned)m_cfg.stage, (unsigned)m_cfg.seats, (unsigned)m_cfg.slotMask, m_cfg.slotMember[0],
	             m_cfg.slotMember[1], m_cfg.slotMember[2], m_cfg.slotMember[3], (unsigned)m_cfg.handicap[0], (unsigned)m_cfg.handicap[1],
	             (unsigned)m_cfg.tickOrder, (double)m_cfg.simK, (unsigned)m_cfg.cksInterval, m_cfg.syncedHash);
	std::fprintf(f, "recording %s\n", seam::record_path().empty() ? "-" : seam::record_path().c_str());
	std::fprintf(f, "host line for tick %u: %s\n", e.desyncTick, e.hostLine.empty() ? "(not known)" : e.hostLine.c_str());
	for (const std::string& l : e.peerLines) std::fprintf(f, "peer line: %s\n", l.c_str());
	std::fprintf(f, "this machine's last checksum lines:\n");
	for (const std::string& l : seam::last_lines()) std::fprintf(f, "  %s\n", l.c_str());
	std::fclose(f);
}

void Session::handle_end()
{
	if (m_endHandled) return;
	m_endHandled = true;
	EndInfo e;
	{
		std::lock_guard<std::mutex> lk(m_link.mu);
		e = m_link.end;
	}
	int failExit;
	{
		std::lock_guard<std::mutex> lk(m_mu);
		failExit = m_failExit;
	}
	m_endLog = e.logReason.empty() ? "error" : e.logReason;
	switch (e.reason) {
	case kEndDesync: {
		std::string path;
		write_report(e, path);
		logline(fmt("desync at tick %u (slot %d: host %08x peer %08x); report %s", e.desyncTick, e.desyncSlot == kNoSlot ? -1 : (int)e.desyncSlot,
		            e.hostTotal, e.peerTotal, path.c_str()));
		std::lock_guard<std::mutex> lk(m_mu);
		m_reportPath = path;
		m_error      = fmt("Desync at tick %u. The match was stopped on every machine. A report was saved to %s.", e.desyncTick, path.c_str());
		m_desyncTick = e.desyncTick;
		m_snapDirty  = true;
		m_exitCode   = 7;
		m_endLog     = "desync";
		break;
	}
	case kEndLeft:
	case kEndDone:
		m_exitCode = 0;
		break;
	case kEndRefused:
	case kEndServer:
		m_exitCode = 9;
		break;
	default:
		m_exitCode = 8;
		break;
	}
	if (failExit) m_exitCode = failExit;
	if (e.reason == kEndLeft) m_endLog = "left";
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_phase     = online::Phase::Ended;
		m_endReason = e.reason == kEndDone ? online::kEndNone : e.reason;
		if (e.reason != kEndDesync) m_error = e.text;
		m_snapDirty = true;
	}
	// The end line now (a UI peer may restart without p2_net_shutdown); the summary follows at shutdown.
	if (m_startApplied) log_end(m_endLog);
}

void Session::log_end(const std::string& reason)
{
	if (m_endLogged) return;
	m_endLogged = true;
	logline("session end reason=" + reason);
}

void Session::game_poll(bool gameRunning)
{
	if (gameRunning && !g_gameBooted.load()) g_gameBooted = true;
	if (gameRunning && m_commandLine && !m_infoCaptured) capture_game_info();
	if (gameRunning && m_preBoot && !m_bootChecked && m_gameInfoReady.load()) {
		// A session from the start menu announced the folder's mod set; the mount has now run. If it
		// dropped a mod (or the folder changed meanwhile), this game would desync: leave instead.
		uint8_t now[32], ann[32];
		{
			std::lock_guard<std::mutex> lk(m_mu);
			std::memcpy(ann, m_announced, 32);
		}
		bool announced = false;
		for (const uint8_t b : ann) announced = announced || b != 0;
		if (announced) m_bootChecked = true; // else net_init has not run yet: compare on a later poll
		if (announced) p2_mods_content_hash(now);
		if (announced && std::memcmp(now, ann, 32) != 0) {
			logline("content after the boot differs from the one sent in the handshake; leaving");
			post([this] { fail(kEndRefused, "content", "The mods that loaded differ from the ones this game was joined with.", 8); });
			leave_now(wire::kByeLeft);
		}
	}
	m_side.poll();
	if (!m_startApplied && m_side.started()) apply_start();
	if (!m_endHandled && m_side.ended()) handle_end();
	// The host is gone and the next tick's bundle cannot come any more (a soft end: the bundles that
	// did arrive were played first).
	const uint32_t tick = p2_net_tick();
	if (!m_endHandled && m_startApplied && m_began && !(g_exitTick && tick >= g_exitTick) && m_side.over(tick + 1)) handle_end();
	if (m_startApplied && m_began) {
		std::lock_guard<std::mutex> lk(m_mu);
		if (m_phase == online::Phase::Starting) {
			m_phase     = online::Phase::InGame;
			m_snapDirty = true;
		}
	}
}

void Session::print_summary()
{
	if (m_summaryPrinted || !m_startApplied) return;
	m_summaryPrinted = true;
	const StallStats& st = m_side.stats();
	std::vector<int32_t> ph;
	std::string dList, link;
	int dMax;
	uint32_t rtt;
	double secs;
	uint64_t out, in;
	{
		std::lock_guard<std::mutex> lk(m_mu);
		ph    = m_phaseAbs;
		dList = m_dList.empty() ? "-" : m_dList;
		dMax  = m_dMax;
		rtt   = m_rttUs;
		out   = m_bytesOut;
		in    = m_bytesIn;
		link  = m_linkName;
		secs  = (double)(m_netEndNs - m_netStartNs) / 1e9;
	}
	long long p50 = -1, p99 = -1, pmax = -1;
	if (m_kind == Kind::Join && !ph.empty()) {
		std::sort(ph.begin(), ph.end());
		p50  = ph[(ph.size() - 1) / 2];
		p99  = ph[(size_t)(0.99 * (double)(ph.size() - 1))];
		pmax = ph.back();
	}
	if (secs <= 0) secs = 1;
	logline(fmt("summary ticks=%llu stalled=%llu rate=%.2f max_ms=%.1f barriers=%llu barrier_ms=%.1f phase_p50_us=%lld phase_p99_us=%lld "
	            "phase_max_us=%lld d=%s d_max=%d rtt_us=%u kbps_out=%.1f kbps_in=%.1f link=%s",
	            (unsigned long long)st.ticks, (unsigned long long)st.stalled, st.ticks ? 100.0 * (double)st.stalled / (double)st.ticks : 0.0, st.maxMs,
	            (unsigned long long)st.barriers, st.barrierMs, p50, p99, pmax, dList.c_str(), dMax, rtt, (double)out * 8.0 / 1000.0 / secs,
	            (double)in * 8.0 / 1000.0 / secs, link.c_str()));
}

void Session::shutdown(bool exitTickReached, uint32_t tick)
{
	const bool hostSide = m_kind != Kind::Join;
	if (m_startApplied && !m_endHandled && !m_left) {
		if (hostSide && exitTickReached) {
			m_side.done(tick);
			const int64_t until = steady_ns() + 10500000000LL;
			while (!m_lingerDone.load() && steady_ns() < until && !m_threadDone.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} else {
			m_side.leave(exitTickReached ? wire::kByeDone : wire::kByeLeft);
		}
	} else if (!m_startApplied && !m_endHandled) {
		m_side.leave(wire::kByeLeft);
		if (m_lobby) post([this] { if (m_lobby) m_lobby->leave(); });
	}
	stop(150000000LL);
	std::string reason = m_endHandled ? m_endLog : "done";
	if (!m_endHandled && m_left) reason = "left";
	log_end(reason);
	print_summary();
	if (m_startApplied) {
		p2_launcher_net_unlock();
		p2_net_set_online(0);
	}
}

// ---- globals --------------------------------------------------------------------------------------------

std::unique_ptr<Session> g_s;

std::string resolve_server()
{
	if (!g_cli.server.empty()) return g_cli.server;
	std::lock_guard<std::mutex> lk(g_settingsMu);
	if (!g_settings.serverUrl.empty()) return g_settings.serverUrl;
	return MPPM_DEFAULT_SERVER_URL;
}

std::string resolve_name()
{
	std::string name;
	if (!g_cli.name.empty()) name = g_cli.name;
	else {
		std::lock_guard<std::mutex> lk(g_settingsMu);
		name = g_settings.playerName;
	}
	// Bounded and cleaned like mppm-server does (24 code points, cut at a character boundary).
	name = wire::clean_name(name);
	return name.empty() ? "Player" : name;
}

void reap()
{
	if (g_s && g_s->left() && g_s->thread_done()) g_s.reset();
}

} // namespace

// ==== session.hpp ======================================================================================

void cli_init(int argc, char** argv, bool replayOrHeadless, int vsStage, int chStage)
{
	read_cli_env(g_cli);
	read_cli_args(g_cli, argc, argv, cli_wants_online(argc, argv));
	if (g_cli.bots > 0) g_cli.host = true;
	if (!g_cli.anyOnlineFlag) return;
	if (!g_cli.envError.empty()) bad_flags(g_cli.envError);
	if (g_cli.host && !g_cli.join.empty()) bad_flags("--host and --join exclude each other");
	if (replayOrHeadless) bad_flags("online sessions run in real time and cannot replay (--replay and --headless-ticks are not allowed)");
	if (g_cli.host && (vsStage >= 0 || chStage >= 0)) bad_flags("--host chooses the stage with --mode and --stage, not --vs-stage or --ch-stage");
	if (g_cli.spectate && g_cli.join.empty()) bad_flags("--spectate needs --join CODE");
	if (g_cli.mode == Mode::Challenge && g_cli.stage > 29) bad_flags("bad --stage for challenge (0 to 29)");
	if (g_cli.handicapGiven && g_cli.mode != Mode::Battle) bad_flags("--handicap is for --mode battle only");
	if (g_cli.slot >= mode_seats(g_cli.mode) && g_cli.host) bad_flags("--slot is not a seat of this mode");
	g_cliOnline = true;
	const Kind kind = g_cli.bots > 0 ? Kind::Bots : (g_cli.host ? Kind::Host : Kind::Join);
	const std::string server = kind == Kind::Bots ? std::string() : resolve_server();
	if (kind != Kind::Bots && server.empty()) bad_flags("no lobby server: give --server URL (or MPPM_SERVER_URL)");
	if (kind == Kind::Join) {
		std::string c8;
		if (!net::normalize_code(g_cli.join, c8)) bad_flags("bad --join code '" + g_cli.join + "' (8 symbols, like ABCD-EFGH)");
		g_cli.join = c8;
	}
	std::string name = resolve_name();
	std::fprintf(stderr, "mppm: net: online (server=%s, role=%s, mode=%s, net-mode=%s, bind=%s)\n", server.empty() ? "-" : server.c_str(),
	             kind == Kind::Bots ? "bots" : (kind == Kind::Host ? "host" : "join"), mode_name(g_cli.mode), net_mode_name(g_cli.netMode),
	             g_cli.bind.empty() ? "0.0.0.0" : g_cli.bind.c_str());
	if (!g_cli.impair.empty()) {
		net::Impair im;
		std::string err;
		if (!net::parse_impair(g_cli.impair, im, err)) bad_flags("bad --net-impair '" + g_cli.impair + "': " + err);
		if (kind == Kind::Bots && im.enabled())
			std::fprintf(stderr, "mppm: net: impairment delay_us=%u jitter_us=%u loss_ppm=%u dup_ppm=%u seed=%u (test only)\n", im.delayUs, im.jitterUs,
			             im.lossPpm, im.dupPpm, im.seed);
	}
	std::fflush(stderr);
	g_s = std::make_unique<Session>(kind, g_cli, server, name, true);
	std::string err;
	if (!g_s->start(err)) {
		if (err.rfind("bad --net-impair", 0) == 0 || err.rfind("--net-input", 0) == 0) bad_flags(err);
		std::fprintf(stderr, "mppm: net: lobby failed error=unreachable msg=%s\n", err.c_str());
		std::fflush(stderr);
		std::exit(9);
	}
}

bool cli_online() { return g_cliOnline; }
const char* cli_net_input() { return g_cli.netInput.c_str(); }

bool exists() { return g_s != nullptr; }

void game_poll(bool gameRunning)
{
	reap();
	if (g_s) g_s->game_poll(gameRunning);
}

bool started() { return g_s && g_s->started(); }
bool in_session() { return g_s && g_s->started() && g_s->began(); }

const SessionConfig& config()
{
	static const SessionConfig kNone;
	return g_s ? g_s->cfg() : kNone;
}

int local_seat() { return g_s && g_s->started() ? g_s->seat() : -1; }
std::string code() { return g_s ? g_s->code() : std::string("local"); }
bool ready(uint32_t tick) { return g_s && g_s->side().ready(tick); }
bool over(uint32_t tick) { return !g_s || g_s->side().over(tick); }
bool take(uint32_t tick, Input out[kMaxSeats], uint8_t& mask) { return g_s && g_s->side().take(tick, out, mask); }

void push_read(uint32_t tick, int64_t dueNs, int64_t slewNs, bool haveInput, const Input& in)
{
	if (!g_s) return;
	g_s->side().tick_due(tick, dueNs, slewNs);
	if (haveInput) {
		Input use = in;
		g_s->local_input(tick, use);
		g_s->side().input(tick, use);
	}
}

void push_checksum(uint32_t tick, uint32_t total, const char* line)
{
	if (g_s) g_s->side().checksum(tick, total, line);
}

void push_loaded(uint32_t tick)
{
	if (!g_s) return;
	g_s->side().loaded(tick);
	if (g_s->kind() == Kind::Bots) host_loaded(tick, g_s->side().load_count());
}

void host_loaded(uint32_t tick, uint16_t count)
{
	if (!g_s) return;
	g_s->bots_shared().loadTick.store(tick, std::memory_order_release);
	g_s->bots_shared().loadCount.store(count, std::memory_order_release);
}

void session_began()
{
	if (!g_s) return;
	g_s->set_began();
	g_s->side().begun();
}

void note_read(uint32_t tick, int64_t waitedPastDueNs, bool barrier)
{
	if (g_s) g_s->side().note_read(tick, waitedPastDueNs, barrier);
}

void note_prewait(uint32_t tick, int64_t waitedPastDueNs)
{
	if (g_s) g_s->side().note_prewait(tick, waitedPastDueNs);
}

void wait_state(bool waiting, uint32_t tick, int64_t sinceNs, bool loading)
{
	if (g_s) g_s->side().wait_state(waiting, tick, sinceNs, loading);
}

bool barrier_pending() { return g_s && g_s->side().barrier_pending(); }

void reached_tick(uint32_t tick)
{
	if (g_s && g_s->kind() == Kind::Bots) g_s->bots_shared().hostRead.store(tick, std::memory_order_release);
}

void leave_now(uint8_t byeReason)
{
	if (g_s) g_s->leave_now(byeReason);
}

bool crash_leave()
{
	Session* s = g_s.get();
	if (!s || !s->started() || s->thread_done()) return false;
	s->link().leaveReq.store(wire::kByeError, std::memory_order_release);
	return true;
}

bool ended() { return g_s && g_s->ended(); }
int exit_code() { return g_s ? g_s->exit_code() : 0; }
bool command_line_run() { return g_s && g_s->command_line(); }
void set_exit_tick(uint32_t tick) { g_exitTick = tick; }

void shutdown(bool exitTickReached, uint32_t tick)
{
	if (!g_s) return;
	g_s->shutdown(exitTickReached, tick);
}

} // namespace mppm::session

// ==== launcher API (p2_online.hpp) ======================================================================

namespace mppm::online {

using mppm::session::g_s;

void configure(const Settings& settings)
{
	std::lock_guard<std::mutex> lk(session::g_settingsMu);
	session::g_settings = settings;
}

bool host(const Config& config, const std::string& password, std::string& err)
{
	session::reap();
	if (g_s && !g_s->left()) {
		err = "Leave the current game first.";
		return false;
	}
	const std::string server = session::resolve_server();
	if (server.empty()) {
		err = "No lobby server is set. Choose one in Settings › Online.";
		return false;
	}
	session::Cli c   = session::g_cli;
	c.host           = true;
	c.join.clear();
	c.mode           = (session::Mode)config.mode;
	c.stage          = config.stage;
	c.handicap[0]    = config.handicap[0];
	c.handicap[1]    = config.handicap[1];
	c.password       = password;
	c.ready          = false;
	c.bots           = 0;
	g_s.reset();
	g_s = std::make_unique<session::Session>(session::Kind::Host, c, server, session::resolve_name(), false);
	if (!g_s->start(err)) {
		g_s.reset();
		if (err.find("not merged") != std::string::npos || err.empty()) err = "Couldn't reach the lobby server.";
		return false;
	}
	return true;
}

bool join(const std::string& code, bool spectator, const std::string& password, std::string& err)
{
	session::reap();
	if (g_s && !g_s->left()) {
		err = "Leave the current game first.";
		return false;
	}
	std::string c8;
	if (!net::normalize_code(code, c8)) {
		err = "That code isn't valid. Codes look like ABCD-EFGH.";
		return false;
	}
	const std::string server = session::resolve_server();
	if (server.empty()) {
		err = "No lobby server is set. Choose one in Settings › Online.";
		return false;
	}
	session::Cli c = session::g_cli;
	c.host         = false;
	c.join         = c8;
	c.spectate     = spectator;
	c.password     = password;
	c.ready        = false;
	c.bots         = 0;
	g_s.reset();
	g_s = std::make_unique<session::Session>(session::Kind::Join, c, server, session::resolve_name(), false);
	if (!g_s->start(err)) {
		g_s.reset();
		if (err.find("not merged") != std::string::npos || err.empty()) err = "Couldn't reach the lobby server.";
		return false;
	}
	return true;
}

void leave()
{
	if (!g_s) return;
	g_s->ui_leave();
	session::reap();
}

void wait_left(int maxMs)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
	while (g_s && g_s->left() && !g_s->thread_done() && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	session::reap();
}

void set_ready(bool ready)
{
	if (session::Session* s = g_s.get()) s->post([s, ready] { s->cmd_ready(ready); });
}

void take_slot(int slot)
{
	if (session::Session* s = g_s.get()) s->post([s, slot] { s->cmd_slot(slot); });
}

void set_config(const Config& config)
{
	if (session::Session* s = g_s.get()) s->post([s, config] { s->cmd_config(config); });
}

void start()
{
	if (session::Session* s = g_s.get()) s->post([s] { s->cmd_start(); });
}

void kick(uint32_t memberId)
{
	if (session::Session* s = g_s.get()) s->post([s, memberId] { s->cmd_kick(memberId); });
}

void drop_slot(int slot)
{
	if (session::Session* s = g_s.get()) s->post([s, slot] { s->cmd_drop(slot); });
}

Snapshot snapshot()
{
	session::reap();
	if (!g_s || g_s->left()) return Snapshot{};
	return g_s->snapshot();
}

Stall stall()
{
	if (!g_s || g_s->left()) return Stall{};
	return g_s->stall_view();
}

std::vector<Notice> take_notices()
{
	if (!g_s) return {};
	return g_s->take_notices();
}

bool active()
{
	if (!g_s || g_s->left()) return false;
	const Phase p = g_s->snapshot().phase;
	return p == Phase::Connecting || p == Phase::Lobby || p == Phase::Starting || p == Phase::InGame;
}

bool in_game()
{
	if (!g_s || g_s->left()) return false;
	const Phase p = g_s->snapshot().phase;
	return p == Phase::Starting || p == Phase::InGame;
}

bool normalize_code(const std::string& in, std::string& out) { return net::normalize_code(in, out); }

} // namespace mppm::online
