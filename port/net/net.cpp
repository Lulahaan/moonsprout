// Online harness core, milestones M5.1a and M5.1b (docs/ONLINE_MULTIPLAYER.md 2.1, 2.3, 2.4, 2.5,
// 2.6, 2.7, 2.10), the content seam of M5.2 (7b) and the captain count of M5.3a. Input seam, session
// and seed, input-log record (version 3, version 4 with more than 2 captains) and replay (versions 2
// to 4), checksum log writer and verify, nonsim regions, tick order, sim frustum k, floor reseed, RNG
// self-test, build id, the mods hash, the replay content gate, --vs-players, --view-captain and the
// MPPM_PERF tick timer.
// The interface and the terms are documented in port/include/p2_net.h.
//
// Host-side C++ only: no game headers. With no flag and no environment variable set, every entry
// point returns at its first line, nothing is opened and nothing is printed.
#include "p2_net.h"
#include "p2_data_dir.hpp"
#include "p2_mods.h"
#include "session/session.hpp"

#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" void p2_msl_srand(unsigned int seed);
extern "C" int p2_msl_rand(void);
extern "C" void p2_rng_test_set_perturb(uint32_t pres, uint32_t other); // port/sdk/msl_compat.c, self-test only

namespace {

constexpr uint32_t kLogMagic        = 0x4C4E504Du; // 'MPNL' read as little-endian u32
constexpr uint16_t kLogVersion      = 3;     // what the engine writes with 1-2 captains; it reads 2 to 4
constexpr uint16_t kLogVersionV2    = 2;
constexpr uint16_t kLogVersionV4    = 4;     // M5.3a: version 3 plus byte 33 = captains, only when N > 2
constexpr uint32_t kCaptainsByte    = 33;    // version 4: captains (4); 0 or 2 read as 2
constexpr uint32_t kLogHeaderSize   = 64;    // version 2's header, the first read
constexpr uint32_t kLogV3FixedSize  = 128;   // version 3: v2's 64 bytes, mods hash, content hash
constexpr uint32_t kLogV3ListMin    = 16;    // the mod list area: NUL-terminated, 16-byte padded
constexpr uint32_t kLogV3ListMax    = 1024;
constexpr uint32_t kLogHashSize     = 32;
constexpr uint32_t kLogSlots        = 4;
constexpr uint32_t kLogSlotSize     = 12;
constexpr uint32_t kLogRecordSize   = kLogSlots * kLogSlotSize; // 48
constexpr uint32_t kTickCountOpen   = 0xFFFFFFFFu;
constexpr uint32_t kDefaultInterval = 30;
constexpr int kExitReadsAfterEnd    = 3;
constexpr uint32_t kMaxVsStage      = 254;     // the record header stores the stage in a byte, 0xFF = none
constexpr uint64_t kMaxReplayBytes  = 1ull << 30; // 1 GiB: about 22 million ticks, over 200 hours
constexpr float kDefaultSimK        = 3.6f;      // the widest k the renderer supports (kMaxK)
constexpr float kMinSimK            = 1.0f;
constexpr uint32_t kEventSlotByte   = 11;        // port 0's slot: the test event code (version 2)
constexpr int kStockCaptains        = 2;
constexpr uint16_t kLogVersionV5    = 5;         // M5.3b: version 4 plus start mode 2 (Challenge) and the VS handicaps
constexpr uint32_t kHandicapByte    = 34;        // version 5: handicap of team 0 (Red), 35: team 1 (Blue); 0 = stock
constexpr uint32_t kMaxChStage      = 29;        // 2P Challenge courses 0..29
constexpr uint32_t kOnlineFlag      = 2u;        // header flags bit 1: recorded in an online session
constexpr int kMaxCaptains          = 4;         // P2_MAX_NAVI; part 1 accepts only 2 or 4 (design D16)
// Exit codes (p2_net_exit_code): 2 start-up failure, 3 checksum mismatch, 4 a log write failed,
// 5 the crash screen came up in a headless run.
constexpr int kExitMismatch   = 3;
constexpr int kExitWriteError = 4;
constexpr int kExitCrash      = 5;
constexpr int kExitRngStrict  = 6; // MPPM_RNG_STRICT=1 and an OTHER-stream draw during the session

struct Config {
	std::string record, replay, cksumLog, cksumVerify;
	bool haveSeed = false;
	uint32_t seed = 0;
	int vsStage   = -1; // from --vs-stage
	bool headless = false;
	bool probe    = false;
	bool haveExitAfter = false;
	uint32_t exitAfter = 0;
	uint32_t interval  = kDefaultInterval;
	int tickOrder      = -1;   // --tick-order / MPPM_TICK_ORDER, -1 = not given
	float simK         = 0.0f; // --sim-k / MPPM_SIM_K, 0 = not given
	int vsPlayers      = 0;    // M5.3a: --vs-players / MPPM_VS_PLAYERS (2 or 4), 0 = not given
	int viewCaptain    = -1;   // M5.3a: --view-captain / MPPM_VIEW_CAPTAIN (0..3), -1 = stock split
	int chStage        = -1;   // M5.3b: --ch-stage / MPPM_CH_STAGE (0..29), -1 = none
};

struct ReplayLog {
	bool loaded = false;
	uint32_t seed = 0;
	uint64_t buildId = 0;
	uint8_t startMode = 0;
	uint8_t vsStage = 0xFF;
	uint32_t flags = 0;
	uint8_t tickOrder = P2_TICK_ORDER_UPDATE_FIRST; // version 2: byte 32
	float simK        = 0.0f;                       // version 2: bytes 36..39, 0 = the default
	uint32_t syncedHash = 0;                        // version 2: bytes 40..43, 0 = not recorded
	uint16_t version = 0;
	uint8_t modsHash[kLogHashSize]    = {};         // version 3: bytes 64..95, zero = stock (and v2)
	uint8_t contentHash[kLogHashSize] = {};         // version 3: bytes 96..127, informational
	std::string modsList;                           // version 3: from byte 128, "" = stock
	int captains = kStockCaptains;                  // version 4: byte 33; versions 2 and 3: 2
	uint8_t handicap[2] = { 0, 0 };                 // version 5: bytes 34, 35
	std::vector<uint8_t> records; // count * 48
	uint32_t count = 0;
};

// State. The seam runs on the main game thread; p2_net_crash_stop may come from the exception
// thread, so file access is serialised by s_mutex and the crash flag is atomic.
bool s_inited  = false;
bool s_anyOn   = false; // any flag at all
bool s_harness = false; // record / replay / checksum-log / checksum-verify / vs-stage
Config s_cfg;
ReplayLog s_replay;

std::atomic<bool> s_crashed { false };
std::mutex s_mutex;

bool s_sessionActive = false;
bool s_sessionEnded  = false; // shutdown ran: a session never restarts
uint32_t s_tick      = 0;
uint64_t s_reads     = 0; // pad reads since boot
uint32_t s_seed      = 0;
bool s_haveClockSeed = false;
uint32_t s_clockSeed = 0;
int s_readsAfterEnd  = 0;

FILE* s_recFile       = nullptr;
uint32_t s_recCount   = 0;
FILE* s_cksumFile     = nullptr;
FILE* s_verifyFile    = nullptr;
bool s_verifyOn       = false; // a verify file was opened for this session
bool s_mismatch       = false;
uint32_t s_lastCTick  = kTickCountOpen; // tick of the last C line
uint32_t s_rolling    = 0;              // CRC-32 state (pre-inverted) over every total
uint32_t s_lineCount  = 0;              // C and L lines
uint32_t s_lastTotal  = 0;
bool s_resetWarned    = false;
bool s_writeError     = false; // a write, flush or close of the record or checksum log failed

std::once_flag s_buildIdOnce; // build_id() may be called from a launcher worker thread
uint64_t s_buildId = 0;

// M5.2: the content of this run (p2_net_set_content). Stock until it is called.
uint8_t s_modsHash[kLogHashSize] = {};
std::string s_modsList;
bool s_contentStock = true;

// M5.1b
std::atomic<int> s_online { 0 };   // p2_net_set_online
int s_nonsimDepth     = 0;         // main thread
bool s_nonsimWarned   = false;
uint8_t s_curEvent    = 0;         // test event code of the record consumed this tick
uint32_t s_syncedHash = 0;         // p2_net_set_synced_hash
bool s_orderNoticed   = false;

// M5.3b: online sessions (port/net/session/), main thread only.
std::deque<std::string> s_lines;   // the last 64 checksum lines (online: the desync report)
std::string s_recPath;             // the record log of this session ("" none)
bool s_leaving        = false;     // MPPM_NET_TEST_LEAVE_AT reached: the run ends after this pass
uint32_t s_testDesync = 0;         // MPPM_NET_TEST_DESYNC
uint32_t s_testLeave  = 0;         // MPPM_NET_TEST_LEAVE_AT
uint32_t s_testHang   = 0;         // MPPM_NET_TEST_HANG_AT=<tick>[:<ms>]: the game thread sleeps after that tick
uint32_t s_testHangMs = 0;
int64_t s_testSlew    = 0;         // MPPM_NET_TEST_SLEW_NS
uint32_t s_slewCount  = 0;         // the slew test's reference retrace
long long s_slewPred  = 0;
bool s_slewPending    = false;
int64_t s_slewSetNs   = 0;

// M5.3a: MPPM_PERF=1 tick timer (p2_net_perf_sim_begin/end), main thread only.
using PerfClock = std::chrono::steady_clock;
int s_perfOn          = -1;      // -1 = MPPM_PERF not read yet
bool s_perfInTick     = false;   // a begin without its end
bool s_perfHavePrev   = false;   // a previous begin to measure the pass from
bool s_perfGap        = false;   // a nonsim region since the previous begin: that pass is not counted
PerfClock::time_point s_perfBegin, s_perfPrev;
std::vector<float> s_perfSim;    // per-tick sim time, ms
double s_perfPassSum  = 0.0;     // ms
uint64_t s_perfPasses = 0;

// ---- helpers --------------------------------------------------------------------------------

uint32_t crc_table[256];
bool crc_ready = false;

void crc_init()
{
	if (crc_ready) return;
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i;
		for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_ready = true;
}

// Raw (pre/post-inversion handled by the caller) CRC-32 update.
uint32_t crc_update(uint32_t state, const uint8_t* p, size_t n)
{
	crc_init();
	for (size_t i = 0; i < n; ++i) state = crc_table[(state ^ p[i]) & 0xFF] ^ (state >> 8);
	return state;
}

void put_u32le(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
void put_u16le(uint8_t* p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
void put_u64le(uint8_t* p, uint64_t v)
{
	for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
uint32_t get_u32le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t get_u16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint64_t get_u64le(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
	return v;
}

// CRC-32 (IEEE) over a list of little-endian u32 values.
uint32_t crc_u32s(const uint32_t* v, size_t n)
{
	uint32_t s = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i) {
		uint8_t b[4];
		put_u32le(b, v[i]);
		s = crc_update(s, b, 4);
	}
	return s ^ 0xFFFFFFFFu;
}

// FNV-1a 64 of the executable file, 0 when it cannot be read. Call build_id(), which runs this once.
uint64_t compute_build_id()
{
	std::string exe;
#if defined(__linux__)
	exe = "/proc/self/exe";
#elif defined(__APPLE__)
	char pathBuf[4096];
	uint32_t pathSize = sizeof(pathBuf);
	if (_NSGetExecutablePath(pathBuf, &pathSize) == 0) exe = pathBuf;
#elif defined(_WIN32)
	char pathBuf[MAX_PATH * 4];
	DWORD len = GetModuleFileNameA(nullptr, pathBuf, sizeof(pathBuf));
	if (len > 0 && len < sizeof(pathBuf)) exe = std::string(pathBuf, len);
#endif
	if (exe.empty()) return 0;
	{
	FILE* f = std::fopen(exe.c_str(), "rb");
	if (!f) return 0;
	uint64_t h = 0xCBF29CE484222325ull;
	static uint8_t buf[1 << 16];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		for (size_t i = 0; i < n; ++i) {
			h ^= buf[i];
			h *= 0x100000001B3ull;
		}
	}
	bool ok = !std::ferror(f);
	std::fclose(f);
	return ok ? h : 0;
	}
}

uint64_t build_id()
{
	std::call_once(s_buildIdOnce, [] { s_buildId = compute_build_id(); });
	return s_buildId;
}

void hex_of(const uint8_t* p, size_t n, char* out)
{
	static const char kHex[] = "0123456789abcdef";
	for (size_t i = 0; i < n; ++i) {
		out[2 * i]     = kHex[p[i] >> 4];
		out[2 * i + 1] = kHex[p[i] & 15];
	}
	out[2 * n] = '\0';
}

bool all_zero(const uint8_t* p, size_t n)
{
	for (size_t i = 0; i < n; ++i)
		if (p[i]) return false;
	return true;
}

// A mod list for a log line: "none" when empty, anything but printable ASCII as '?' (the list of a
// replay header is untrusted input).
std::string printable_list(const std::string& list)
{
	if (list.empty()) return "none";
	std::string out = list;
	for (char& ch : out)
		if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) ch = '?';
	return out;
}

// The mod list text as the version 3 header stores it: at most kLogV3ListMax - 1 bytes (a longer
// list is cut; the mods hash is what the gate compares).
std::string header_list()
{
	std::string l = s_modsList;
	if (l.size() > kLogV3ListMax - 1) l.resize(kLogV3ListMax - 1);
	return l;
}

// Bytes of the version 3 list area for a list: NUL-terminated, zero-padded to a multiple of 16.
uint32_t list_area_size(size_t len) { return (uint32_t)((len + 1 + 15) / 16 * 16); }

// A whole decimal, hex (0x) or octal (0) number from 0 to 2^32-1; anything else (a sign, trailing
// text, a value out of range) is rejected rather than wrapped.
bool parse_u32(const char* s, uint32_t* out)
{
	if (!s || !*s) return false;
	while (*s == ' ' || *s == '\t') ++s;
	if (*s < '0' || *s > '9') return false; // strtoull would accept "-1" as 2^64-1
	char* end = nullptr;
	errno = 0;
	unsigned long long v = std::strtoull(s, &end, 0);
	if (end == s || *end != '\0' || errno == ERANGE || v > 0xFFFFFFFFull) return false;
	*out = (uint32_t)v;
	return true;
}

// "update" / "draw" (also "update-first", "draw-first", "1", "0"); -1 when bad.
int parse_order(const char* s)
{
	if (!s) return -1;
	if (!std::strcmp(s, "update") || !std::strcmp(s, "update-first") || !std::strcmp(s, "1")) return P2_TICK_ORDER_UPDATE_FIRST;
	if (!std::strcmp(s, "draw") || !std::strcmp(s, "draw-first") || !std::strcmp(s, "0")) return P2_TICK_ORDER_DRAW_FIRST;
	return -1;
}

// A sim k from 1.0 to 3.6; false when bad.
bool parse_simk(const char* s, float* out)
{
	if (!s || !*s) return false;
	char* end = nullptr;
	errno     = 0;
	double v  = std::strtod(s, &end);
	if (end == s || *end != '\0' || errno == ERANGE || !(v >= kMinSimK && v <= kDefaultSimK + 1e-6)) return false;
	*out = (float)v;
	if (*out > kDefaultSimK) *out = kDefaultSimK;
	return true;
}

// M5.3a: the captain count of --vs-players / MPPM_VS_PLAYERS. Only 2 and 4 exist in part 1 (3 needs the
// third onyon); anything else ends the run with exit 2, so a test never plays 2 captains by mistake.
int parse_players(const char* s, const char* what)
{
	uint32_t n = 0;
	if (parse_u32(s, &n) && (n == (uint32_t)kStockCaptains || n == (uint32_t)kMaxCaptains)) return (int)n;
	std::fprintf(stderr, "mppm: net: bad %s '%s' (2 or 4 captains; 3 teams are not supported yet); exiting 2\n", what, s);
	std::fflush(stderr);
	std::exit(2);
}

// M5.3a: --view-captain / MPPM_VIEW_CAPTAIN, 0..3. Presentation only, so a bad value is ignored.
void parse_view_captain(const char* s, const char* what, int* out)
{
	uint32_t n = 0;
	if (parse_u32(s, &n) && n < (uint32_t)kMaxCaptains) *out = (int)n;
	else std::fprintf(stderr, "mppm: net: bad %s '%s' (0 to %d); ignored\n", what, s, kMaxCaptains - 1);
}

bool env_true(const char* name)
{
	const char* v = std::getenv(name);
	return v && *v && std::strcmp(v, "0") != 0;
}

void read_env(Config& c)
{
	const char* v;
	if ((v = std::getenv("MPPM_RECORD")) && *v) c.record = v;
	if ((v = std::getenv("MPPM_REPLAY")) && *v) c.replay = v;
	if ((v = std::getenv("MPPM_CHECKSUM_LOG")) && *v) c.cksumLog = v;
	if ((v = std::getenv("MPPM_CHECKSUM_VERIFY")) && *v) c.cksumVerify = v;
	uint32_t n;
	if (parse_u32(std::getenv("MPPM_SEED"), &n)) {
		c.haveSeed = true;
		c.seed     = n;
	}
	if (const char* v = std::getenv("MPPM_VS_STAGE"); v && *v) {
		if (parse_u32(v, &n) && n <= kMaxVsStage) c.vsStage = (int)n;
		else std::fprintf(stderr, "mppm: net: bad MPPM_VS_STAGE '%s' (0 to %u); ignored\n", v, kMaxVsStage);
	}
	if (env_true("MPPM_HEADLESS_TICKS")) c.headless = true;
	if (env_true("MPPM_LATENCY_PROBE")) c.probe = true;
	if (parse_u32(std::getenv("MPPM_EXIT_AFTER_TICKS"), &n)) {
		c.haveExitAfter = true;
		c.exitAfter     = n;
	}
	if (parse_u32(std::getenv("MPPM_CHECKSUM_INTERVAL"), &n) && n > 0) c.interval = n;
	if (const char* v = std::getenv("MPPM_TICK_ORDER"); v && *v) {
		int o = parse_order(v);
		if (o >= 0) c.tickOrder = o;
		else std::fprintf(stderr, "mppm: net: bad MPPM_TICK_ORDER '%s' (update or draw); ignored\n", v);
	}
	if (const char* v = std::getenv("MPPM_SIM_K"); v && *v) {
		float k;
		if (parse_simk(v, &k)) c.simK = k;
		else std::fprintf(stderr, "mppm: net: bad MPPM_SIM_K '%s' (1.0 to 3.6); ignored\n", v);
	}
	if (const char* v = std::getenv("MPPM_VS_PLAYERS"); v && *v) c.vsPlayers = parse_players(v, "MPPM_VS_PLAYERS");
	if (const char* v = std::getenv("MPPM_VIEW_CAPTAIN"); v && *v) parse_view_captain(v, "MPPM_VIEW_CAPTAIN", &c.viewCaptain);
	if (const char* v = std::getenv("MPPM_CH_STAGE"); v && *v) { // M5.3b
		if (parse_u32(v, &n) && n <= kMaxChStage) c.chStage = (int)n;
		else std::fprintf(stderr, "mppm: net: bad MPPM_CH_STAGE '%s' (0 to %u); ignored\n", v, kMaxChStage);
	}
}

// Flags override the environment. Unknown arguments are ignored (the launcher and Aurora read
// the same argv).
void read_args(Config& c, int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (!a || a[0] != '-' || a[1] != '-') continue;
		std::string name(a + 2);
		const char* val = nullptr;
		std::string inlineVal;
		size_t eq = name.find('=');
		bool hasInline = eq != std::string::npos;
		if (hasInline) {
			inlineVal = name.substr(eq + 1);
			name      = name.substr(0, eq);
		}
		auto take = [&]() -> const char* {
			if (hasInline) return inlineVal.c_str();
			if (i + 1 < argc) return argv[++i];
			std::fprintf(stderr, "mppm: net: --%s needs a value\n", name.c_str());
			return nullptr;
		};
		uint32_t n;
		if (name == "record") {
			if ((val = take())) c.record = val;
		} else if (name == "replay") {
			if ((val = take())) c.replay = val;
		} else if (name == "checksum-log") {
			if ((val = take())) c.cksumLog = val;
		} else if (name == "checksum-verify") {
			if ((val = take())) c.cksumVerify = val;
		} else if (name == "seed") {
			if ((val = take())) {
				if (parse_u32(val, &n)) {
					c.haveSeed = true;
					c.seed     = n;
				} else {
					std::fprintf(stderr, "mppm: net: bad --seed '%s'\n", val);
				}
			}
		} else if (name == "vs-stage") {
			if ((val = take())) {
				if (parse_u32(val, &n) && n <= kMaxVsStage) c.vsStage = (int)n;
				else std::fprintf(stderr, "mppm: net: bad --vs-stage '%s' (0 to %u); ignored\n", val, kMaxVsStage);
			}
		} else if (name == "exit-after-ticks") {
			if ((val = take())) {
				if (parse_u32(val, &n)) {
					c.haveExitAfter = true;
					c.exitAfter     = n;
				} else {
					std::fprintf(stderr, "mppm: net: bad --exit-after-ticks '%s'\n", val);
				}
			}
		} else if (name == "headless-ticks") {
			c.headless = !hasInline || (inlineVal != "0");
		} else if (name == "latency-probe") {
			c.probe = !hasInline || (inlineVal != "0");
		} else if (name == "tick-order") {
			if ((val = take())) {
				int o = parse_order(val);
				if (o >= 0) c.tickOrder = o;
				else std::fprintf(stderr, "mppm: net: bad --tick-order '%s' (update or draw); ignored\n", val);
			}
		} else if (name == "sim-k") {
			if ((val = take())) {
				float k;
				if (parse_simk(val, &k)) c.simK = k;
				else std::fprintf(stderr, "mppm: net: bad --sim-k '%s' (1.0 to 3.6); ignored\n", val);
			}
		} else if (name == "vs-players") {
			if ((val = take())) c.vsPlayers = parse_players(val, "--vs-players");
		} else if (name == "view-captain") {
			if ((val = take())) parse_view_captain(val, "--view-captain", &c.viewCaptain);
		} else if (name == "ch-stage") { // M5.3b: direct boot into 2P Challenge course N
			if ((val = take())) {
				if (parse_u32(val, &n) && n <= kMaxChStage) c.chStage = (int)n;
				else {
					std::fprintf(stderr, "mppm: net: bad --ch-stage '%s' (0 to %u); exiting 2\n", val, kMaxChStage);
					std::fflush(stderr);
					std::exit(2);
				}
			}
		}
	}
}

// Whether two log paths name the same file (an existing file by identity, else by normalised path).
bool same_path(const std::string& a, const std::string& b)
{
	if (a.empty() || b.empty()) return false;
	if (a == b) return true;
	std::error_code ec;
	if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
	std::error_code ea, eb;
	const std::filesystem::path pa = std::filesystem::weakly_canonical(a, ea);
	const std::filesystem::path pb = std::filesystem::weakly_canonical(b, eb);
	return !ea && !eb && pa == pb;
}

[[noreturn]] void fail_init(const char* msg, const std::string& path)
{
	std::fprintf(stderr, "mppm: net: %s: %s\n", msg, path.c_str());
	std::fflush(stderr);
	std::exit(2);
}

void load_replay(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) fail_init("cannot open replay log", path);
	std::vector<uint8_t> data(kLogHeaderSize);
	if (std::fread(data.data(), 1, kLogHeaderSize, f) != kLogHeaderSize) {
		std::fclose(f);
		fail_init("replay log too short", path);
	}
	if (get_u32le(data.data()) != kLogMagic) {
		std::fclose(f);
		fail_init("replay log has a bad magic (want MPNL)", path);
	}
	uint8_t buf[1 << 14];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		if (data.size() + n > kMaxReplayBytes) {
			std::fclose(f);
			fail_init("replay log is larger than 1 GiB", path);
		}
		data.insert(data.end(), buf, buf + n);
	}
	std::fclose(f);
	const uint8_t* h = data.data();
	if (get_u16le(h + 4) == 1) {
		fail_init("replay log is version 1 (M5.1a): its ticks include loading passes; make a new one with netlog.py synth", path);
	}
	const uint16_t version = get_u16le(h + 4);
	if (version != kLogVersionV2 && version != kLogVersion && version != kLogVersionV4 && version != kLogVersionV5) fail_init("replay log has an unsupported version", path);
	uint32_t hdrSize = get_u16le(h + 6);
	if (hdrSize < kLogHeaderSize || hdrSize > data.size()) fail_init("replay log has a bad header size", path);
	s_replay.version = version;
	if (version >= kLogVersion) {
		// 128 fixed bytes, then the mod list area (16 to 1024 bytes) with a NUL inside it.
		if (hdrSize < kLogV3FixedSize + kLogV3ListMin || hdrSize > kLogV3FixedSize + kLogV3ListMax) {
			fail_init("replay log has a bad header size (versions 3 to 5: 144 to 1152)", path);
		}
		const uint8_t* list = h + kLogV3FixedSize;
		const size_t area   = hdrSize - kLogV3FixedSize;
		const void* nul     = std::memchr(list, 0, area);
		if (!nul) fail_init("replay log has a bad header (mod list without a NUL)", path);
		std::memcpy(s_replay.modsHash, h + 64, kLogHashSize);
		std::memcpy(s_replay.contentHash, h + 96, kLogHashSize);
		s_replay.modsList.assign((const char*)list, (const char*)nul - (const char*)list);
	}
	if (version == kLogVersionV4 || version == kLogVersionV5) {
		// M5.3a: byte 33 = captains. 0 and 2 mean the stock 2; part 1 plays only 4 more (design D16).
		const uint8_t n = h[kCaptainsByte];
		if (n == kMaxCaptains) s_replay.captains = kMaxCaptains;
		else if (n == 0 || n == kStockCaptains) s_replay.captains = kStockCaptains;
		else fail_init("replay log has a bad header (version 4: captains byte not 2 or 4)", path);
	}
	if (version == kLogVersionV5) {
		// M5.3b: the VS handicaps (0 = stock, else 1..10) and start mode 2 (2P Challenge, course at 27).
		for (int t = 0; t < 2; ++t) {
			const uint8_t hc = h[kHandicapByte + t];
			if (hc > 10) fail_init("replay log has a bad header (version 5: handicap above 10)", path);
			s_replay.handicap[t] = hc;
		}
		if (h[26] == 2 && h[27] > kMaxChStage) fail_init("replay log has a bad header (version 5: Challenge course above 29)", path);
	}
	if (h[26] > 2 || (h[26] == 2 && version != kLogVersionV5)) fail_init("replay log has a bad header (start mode)", path);
	if (h[24] != kLogSlots || h[25] != kLogRecordSize) fail_init("replay log has an unsupported record layout", path);
	s_replay.seed      = get_u32le(h + 8);
	uint32_t count     = get_u32le(h + 12);
	s_replay.buildId   = get_u64le(h + 16);
	s_replay.startMode = h[26];
	s_replay.vsStage   = h[27];
	s_replay.flags     = get_u32le(h + 28);
	if (h[32] != P2_TICK_ORDER_DRAW_FIRST && h[32] != P2_TICK_ORDER_UPDATE_FIRST) fail_init("replay log has a bad header (tick order byte)", path);
	s_replay.tickOrder = h[32];
	uint32_t kBits     = get_u32le(h + 36);
	if (kBits != 0) {
		float k;
		std::memcpy(&k, &kBits, 4);
		if (!(k >= kMinSimK && k <= kDefaultSimK)) fail_init("replay log has a bad header (sim k not 1.0 to 3.6)", path);
		s_replay.simK = k;
	}
	s_replay.syncedHash = get_u32le(h + 40);
	uint32_t avail     = (uint32_t)((data.size() - hdrSize) / kLogRecordSize);
	if (count == kTickCountOpen) count = avail;
	if (count > avail) {
		std::fprintf(stderr, "mppm: net: replay log claims %u ticks but holds %u; using %u\n", count, avail, avail);
		count = avail;
	}
	s_replay.count = count;
	s_replay.records.assign(data.begin() + hdrSize, data.begin() + hdrSize + (size_t)count * kLogRecordSize);
	s_replay.loaded = true;
	if (s_replay.buildId != 0 && s_replay.buildId != build_id()) {
		std::fprintf(stderr, "mppm: net: warning: replay log was made by another build (%016" PRIx64 ", this is %016" PRIx64 ")\n",
		             s_replay.buildId, build_id());
	}
}

void do_init(const Config& c)
{
	s_inited  = true;
	s_cfg     = c;
	s_harness = !c.record.empty() || !c.replay.empty() || !c.cksumLog.empty() || !c.cksumVerify.empty() || c.vsStage >= 0 || c.chStage >= 0;
	s_anyOn   = s_harness || c.haveSeed || c.headless || c.probe || c.haveExitAfter;
	if (!s_anyOn) return;

	// An output log on the path of an input log would destroy the input: --record over --replay,
	// --checksum-log over --checksum-verify, or two outputs on one file.
	{
		const std::string* outs[] = { &c.record, &c.cksumLog };
		const std::string* all[]  = { &c.record, &c.cksumLog, &c.replay, &c.cksumVerify };
		for (const std::string* o : outs) {
			for (const std::string* other : all) {
				if (o != other && same_path(*o, *other)) fail_init("an output log is on the path of another log", *o);
			}
		}
	}
	if (c.vsStage >= 0 && c.chStage >= 0) fail_init("--vs-stage and --ch-stage exclude each other", "--ch-stage");
	if (!c.replay.empty()) load_replay(c.replay);
	if (s_replay.loaded && c.haveSeed && c.seed != s_replay.seed) {
		std::fprintf(stderr, "mppm: net: warning: --seed %u ignored: the replay header's seed %u is used\n", c.seed, s_replay.seed);
	}
	// Output logs that cannot be written fail now (exit 2), not silently at the start barrier.
	// "a" creates a missing file and truncates nothing; the session opens it again with "w".
	for (const std::string* out : { &c.record, &c.cksumLog }) {
		if (out->empty()) continue;
		FILE* t = std::fopen(out->c_str(), "ab");
		if (!t) fail_init("cannot create output log", *out);
		std::fclose(t);
	}
	if (!c.cksumVerify.empty()) {
		FILE* t = std::fopen(c.cksumVerify.c_str(), "r");
		if (!t) fail_init("cannot open checksum verify log", c.cksumVerify);
		std::fclose(t);
	}
	if (s_replay.loaded && s_replay.startMode == 1 && s_cfg.vsStage >= 0 && s_cfg.vsStage != s_replay.vsStage) {
		std::fprintf(stderr, "mppm: net: warning: --vs-stage %d differs from the replay header's stage %u; using the header\n",
		             s_cfg.vsStage, (unsigned)s_replay.vsStage);
	}
	if (s_replay.loaded && s_replay.startMode == 2 && s_cfg.chStage >= 0 && s_cfg.chStage != s_replay.vsStage) {
		std::fprintf(stderr, "mppm: net: warning: --ch-stage %d differs from the replay header's course %u; using the header\n", s_cfg.chStage,
		             (unsigned)s_replay.vsStage);
	}
	if (s_replay.loaded && s_replay.version == kLogVersionV5 && (s_replay.handicap[0] || s_replay.handicap[1]) && std::getenv("MPPM_VS_HANDICAP")) {
		std::fprintf(stderr, "mppm: net: warning: MPPM_VS_HANDICAP differs from the replay header's handicaps; using the header\n");
	}
	if (s_replay.loaded && s_replay.startMode == 1 && s_cfg.vsPlayers > 0 && s_cfg.vsPlayers != s_replay.captains) {
		std::fprintf(stderr, "mppm: net: warning: --vs-players %d differs from the replay header's %d captains; using the header\n",
		             s_cfg.vsPlayers, s_replay.captains);
	}

	char seedBuf[32];
	if (s_replay.loaded) std::snprintf(seedBuf, sizeof(seedBuf), "%u(replay)", s_replay.seed);
	else if (c.haveSeed) std::snprintf(seedBuf, sizeof(seedBuf), "%u", c.seed);
	else std::snprintf(seedBuf, sizeof(seedBuf), "clock");
	auto s = [](const std::string& v) { return v.empty() ? "-" : v.c_str(); };
	char playersBuf[48] = ""; // M5.3a: only with more than 2 captains, so the 1-2 captain line is unchanged
	if (p2_net_vs_players() > kStockCaptains) std::snprintf(playersBuf, sizeof(playersBuf), ", players=%d", p2_net_vs_players());
	if (p2_net_ch_stage() >= 0) { // M5.3b: only for Challenge (same reason)
		const size_t used = std::strlen(playersBuf);
		std::snprintf(playersBuf + used, sizeof(playersBuf) - used, ", ch-stage=%d", p2_net_ch_stage());
	}
	std::fprintf(stderr, "mppm: net: %s (record=%s, replay=%s, checksum=%s, verify=%s, vs-stage=%d, seed=%s, headless=%d%s)\n",
	             s_harness ? "harness on" : "harness off, flags only", s(c.record), s(c.replay), s(c.cksumLog), s(c.cksumVerify), p2_net_vs_stage(), seedBuf, c.headless ? 1 : 0,
	             playersBuf);
	std::fflush(stderr);
}

// Every entry point starts here: until p2_net_init has run, read the environment only.
inline bool on()
{
	if (!s_inited) {
		Config c;
		read_env(c);
		do_init(c);
	}
	return s_anyOn;
}

bool known_seed(uint32_t* out)
{
	if (s_replay.loaded) {
		*out = s_replay.seed;
		return true;
	}
	if (s_cfg.haveSeed) {
		*out = s_cfg.seed;
		return true;
	}
	return false;
}

// ---- checksum log (s_mutex held) -------------------------------------------------------------

void verify_line(const char* line)
{
	if (!s_verifyFile || s_mismatch) return;
	char want[512];
	bool got = false;
	while (std::fgets(want, sizeof(want), s_verifyFile)) {
		if (want[0] == '#') continue;
		size_t len = std::strlen(want);
		while (len && (want[len - 1] == '\n' || want[len - 1] == '\r')) want[--len] = '\0';
		if (len == 0) continue;
		got = true;
		break;
	}
	if (got && std::strcmp(want, line) == 0) return;
	s_mismatch = true;
	std::fprintf(stderr, "mppm: net: CHECKSUM MISMATCH at tick %u\nmppm: net:   got:  %s\nmppm: net:   want: %s\n", s_tick, line,
	             got ? want : "(verify log ended)");
	std::fflush(stderr);
}

// A failed write leaves a short log: report it once, and the run exits 4 (kExitWriteError).
void note_write(bool ok, const char* what)
{
	if (ok || s_writeError) return;
	s_writeError = true;
	std::fprintf(stderr, "mppm: net: write error on the %s log (%s); the run will exit %d\n", what,
	             what[0] == 'r' ? s_cfg.record.c_str() : s_cfg.cksumLog.c_str(), kExitWriteError);
	std::fflush(stderr);
}

void emit_line(const char* line)
{
	if (s_cksumFile) {
		bool ok = std::fputs(line, s_cksumFile) >= 0;
		ok &= std::fputc('\n', s_cksumFile) != EOF;
		ok &= std::fflush(s_cksumFile) == 0; // a killed or hung run keeps every line up to its last one
		note_write(ok, "checksum");
	}
	verify_line(line);
}

void add_total(uint32_t total)
{
	uint8_t b[4];
	put_u32le(b, total);
	s_rolling   = crc_update(s_rolling, b, 4);
	s_lastTotal = total;
	++s_lineCount;
}

bool cksum_open() { return s_cksumFile || s_verifyOn; }

// M5.3b: online sessions compute the checksum lines whether or not a log is open.
bool cks_active() { return cksum_open() || mppm::session::started(); }

// M5.3b: keeps the last 64 lines and hands (tick, total, line) to the session (compared by the host).
void online_line(uint32_t tick, uint32_t total, const char* line)
{
	if (!mppm::session::started()) return;
	s_lines.emplace_back(line);
	while (s_lines.size() > 64) s_lines.pop_front();
	mppm::session::push_checksum(tick, total, line);
}

bool due_at(uint32_t tick) { return tick > 0 && tick % s_cfg.interval == 0; }

// L line for the tick that just ended, when it was due and got no C line.
void write_pending_l()
{
	if (!s_sessionActive || !cks_active()) return;
	const bool due = mppm::session::started() ? (s_tick > 0 && s_tick % kDefaultInterval == 0) : due_at(s_tick);
	if (!due || s_lastCTick == s_tick) return;
	uint32_t rng      = p2_msl_rand_state();
	uint32_t vals[2]  = { s_tick, rng };
	uint32_t total    = crc_u32s(vals, 2);
	add_total(total);
	char line[128];
	std::snprintf(line, sizeof(line), "L %u %08x rng=%08x", s_tick, total, rng);
	s_lastCTick = s_tick; // one line per tick
	emit_line(line);
	online_line(s_tick, total, line);
}

// Version 3: v2's 64 bytes, the mods hash at 64, the content hash at 96, the mod list from 128.
// M5.3a: with more than 2 captains, version 4 = the same plus byte 33 = captains; 1-2 captain
// recordings stay version 3 byte for byte (byte 33 zero).
// M5.3b: version 5 = version 4 plus start mode 2 (Challenge, course at 27) and the VS handicaps at 34/35,
// written only for a Challenge session or non-stock handicaps; flags bit 1 = recorded online.
void write_record_header(FILE* f, uint32_t tickCount)
{
	const int captains = p2_net_vs_players();
	const int course   = p2_net_ch_stage();
	const int hc0 = p2_net_vs_handicap(0), hc1 = p2_net_vs_handicap(1);
	const bool v5      = course >= 0 || hc0 != 0 || hc1 != 0;
	const std::string list = header_list();
	const uint32_t size    = kLogV3FixedSize + list_area_size(list.size());
	uint8_t h[kLogV3FixedSize + kLogV3ListMax] = {};
	put_u32le(h + 0, kLogMagic);
	put_u16le(h + 4, v5 ? kLogVersionV5 : (captains > kStockCaptains ? kLogVersionV4 : kLogVersion));
	put_u16le(h + 6, (uint16_t)size);
	put_u32le(h + 8, s_seed);
	put_u32le(h + 12, tickCount);
	put_u64le(h + 16, build_id());
	h[24]     = (uint8_t)kLogSlots;
	h[25]     = (uint8_t)kLogRecordSize;
	int stage = p2_net_vs_stage();
	h[26]     = stage >= 0 ? 1 : (course >= 0 ? 2 : 0);
	h[27]     = stage >= 0 ? (uint8_t)stage : (course >= 0 ? (uint8_t)course : 0xFF);
	put_u32le(h + 28, mppm::session::started() ? kOnlineFlag : 0);
	h[32] = (uint8_t)p2_net_tick_order();
	if (captains > kStockCaptains || v5) h[kCaptainsByte] = (uint8_t)captains;
	if (v5) {
		h[kHandicapByte]     = (uint8_t)hc0;
		h[kHandicapByte + 1] = (uint8_t)hc1;
	}
	float k = p2_net_sim_k(kDefaultSimK);
	uint32_t kBits;
	std::memcpy(&kBits, &k, 4);
	put_u32le(h + 36, kBits);
	put_u32le(h + 40, s_syncedHash);
	std::memcpy(h + 64, s_modsHash, kLogHashSize);
	p2_mods_content_hash(h + 96);
	std::memcpy(h + kLogV3FixedSize, list.data(), list.size());
	note_write(std::fwrite(h, 1, size, f) == size, "record");
}

void close_logs_locked()
{
	if (s_recFile) {
		bool ok = std::fflush(s_recFile) == 0;
		ok &= std::fclose(s_recFile) == 0;
		note_write(ok, "record");
		s_recFile = nullptr;
	}
	if (s_cksumFile) {
		bool ok = std::fflush(s_cksumFile) == 0;
		ok &= std::fclose(s_cksumFile) == 0;
		note_write(ok, "checksum");
		s_cksumFile = nullptr;
	}
	if (s_verifyFile) {
		std::fclose(s_verifyFile);
		s_verifyFile = nullptr;
	}
}

void encode_slot(uint8_t* p, const PADStatus& s, uint8_t event)
{
	put_u16le(p + 0, s.button);
	p[2]  = (uint8_t)s.stickX;
	p[3]  = (uint8_t)s.stickY;
	p[4]  = (uint8_t)s.substickX;
	p[5]  = (uint8_t)s.substickY;
	p[6]  = s.triggerLeft;
	p[7]  = s.triggerRight;
	p[8]  = s.analogA;
	p[9]  = s.analogB;
	p[10] = (uint8_t)s.err;
	p[11] = event; // port 0: the test event code; ports 1 to 3: 0
}

void decode_slot(const uint8_t* p, PADStatus& s)
{
	s.button       = get_u16le(p + 0);
	s.stickX       = (s8)p[2];
	s.stickY       = (s8)p[3];
	s.substickX    = (s8)p[4];
	s.substickY    = (s8)p[5];
	s.triggerLeft  = p[6];
	s.triggerRight = p[7];
	s.analogA      = p[8];
	s.analogB      = p[9];
	s.err          = (s8)p[10];
	s.extButton    = 0;
}

void release_slots(PADStatus* st)
{
	for (uint32_t p = 0; p < kLogSlots; ++p) {
		s8 err = st[p].err;
		std::memset(&st[p], 0, sizeof(PADStatus));
		st[p].err = err;
	}
}

void neutral_slots(PADStatus* st)
{
	// Keep each port's connection state from the last replayed tick, release everything else.
	const uint8_t* last = s_replay.count ? &s_replay.records[(size_t)(s_replay.count - 1) * kLogRecordSize] : nullptr;
	for (uint32_t p = 0; p < kLogSlots; ++p) {
		s8 err = last ? (s8)last[p * kLogSlotSize + 10] : st[p].err;
		std::memset(&st[p], 0, sizeof(PADStatus));
		st[p].err = err;
	}
}

// ---- M5.3a: MPPM_PERF tick timer --------------------------------------------------------------

bool perf_on()
{
	if (s_perfOn < 0) s_perfOn = env_true("MPPM_PERF") ? 1 : 0;
	return s_perfOn == 1;
}

double ms_between(PerfClock::time_point a, PerfClock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

// The summary of p2_net_shutdown: sim = BaseGameSection::doUpdate of a session tick; pass = from one
// tick's sim begin to the next one's (update, draw-command building and pacing), without passes that
// held a load barrier.
void print_perf()
{
	if (!perf_on()) return;
	std::vector<float> v = s_perfSim;
	double sum = 0.0;
	for (float x : v) sum += x;
	std::sort(v.begin(), v.end());
	auto pct = [&v](double p) -> double {
		if (v.empty()) return 0.0;
		size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
		return v[std::min(i, v.size() - 1)];
	};
	std::fprintf(stderr, "mppm: net: perf ticks=%zu sim avg=%.3f p50=%.3f p99=%.3f max=%.3f ms, pass avg=%.3f ms\n", v.size(),
	             v.empty() ? 0.0 : sum / (double)v.size(), pct(0.50), pct(0.99), v.empty() ? 0.0 : (double)v.back(),
	             s_perfPasses ? s_perfPassSum / (double)s_perfPasses : 0.0);
}

// ---- RNG self-test (MPPM_RNG_SELFTEST=1) -------------------------------------------------------

struct RngDraws {
	uint32_t sim[8], pres[8], other[8];
};

// Draws 8 values from each scope; SIM from the main thread, PRES inside a presentation scope,
// OTHER from a second thread.
void rng_draw_all(RngDraws& d)
{
	for (int i = 0; i < 8; ++i) d.sim[i] = (uint32_t)p2_msl_rand();
	p2_rng_pres_begin();
	for (int i = 0; i < 8; ++i) d.pres[i] = (uint32_t)p2_msl_rand();
	p2_rng_pres_end();
	std::thread t([&d] {
		for (int i = 0; i < 8; ++i) d.other[i] = (uint32_t)p2_msl_rand();
	});
	t.join();
}

[[noreturn]] void rng_selftest()
{
	int fails = 0;
	auto check = [&fails](bool ok, const char* what) {
		if (!ok) {
			++fails;
			std::fprintf(stderr, "mppm: net: rng selftest: FAILED: %s\n", what);
		}
	};
	const uint32_t seed = 20261122u;
	p2_rng_test_set_perturb(0, 0);
	p2_msl_srand(seed);
	p2_rng_session(1);
	check(p2_rng_state(P2_RNG_SIM) == seed, "srand sets SIM to the seed");
	check(p2_rng_state(P2_RNG_PRES) == p2_rng_mix(seed ^ 0x50524553u), "srand sets PRES");
	check(p2_rng_state(P2_RNG_OTHER) == p2_rng_mix(seed ^ 0x4F544852u), "srand sets OTHER");

	// The LCG itself: MSL's rand() from state 1 gives 16838, 5758, 10113.
	p2_rng_set_sim(1);
	check(p2_msl_rand() == 16838 && p2_msl_rand() == 5758 && p2_msl_rand() == 10113, "SIM is MSL's LCG");

	P2RngStats st0, st1;
	p2_rng_stats(&st0);
	uint32_t simBefore = p2_rng_state(P2_RNG_SIM);
	p2_rng_pres_begin();
	p2_rng_pres_begin(); // nests
	for (int i = 0; i < 5; ++i) p2_msl_rand();
	p2_rng_pres_end();
	for (int i = 0; i < 5; ++i) p2_msl_rand();
	p2_rng_pres_end();
	check(p2_rng_state(P2_RNG_SIM) == simBefore, "PRES draws do not move SIM");
	p2_rng_draw_begin();
	for (int i = 0; i < 3; ++i) p2_msl_rand();
	p2_rng_draw_end();
	p2_rng_draw_end(); // never below 0
	p2_rng_pres_end();
	check(p2_rng_state(P2_RNG_SIM) == simBefore, "draw-scope draws do not move SIM");
	std::thread([] {
		for (int i = 0; i < 4; ++i) p2_msl_rand();
	}).join();
	check(p2_rng_state(P2_RNG_SIM) == simBefore, "OTHER draws do not move SIM");
	p2_rng_stats(&st1);
	check(st1.pres - st0.pres == 10, "pres counter");
	check(st1.drawUnflagged - st0.drawUnflagged == 3, "draw-unflagged counter");
	check(st1.other - st0.other == 4 && st1.otherInSession - st0.otherInSession == 4, "other counters");
	p2_msl_rand();
	check(p2_rng_state(P2_RNG_SIM) != simBefore, "a main-thread draw outside every scope moves SIM");
	simBefore = p2_rng_state(P2_RNG_SIM);
	p2_rng_handoff(1);
	std::thread([] { p2_msl_rand(); }).join();
	p2_rng_handoff(0);
	check(p2_rng_state(P2_RNG_SIM) != simBefore, "a draw by another thread during a hand-off moves SIM");
	p2_rng_stats(&st0);
	check(st0.simHandoff - st1.simHandoff == 1, "handoff counter");
	simBefore = p2_rng_state(P2_RNG_SIM);
	std::thread([] { p2_msl_rand(); }).join();
	check(p2_rng_state(P2_RNG_SIM) == simBefore, "after the hand-off another thread uses OTHER again");

	RngDraws a, b, c;
	p2_msl_srand(seed);
	rng_draw_all(a);
	p2_msl_srand(seed);
	rng_draw_all(b);
	check(std::memcmp(&a, &b, sizeof(a)) == 0, "the same seed gives the same three sequences");
	check(std::memcmp(a.sim, a.pres, sizeof(a.sim)) != 0 && std::memcmp(a.sim, a.other, sizeof(a.sim)) != 0 &&
	          std::memcmp(a.pres, a.other, sizeof(a.sim)) != 0,
	      "the three streams differ");
	p2_rng_test_set_perturb(305419896u, 2596069104u);
	p2_msl_srand(seed);
	rng_draw_all(c);
	check(std::memcmp(a.sim, c.sim, sizeof(a.sim)) == 0, "perturb leaves SIM alone");
	check(std::memcmp(a.pres, c.pres, sizeof(a.pres)) != 0, "perturb changes PRES");
	check(std::memcmp(a.other, c.other, sizeof(a.other)) != 0, "perturb changes OTHER");
	p2_rng_test_set_perturb(0, 0);
	p2_msl_srand(seed);
	rng_draw_all(c);
	check(std::memcmp(&a, &c, sizeof(a)) == 0, "perturb 0 restores the sequences");

	std::fprintf(stderr, "mppm: net: rng selftest %s\n", fails ? "FAIL" : "PASS");
	std::fflush(nullptr);
	std::exit(fails ? 1 : 0);
}

} // namespace

// ---- interface -------------------------------------------------------------------------------

extern "C" void p2_net_init(int argc, char** argv)
{
	if (env_true("MPPM_RNG_SELFTEST")) rng_selftest();
	if (s_inited) return; // a lazy init already ran: the environment decided
	Config c;
	read_env(c);
	read_args(c, argc, argv);
	do_init(c);
	// M5.3b: the online flags. An online command-line run (--host, --join, --net-test-bots) turns the
	// seam on (its session starts at the VS or Challenge inject after START) and starts the net thread.
	mppm::session::cli_init(argc, argv, !c.replay.empty() || c.headless, c.vsStage, c.chStage);
	if (mppm::session::cli_online()) s_anyOn = true;
	if (c.haveExitAfter) mppm::session::set_exit_tick(c.exitAfter);
	uint32_t n = 0;
	if (parse_u32(std::getenv("MPPM_NET_TEST_DESYNC"), &n)) s_testDesync = n;
	if (parse_u32(std::getenv("MPPM_NET_TEST_LEAVE_AT"), &n)) s_testLeave = n;
	if (const char* v = std::getenv("MPPM_NET_TEST_HANG_AT"); v && *v) {
		char* end           = nullptr;
		const unsigned long t = std::strtoul(v, &end, 10);
		const unsigned long ms = (end && *end == ':') ? std::strtoul(end + 1, nullptr, 10) : 120000;
		s_testHang   = (uint32_t)t;
		s_testHangMs = (uint32_t)std::min<unsigned long>(ms, 600000);
	}
	if (const char* v = std::getenv("MPPM_NET_TEST_SLEW_NS"); v && *v) {
		s_testSlew = std::strtoll(v, nullptr, 10);
		if (s_testSlew) s_anyOn = true;
	}
}

extern "C" int p2_net_active(void)
{
	if (s_online.load(std::memory_order_relaxed)) return 1;
	if (!on()) return 0;
	return s_harness ? 1 : 0;
}

extern "C" int p2_net_session_active(void)
{
	if (!on()) return 0;
	return (s_sessionActive && !s_crashed.load()) ? 1 : 0;
}

extern "C" uint32_t p2_net_tick(void)
{
	if (!on()) return 0;
	return s_tick;
}

extern "C" int p2_net_headless(void)
{
	if (!on()) return 0;
	return s_cfg.headless ? 1 : 0;
}

extern "C" int p2_net_latency_probe(void)
{
	if (!on()) return 0;
	return s_cfg.probe ? 1 : 0;
}

extern "C" int p2_net_vs_stage(void)
{
	if (!on()) return -1;
	if (mppm::session::started()) { // M5.3b: the session's stage for the Battle modes
		const auto& c = mppm::session::config();
		return c.mode == mppm::session::Mode::Challenge ? -1 : (int)c.stage;
	}
	if (s_replay.loaded && s_replay.startMode == 1) return s_replay.vsStage == 0xFF ? -1 : (int)s_replay.vsStage;
	if (s_replay.loaded && s_replay.startMode == 2) return -1;
	return s_cfg.vsStage;
}

extern "C" int p2_net_pad_wallclock_off(void) { return p2_net_active(); }

extern "C" unsigned p2_net_boot_seed(unsigned clockSeed)
{
	if (!on()) return clockSeed;
	if (!s_haveClockSeed) {
		s_haveClockSeed = true;
		s_clockSeed     = clockSeed;
	}
	uint32_t seed;
	return known_seed(&seed) ? seed : clockSeed;
}

// M5.3b: an online session always records (D24): --record, else <pref>/net/sessions/<code>-<time>.mpnl,
// keeping the 5 newest there.
static std::string online_record_path()
{
	if (!s_cfg.record.empty()) return s_cfg.record;
	if (mppm::data_dir_utf8().empty()) return {};
	std::string dir = mppm::data_dir_utf8() + "net/sessions/";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::vector<std::filesystem::path> old;
	for (const auto& e : std::filesystem::directory_iterator(dir, ec))
		if (e.path().extension() == ".mpnl") old.push_back(e.path());
	// Oldest first by the "YYYYmmdd-HHMMSS" that ends every name (the code in front is random).
	const auto when_of = [](const std::filesystem::path& p) {
		const std::string s = p.stem().string();
		return s.size() >= 15 ? s.substr(s.size() - 15) : s;
	};
	std::sort(old.begin(), old.end(), [&](const auto& a, const auto& b) {
		const std::string wa = when_of(a), wb = when_of(b);
		return wa != wb ? wa < wb : a < b;
	});
	while (old.size() >= 5) {
		std::filesystem::remove(old.front(), ec);
		old.erase(old.begin());
	}
	char when[32];
	const std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_r(&t, &tmv);
	std::strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tmv);
	return dir + mppm::session::code() + "-" + when + ".mpnl";
}

extern "C" void p2_net_session_begin(void)
{
	const bool onl = mppm::session::started();
	if (!on() || (!s_harness && !onl)) return;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_sessionActive || s_sessionEnded || s_crashed.load()) return;

	if (onl) s_seed = mppm::session::config().seed;
	else if (!known_seed(&s_seed)) s_seed = s_haveClockSeed ? s_clockSeed : (uint32_t)std::time(nullptr);
	p2_msl_srand(s_seed);
	p2_rng_session(1);
	s_tick          = 0;
	s_curEvent      = 0;
	s_readsAfterEnd = 0;
	s_lastCTick     = kTickCountOpen;
	s_rolling       = 0xFFFFFFFFu;
	s_lineCount     = 0;
	s_lastTotal     = 0;

	s_recPath = onl ? online_record_path() : s_cfg.record;
	if (!s_recPath.empty()) {
		s_recFile = std::fopen(s_recPath.c_str(), "wb");
		if (!s_recFile) {
			std::fprintf(stderr, "mppm: net: cannot create record log %s\n", s_recPath.c_str());
			note_write(false, "record");
		} else {
			write_record_header(s_recFile, kTickCountOpen);
		}
		s_recCount = 0;
	}
	if (!s_cfg.cksumLog.empty()) {
		s_cksumFile = std::fopen(s_cfg.cksumLog.c_str(), "w");
		if (!s_cksumFile) {
			std::fprintf(stderr, "mppm: net: cannot create checksum log %s\n", s_cfg.cksumLog.c_str());
			note_write(false, "checksum");
		} else {
			note_write(std::fprintf(s_cksumFile, "# mppm-cksum v2 seed=%u interval=%u\n# build=%016" PRIx64 "\n", s_seed,
			                        s_cfg.interval, build_id()) > 0,
			           "checksum");
			if (!s_contentStock) { // M5.2: only for modded content, so stock logs stay byte-identical
				char hex[2 * kLogHashSize + 1];
				hex_of(s_modsHash, kLogHashSize, hex);
				note_write(std::fprintf(s_cksumFile, "# mods=%s %s\n", hex, printable_list(s_modsList).c_str()) > 0, "checksum");
			}
			if (p2_net_vs_players() > kStockCaptains) { // M5.3a: only with more than 2 captains (same reason)
				note_write(std::fprintf(s_cksumFile, "# players=%d\n", p2_net_vs_players()) > 0, "checksum");
			}
		}
	}
	if (!s_cfg.cksumVerify.empty()) {
		s_verifyFile = std::fopen(s_cfg.cksumVerify.c_str(), "r");
		if (!s_verifyFile) {
			std::fprintf(stderr, "mppm: net: cannot open checksum verify log %s\n", s_cfg.cksumVerify.c_str());
			s_mismatch = true; // a requested verify that cannot run is a failure
		} else {
			s_verifyOn = true;
		}
	}
	s_sessionActive = true;
	char playersBuf[96] = "";
	if (p2_net_vs_players() > kStockCaptains) std::snprintf(playersBuf, sizeof(playersBuf), " players=%d", p2_net_vs_players());
	if (onl) { // M5.3b: only online, so harness lines stay as they were
		const size_t used = std::strlen(playersBuf);
		std::snprintf(playersBuf + used, sizeof(playersBuf) - used, " online=%s seat=%d record=%s", mppm::session::code().c_str(),
		              mppm::session::local_seat(), s_recPath.empty() ? "-" : s_recPath.c_str());
	}
	std::fprintf(stderr, "mppm: net: session start seed=%u start=%s order=%s sim-k=%.2f%s\n", s_seed,
	             p2_net_vs_stage() >= 0 ? "vs" : (p2_net_ch_stage() >= 0 ? "challenge" : "boot"),
	             p2_net_tick_order() == P2_TICK_ORDER_UPDATE_FIRST ? "update" : "draw", (double)p2_net_sim_k(kDefaultSimK), playersBuf);
	if (s_replay.loaded && s_replay.syncedHash != 0 && s_replay.syncedHash != s_syncedHash) {
		std::fprintf(stderr, "mppm: net: warning: the replay was recorded with other Synced settings (hash %08x, this run %08x)\n",
		             s_replay.syncedHash, s_syncedHash);
	}
	std::fflush(stderr);
	if (onl) {
		mppm::session::session_began();
		// The start barrier (D10): Battle and Challenge load their stage inside sim ticks (LoadState with
		// synchronous DVD commands), so no nonsim region follows the inject; the inject itself is the
		// first barrier, and tick 1 runs at the host's release time on every peer.
		mppm::session::push_loaded(0);
	}
}

// ---- M5.2: build id and content ------------------------------------------------------------

extern "C" uint64_t p2_net_build_id(void) { return build_id(); }

extern "C" void p2_net_set_content(const uint8_t modsHash[32], const char* list)
{
	static const uint8_t kZero[kLogHashSize] = {};
	std::memcpy(s_modsHash, modsHash ? modsHash : kZero, kLogHashSize);
	s_modsList     = list ? list : "";
	s_contentStock = all_zero(s_modsHash, kLogHashSize);
	if (!on()) return;

	char runHex[2 * kLogHashSize + 1];
	hex_of(s_modsHash, kLogHashSize, runHex);
	const std::string runList = printable_list(s_modsList);
	if (s_replay.loaded && !(s_replay.flags & 1u) && std::memcmp(s_replay.modsHash, s_modsHash, kLogHashSize) != 0) {
		char repHex[2 * kLogHashSize + 1];
		hex_of(s_replay.modsHash, kLogHashSize, repHex);
		std::fprintf(stderr,
		             "mppm: net: the replay was recorded with other content: replay has %s (mods %.8s), this run has %s (mods %.8s); "
		             "turn on the same mods in the same order\n",
		             printable_list(s_replay.modsList).c_str(), repHex, runList.c_str(), runHex);
		std::fflush(stderr);
		std::fflush(nullptr);
		std::_Exit(2); // Aurora is up (its threads would abort in static destructors); no log is open yet
	}
	if (s_contentStock) return;
	if (s_harness) {
		size_t n = 1;
		for (char ch : s_modsList)
			if (ch == ',') ++n;
		std::fprintf(stderr, "mppm: net: content: %zu mods %.8s (%s)\n", s_modsList.empty() ? (size_t)0 : n, runHex, runList.c_str());
	}
	if (s_replay.loaded && (s_replay.flags & 1u)) {
		std::fprintf(stderr, "mppm: net: synthetic replay: content not checked (this run: %s)\n", runList.c_str());
	}
	std::fflush(stderr);
}

extern "C" int p2_net_should_exit(void)
{
	if (!on()) return 0;
	if (s_mismatch) return 1;
	if (mppm::session::command_line_run()) {
		// M5.3b: a command-line online run ends with its session (desync, host lost, lobby failure,
		// a test leave) or after --exit-after-ticks (the host then lingers in p2_net_shutdown).
		if (mppm::session::ended() || s_leaving) return 1;
		if (s_cfg.haveExitAfter && s_sessionActive && s_tick >= s_cfg.exitAfter) return 1;
		return 0;
	}
	if (s_harness) {
		if (s_cfg.haveExitAfter && s_sessionActive && s_tick >= s_cfg.exitAfter) return 1;
		if (s_replay.loaded && s_sessionActive && s_tick >= s_replay.count) return 1;
	} else if (s_cfg.haveExitAfter && s_reads >= s_cfg.exitAfter) {
		return 1;
	}
	return 0;
}

extern "C" int p2_net_exit_code(void)
{
	if (!on()) return 0;
	if (mppm::session::command_line_run() && mppm::session::exit_code() != 0) return mppm::session::exit_code(); // M5.3b (D30)
	if (s_mismatch) return kExitMismatch;
	if (s_writeError) return kExitWriteError;
	if (env_true("MPPM_RNG_STRICT")) {
		P2RngStats st;
		p2_rng_stats(&st);
		if (st.otherInSession > 0) return kExitRngStrict;
	}
	return 0;
}

static void p2_net_shutdown_locked(void);

extern "C" void p2_net_shutdown(void)
{
	if (!on()) return;
	const bool exitReached = s_cfg.haveExitAfter && s_sessionActive && s_tick >= s_cfg.exitAfter;
	const uint32_t tick    = s_tick;
	p2_net_shutdown_locked();
	// M5.3b: the host lingers until every peer acknowledged the last tick, BYE, the net thread stops,
	// the end and summary lines. Idempotent.
	static bool s_sessionShut = false;
	if (!s_sessionShut && mppm::session::exists()) {
		s_sessionShut = true;
		mppm::session::shutdown(exitReached, tick);
	}
}

static void p2_net_shutdown_locked(void)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_sessionActive) {
		if (!s_crashed.load()) close_logs_locked();
		return;
	}
	if (!s_crashed.load()) {
		write_pending_l();
		if (cksum_open()) {
			uint32_t rolling = s_rolling ^ 0xFFFFFFFFu;
			char line[128];
			std::snprintf(line, sizeof(line), "E %u %08x lines=%u", s_tick, rolling, s_lineCount);
			emit_line(line);
		}
		if (s_recFile) {
			std::fflush(s_recFile);
			bool ok = std::fseek(s_recFile, 12, SEEK_SET) == 0;
			if (ok) {
				uint8_t b[4];
				put_u32le(b, s_recCount);
				ok = std::fwrite(b, 1, 4, s_recFile) == 4;
			}
			note_write(ok, "record");
		}
		close_logs_locked();
	}
	s_sessionActive = false;
	s_sessionEnded  = true;
	std::fprintf(stderr, "mppm: net: session end ticks=%u lines=%u final=%08x\n", s_tick, s_lineCount, s_rolling ^ 0xFFFFFFFFu);
	print_perf();
	p2_rng_session(0);
	{
		P2RngStats st;
		p2_rng_stats(&st);
		std::fprintf(stderr, "mppm: net: rng sim=%" PRIu64 " handoff=%" PRIu64 " pres=%" PRIu64 " draw-unflagged=%" PRIu64 " other=%" PRIu64
		             " other-in-session=%" PRIu64 "\n",
		             st.sim, st.simHandoff, st.pres, st.drawUnflagged, st.other, st.otherInSession);
	}
	std::fflush(stderr);
}

extern "C" void p2_net_crash_stop(void)
{
	// An online session says BYE at once (its net thread still runs): the others drop this seat (or,
	// for a host, the session ends) instead of waiting on a game that cannot go on.
	const bool leaving = mppm::session::crash_leave();
	if (!on()) return;
	if (s_crashed.exchange(true)) return;
	// The crash may have happened inside the seam on another thread holding the lock: then only
	// the flag is set and the logs stay as they are (the stdio buffers are lost).
	if (s_mutex.try_lock()) {
		close_logs_locked();
		s_mutex.unlock();
	}
	std::fprintf(stderr, "mppm: net: crash screen: harness stopped at tick %u\n", s_tick);
	std::fflush(stderr);
	if (s_cfg.headless) {
		// Nobody can read the crash screen of a headless run, and the seam no longer feeds input:
		// end the process instead of waiting for an outer timeout.
		std::fprintf(stderr, "mppm: net: headless run: exiting %d\n", kExitCrash);
		std::fflush(stderr); // not fflush(nullptr): the crashed thread may hold another stream's lock
		if (leaving) std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the BYE goes out first
		std::_Exit(kExitCrash);
	}
}

extern "C" int p2_net_checksum_due(void)
{
	if (!on()) return 0;
	if (!s_sessionActive || s_crashed.load()) return 0;
	if (mppm::session::started()) return (s_tick > 0 && s_tick % kDefaultInterval == 0) ? 1 : 0; // M5.3b: always online, every 30
	if (!cksum_open()) return 0;
	return due_at(s_tick) ? 1 : 0;
}

extern "C" void p2_net_checksum_submit(const P2NetChecksum* sum)
{
	if (!on() || !sum) return;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_sessionActive || s_crashed.load() || !cks_active()) return;
	if (s_lastCTick == s_tick) return; // one line per tick
	uint32_t vals[9] = { s_tick, sum->rng, sum->navi, sum->piki, sum->enemy, sum->pellet, sum->timers, sum->mode, sum->camera };
	uint32_t total   = crc_u32s(vals, 9);
	add_total(total);
	char line[256];
	std::snprintf(line, sizeof(line), "C %u %08x rng=%08x navi=%08x piki=%08x enemy=%08x pellet=%08x time=%08x mode=%08x cam=%08x n=%u/%u/%u/%u",
	              s_tick, total, sum->rng, sum->navi, sum->piki, sum->enemy, sum->pellet, sum->timers, sum->mode, sum->camera,
	              (unsigned)sum->naviCount, (unsigned)sum->pikiCount, (unsigned)sum->enemyCount, (unsigned)sum->pelletCount);
	s_lastCTick = s_tick;
	emit_line(line);
	online_line(s_tick, total, line);
}

// ---- M5.3b: online input seam -------------------------------------------------------------------

static int online_ready_cb(void* arg)
{
	const uint32_t t = *static_cast<uint32_t*>(arg);
	if (mppm::session::ready(t)) return 1;
	// A command-line run ends when its session does; the UI keeps the picture until "Back to start menu".
	return (mppm::session::over(t) && mppm::session::command_line_run()) ? 1 : 0;
}

static void online_neutral(PADStatus* st)
{
	for (uint32_t p = 0; p < kLogSlots; ++p) {
		std::memset(&st[p], 0, sizeof(PADStatus));
		st[p].err = PAD_ERR_NO_CONTROLLER;
	}
}

static void online_exchange(PADStatus* st)
{
	using namespace mppm::session;
	const uint32_t t = s_tick + 1;
	if (s_leaving || ended()) {
		online_neutral(st);
		s_curEvent = 0;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		write_pending_l();
	}
	// Local input: physical port 0 (or the --net-input record of this seat), tagged t + d by the net thread.
	const int seat = local_seat();
	Input in;
	put_u16le(in.b, st[0].button);
	in.b[2] = (uint8_t)st[0].stickX;
	in.b[3] = (uint8_t)st[0].stickY;
	in.b[4] = (uint8_t)st[0].substickX;
	in.b[5] = (uint8_t)st[0].substickY;
	in.b[6] = st[0].triggerLeft;
	in.b[7] = st[0].triggerRight;
	const long long due   = p2_host_next_tick_due_ns();
	const bool barrier    = barrier_pending();
	push_read(t, due, p2_vi_phase_slew_get(), seat >= 0, in);
	int64_t waited        = 0; // past the tick's due time, waiting for readiness (a read that is late but finds its tick ready is no stall)
	if (!ready(t)) {
		// Fallback hold (D14): the first read after a load barrier, or a missed late-sampling wait.
		uint32_t arg        = t;
		const int64_t since = (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		wait_state(true, t, since, barrier);
		p2_host_net_hold(online_ready_cb, &arg);
		wait_state(false, t, since, barrier);
		if (!ready(t)) { // the session ended, or the window asked to quit
			online_neutral(st);
			s_curEvent = 0;
			return;
		}
		const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		waited            = (due > 0 && now > due) ? now - std::max<int64_t>(due, since) : 0;
	}
	note_read(t, waited, barrier);
	Input b[kMaxSeats];
	uint8_t mask = 0;
	take(t, b, mask);
	for (uint32_t p = 0; p < kLogSlots; ++p) {
		std::memset(&st[p], 0, sizeof(PADStatus));
		if (mask & (1u << p)) {
			st[p].button       = get_u16le(b[p].b);
			st[p].stickX       = (s8)b[p].b[2];
			st[p].stickY       = (s8)b[p].b[3];
			st[p].substickX    = (s8)b[p].b[4];
			st[p].substickY    = (s8)b[p].b[5];
			st[p].triggerLeft  = b[p].b[6];
			st[p].triggerRight = b[p].b[7];
			st[p].err          = PAD_ERR_NONE;
		} else {
			st[p].err = PAD_ERR_NO_CONTROLLER;
		}
	}
	s_curEvent = 0;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_recFile) {
			uint8_t rec[kLogRecordSize];
			for (uint32_t p = 0; p < kLogSlots; ++p) encode_slot(rec + p * kLogSlotSize, st[p], 0);
			bool ok = std::fwrite(rec, 1, sizeof(rec), s_recFile) == sizeof(rec);
			ok &= std::fflush(s_recFile) == 0;
			note_write(ok, "record");
			++s_recCount;
		}
	}
	if (s_testDesync && t == s_testDesync) { // D23: this peer's next checksum differs
		p2_rng_set_sim(p2_rng_state(P2_RNG_SIM) ^ 1u);
		std::fprintf(stderr, "mppm: net: test: SIM RNG perturbed at tick %u\n", t);
	}
	++s_tick;
	reached_tick(s_tick);
	if (s_testLeave && s_tick >= s_testLeave && !s_leaving) {
		s_leaving = true;
		leave_now(1); // wire::kByeLeft
		std::fprintf(stderr, "mppm: net: test: leaving after tick %u\n", s_tick);
	}
	if (s_testHang && s_tick == s_testHang) {
		// A stuck game whose net thread runs on (fix round 2): the host must drop this seat by progress.
		std::fprintf(stderr, "mppm: net: test: game thread hangs for %u ms after tick %u\n", s_testHangMs, s_tick);
		std::fflush(stderr);
		std::this_thread::sleep_for(std::chrono::milliseconds(s_testHangMs));
		std::fprintf(stderr, "mppm: net: test: game thread goes on\n");
	}
}

// MPPM_NET_TEST_SLEW_NS (harness and online runs): one slew of the retrace grid at the first session
// tick, and the measured shift of the grid 12 retraces (0.2 s) later.
static void slew_test_tick()
{
	if (!s_testSlew) return;
	if (s_tick == 1 && !s_slewPending && s_slewSetNs == 0) {
		s_slewCount   = VIGetRetraceCount() + 12;
		s_slewPred    = p2_vi_retrace_time_ns(s_slewCount);
		p2_vi_phase_slew_set(s_testSlew);
		s_slewPending = true;
		s_slewSetNs   = 1;
		std::fprintf(stderr, "mppm: net: test slew set ns=%lld at tick 1\n", (long long)s_testSlew);
		return;
	}
	if (s_slewPending && (int32_t)(VIGetRetraceCount() - s_slewCount) > 0) {
		s_slewPending = false;
		const long long actual = p2_vi_retrace_time_ns(s_slewCount);
		std::fprintf(stderr, "mppm: net: test slew shift_us=%lld after_retraces=12 remaining_us=%lld\n", (actual - s_slewPred) / 1000,
		             p2_vi_phase_slew_get() / 1000);
	}
}

extern "C" void p2_net_exchange(struct PADStatus* st)
{
	if (!on() || s_crashed.load()) return;
	++s_reads;
	const bool onl = mppm::session::started();
	if (!s_harness && !onl) return;

	if (!s_sessionActive) {
		if (s_sessionEnded) return;
		if (onl || mppm::session::cli_online() || p2_net_vs_stage() >= 0 || p2_net_ch_stage() >= 0) {
			// Before the VS start barrier the boot needs no input. Release every button and stick so
			// the button state carried into tick 0 (JUTGamePad's previous-read edge and repeat state)
			// is the same in a recording and in its replay. The connection state (err) stays live.
			release_slots(st);
			return;
		}
		p2_net_session_begin();
		if (!s_sessionActive) return;
	}

	if (s_nonsimDepth > 0) {
		// A load-barrier read (plan 2.3 step 5): not a tick. Released pads, no record consumed or
		// written, the tick does not advance; the number of these reads may differ per machine.
		release_slots(st);
		s_curEvent = 0;
		return;
	}
	if (onl) {
		online_exchange(st);
		slew_test_tick();
		return;
	}

	if (p2_net_should_exit()) {
		// The run is over but the host has not quit (no host package): hold the input, then leave.
		if (s_replay.loaded) neutral_slots(st);
		if (++s_readsAfterEnd >= kExitReadsAfterEnd) {
			p2_net_shutdown();
			std::fflush(nullptr);
			std::_Exit(p2_net_exit_code());
		}
		return;
	}

	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_crashed.load()) return;
	write_pending_l();

	if (s_replay.loaded && s_tick < s_replay.count) {
		const uint8_t* rec = &s_replay.records[(size_t)s_tick * kLogRecordSize];
		for (uint32_t p = 0; p < kLogSlots; ++p) decode_slot(rec + p * kLogSlotSize, st[p]);
		s_curEvent = rec[kEventSlotByte];
	} else {
		s_curEvent = 0;
	}
	if (s_recFile) {
		uint8_t rec[kLogRecordSize];
		for (uint32_t p = 0; p < kLogSlots; ++p) encode_slot(rec + p * kLogSlotSize, st[p], p == 0 ? s_curEvent : 0);
		bool ok = std::fwrite(rec, 1, sizeof(rec), s_recFile) == sizeof(rec);
		// Every tick: a panic's abort() drops the stdio buffer, and the log must reach the tick
		// that crashed.
		ok &= std::fflush(s_recFile) == 0;
		note_write(ok, "record");
		++s_recCount;
	}
	++s_tick;
	if (s_testSlew) slew_test_tick();
}

extern "C" void p2_net_abort_run(const char* why)
{
	if (!on()) return;
	std::fprintf(stderr, "mppm: net: %s: the harness run cannot start; exiting 2\n", why ? why : "error");
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		close_logs_locked();
	}
	std::fflush(stderr);
	std::_Exit(2);
}


// ---- M5.1b ------------------------------------------------------------------------------------

extern "C" void p2_net_set_online(int on) { s_online.store(on ? 1 : 0, std::memory_order_relaxed); }

extern "C" int p2_net_tick_order(void)
{
	(void)on(); // reads the environment when p2_net_init has not run
	if (mppm::session::started()) return mppm::session::config().tickOrder; // M5.3b: the session constant
	const int given = s_cfg.tickOrder;
	if (s_replay.loaded) {
		if (given >= 0 && given != s_replay.tickOrder && !s_orderNoticed) {
			s_orderNoticed = true;
			auto name      = [](int o) { return o == P2_TICK_ORDER_UPDATE_FIRST ? "update" : "draw"; };
			std::fprintf(stderr, "mppm: net: tick order %s overrides the replay header's %s\n", name(given), name(s_replay.tickOrder));
			std::fflush(stderr);
		}
		return given >= 0 ? given : s_replay.tickOrder;
	}
	return given >= 0 ? given : P2_TICK_ORDER_UPDATE_FIRST;
}

extern "C" float p2_net_sim_k(float windowK)
{
	if (!p2_net_active()) return windowK;
	if (mppm::session::started()) return mppm::session::config().simK; // M5.3b: the session constant
	if (s_cfg.simK > 0.0f) return s_cfg.simK;
	if (s_replay.loaded && s_replay.simK > 0.0f) return s_replay.simK;
	return kDefaultSimK;
}

extern "C" void p2_net_set_synced_hash(uint32_t hash) { s_syncedHash = hash; }

extern "C" void p2_net_nonsim_enter(void)
{
	++s_nonsimDepth;
	s_perfGap = true; // M5.3a: a load barrier inside a pass is not a pass time
}

extern "C" void p2_net_nonsim_leave(void)
{
	if (s_nonsimDepth > 0) {
		--s_nonsimDepth;
		// M5.3b load barrier (D10): the outermost leave in an online session reports "loaded at tick B";
		// tick B + 1 waits for the host's release.
		if (s_nonsimDepth == 0 && s_sessionActive && !s_crashed.load() && mppm::session::started()) mppm::session::push_loaded(s_tick);
		return;
	}
	if (!s_nonsimWarned) {
		s_nonsimWarned = true;
		std::fprintf(stderr, "mppm: net: warning: p2_net_nonsim_leave without a matching enter; ignored\n");
		std::fflush(stderr);
	}
}

extern "C" int p2_net_nonsim(void) { return s_nonsimDepth; }

extern "C" void p2_net_floor_begin(uint32_t index)
{
	if (!on() || index == 0) return;
	if (!s_sessionActive || s_crashed.load()) return;
	p2_rng_set_sim(p2_rng_mix(s_seed + 0x9E3779B9u * index));
	std::fprintf(stderr, "mppm: net: floor %u reseed\n", index);
	std::fflush(stderr);
}

extern "C" int p2_net_debug_event(void)
{
	if (!on()) return 0;
	if (!s_sessionActive || s_crashed.load() || !s_replay.loaded || s_nonsimDepth > 0) return 0;
	return s_curEvent;
}

// ---- M5.3a ------------------------------------------------------------------------------------

extern "C" int p2_net_vs_players(void)
{
	if (mppm::session::started()) { // M5.3b: the session's seats (Battle 2, Battle 2 vs 2 4, Challenge 2)
		const auto& c = mppm::session::config();
		return c.mode == mppm::session::Mode::Challenge ? kStockCaptains : (int)c.seats;
	}
	if (!on() || p2_net_vs_stage() < 0) return kStockCaptains;
	if (s_replay.loaded) return s_replay.captains; // the header wins (do_init warned about a different flag)
	return s_cfg.vsPlayers > 0 ? s_cfg.vsPlayers : kStockCaptains;
}

extern "C" int p2_net_view_captain(void)
{
	(void)on(); // reads the environment when p2_net_init has not run
	// M5.3b (D16): with 4 seats every peer views its own captain (spectators captain 0) unless
	// --view-captain says otherwise; 2 seats keep the stock split.
	if (mppm::session::started() && p2_net_vs_players() <= kStockCaptains) {
		// 2 seats: the stock LOD reads the presented viewports, so a per-peer view would change the
		// simulation on this peer only (D16). --view-captain is ignored online.
		static bool said = false;
		if (s_cfg.viewCaptain >= 0 && !said) {
			said = true;
			std::fprintf(stderr, "mppm: net: --view-captain is ignored in a 2-seat online session (stock split)\n");
		}
		return -1;
	}
	if (s_cfg.viewCaptain < 0 && mppm::session::started()) {
		const int seat = mppm::session::local_seat();
		return seat >= 0 ? seat : 0;
	}
	return s_cfg.viewCaptain;
}

// ---- M5.3b ------------------------------------------------------------------------------------

extern "C" int p2_net_online(void) { return mppm::session::started() && !mppm::session::ended() ? 1 : 0; }

extern "C" int p2_net_local_slot(void) { return mppm::session::started() ? mppm::session::local_seat() : -1; }

extern "C" int p2_net_ch_stage(void)
{
	if (mppm::session::started()) {
		const auto& c = mppm::session::config();
		return c.mode == mppm::session::Mode::Challenge ? (int)c.stage : -1;
	}
	if (!on()) return -1;
	if (s_replay.loaded && s_replay.startMode == 2) return (int)s_replay.vsStage;
	if (s_replay.loaded && s_replay.startMode == 1) return -1;
	return s_cfg.chStage;
}

extern "C" int p2_net_vs_handicap(int team)
{
	if (team < 0 || team > 1) return 0;
	if (mppm::session::started()) return mppm::session::config().handicap[team];
	if (!on()) return 0;
	if (s_replay.loaded && s_replay.version == kLogVersionV5) return s_replay.handicap[team];
	return 0;
}

extern "C" int p2_net_tick_ready(uint32_t tick)
{
	if (!mppm::session::started()) return 1;
	return mppm::session::ready(tick) ? 1 : 0;
}

extern "C" void p2_net_game_poll(void) { mppm::session::game_poll(); }

namespace mppm::session::seam {
void online_on()
{
	s_inited = true;
	s_anyOn  = true;
}
void online_off() { s_online.store(0, std::memory_order_relaxed); }
std::vector<std::string> last_lines() { return std::vector<std::string>(s_lines.begin(), s_lines.end()); }
std::string record_path() { return s_recPath; }
uint32_t seed() { return s_cfg.haveSeed ? s_cfg.seed : 0; }
} // namespace mppm::session::seam

extern "C" void p2_net_perf_sim_begin(void)
{
	if (!perf_on() || !s_sessionActive || s_crashed.load()) return;
	const PerfClock::time_point now = PerfClock::now();
	if (s_perfHavePrev && !s_perfGap) {
		s_perfPassSum += ms_between(s_perfPrev, now);
		++s_perfPasses;
	}
	s_perfPrev     = now;
	s_perfHavePrev = true;
	s_perfGap      = false;
	s_perfBegin    = now;
	s_perfInTick   = true;
}

extern "C" void p2_net_perf_sim_end(void)
{
	if (!s_perfInTick) return;
	s_perfInTick = false;
	if (!s_sessionActive || s_crashed.load()) return;
	s_perfSim.push_back((float)ms_between(s_perfBegin, PerfClock::now()));
}
