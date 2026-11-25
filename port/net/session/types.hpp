// M5.3b session types shared by the wire format, the pure host and client cores, the game link and
// the net thread (docs/online/M5_3B_LOCKSTEP.md). No game headers, no Aurora, no SDL.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace mppm::session {

constexpr int kMaxSeats          = 4;
constexpr uint32_t kNoTick       = 0xFFFFFFFFu;
constexpr int64_t kRetraceNs     = 1000000000LL * 1001 / 60000;   // 16.683 ms (NTSC 59.94 Hz)
constexpr int64_t kTickNs        = 2 * kRetraceNs;                // P = 33.367 ms
constexpr int64_t kPhaseMaxNs    = 16 * kTickNs;                  // phase errors beyond this are no samples
constexpr int kMaxDelay          = 8;                             // d_i cap (ticks)
constexpr int kMaxWindow         = 32;                            // inputs or bundles in one datagram
constexpr uint32_t kCksInterval  = 30;
constexpr uint8_t kNoSlot        = 255;

// 8 bytes per seat per tick (D9): u16 buttons, s8 stickX, stickY, substickX, substickY, u8 triggerL, triggerR.
struct Input {
	uint8_t b[8] = {};
	bool operator==(const Input& o) const { return std::memcmp(b, o.b, 8) == 0; }
	bool operator!=(const Input& o) const { return !(*this == o); }
};

enum class Mode : uint8_t { Battle = 0, Battle2v2 = 1, Challenge = 2 };
inline int mode_seats(Mode m) { return m == Mode::Battle2v2 ? 4 : 2; }
inline const char* mode_name(Mode m)
{
	return m == Mode::Battle2v2 ? "2v2" : (m == Mode::Challenge ? "challenge" : "battle");
}

// START payload (wire type 6). Everything every peer needs to enter the same match.
struct SessionConfig {
	uint32_t sessionTag   = 0;
	uint32_t seed         = 0;
	Mode mode             = Mode::Battle;
	uint8_t stage         = 0;     // VS stage or Challenge course
	uint8_t seats         = 2;
	uint8_t slotMask      = 0x3;
	uint32_t slotMember[kMaxSeats] = { 0, 0, 0, 0 };
	uint8_t handicap[2]   = { 0, 0 };
	uint8_t tickOrder     = 1;
	float simK            = 3.6f;
	uint16_t cksInterval  = kCksInterval;
	uint8_t maxDelay      = kMaxDelay;
	uint32_t syncedHash   = 0;
	std::string syncedJson;          // <= 2048
	uint8_t build[32]     = {};
	uint8_t content[32]   = {};
	std::string mods;                // <= 1023
};

// Why a session ended (mirrors mppm::online::EndReason).
enum EndReason : int {
	kEndNone = 0,
	kEndLeft,
	kEndHostLeft,
	kEndKicked,
	kEndDesync,
	kEndConnectionLost,
	kEndRefused,
	kEndServer,
	kEndDone,      // internal: --exit-after-ticks or window close
	kEndError,     // internal: settings differ, other local failure
};

// u32 microsecond clock fields wrap: differences are int32.
inline uint32_t us32(int64_t ns) { return (uint32_t)(uint64_t)(ns / 1000); }
inline int32_t us_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

} // namespace mppm::session
