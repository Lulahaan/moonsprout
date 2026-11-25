// M5.3b session wire format (docs/online/M5_3B_LOCKSTEP.md, "Wire format"). Little-endian; ENet
// carries the datagram length. Every decoder checks every length and range and never reads outside
// the buffer: a structurally malformed datagram is Malformed (the sender is disconnected), a datagram
// that parses but breaks a rule (count > 32, a name too long) is Invalid (dropped and counted).
#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mppm::session::wire {

constexpr uint8_t kMagic    = 0xB5;
constexpr uint8_t kVersion  = 1;
constexpr uint16_t kProto   = 1;
constexpr size_t kHeader    = 8;
constexpr size_t kMaxNameChars = 24; // a name: at most 24 code points, as mppm-server keeps (sanitize_name) ...
constexpr size_t kMaxName   = 96;     // ... so at most 96 bytes of UTF-8
constexpr size_t kMaxNote   = 120;
constexpr size_t kMaxMods   = 1023;
constexpr size_t kMaxText   = 200;
constexpr size_t kMaxCks    = 300;
constexpr size_t kMaxSynced = 2048;
constexpr int kMaxEvents    = 8;
constexpr int kMaxMembers   = 16;

enum Type : uint8_t {
	HELLO = 1, WELCOME = 2, REJECT = 3, LOBBY = 4, REQ = 5, START = 6, RELEASE = 7, DROP = 8,
	DESYNC = 9, CKSLINE = 10, END = 11, BYE = 12,
	CHUNK = 13, // a control message above kMaxChunked bytes in parts (the transport caps a datagram at 1200)
	INPUT = 32, BUNDLE = 33,
};
inline bool is_control(uint8_t t) { return t >= HELLO && t <= CHUNK; }
// CHUNK payload: u8 index, u8 count (2..16), then up to kChunkBytes bytes of the whole datagram.
constexpr size_t kMaxChunked = 1100;
constexpr size_t kChunkBytes = 1000;
constexpr int kMaxChunks     = 16;
inline bool is_input_channel(uint8_t t) { return t == INPUT || t == BUNDLE; }

enum class Result { Ok, Malformed, Invalid };

struct Header {
	uint8_t type       = 0;
	uint8_t flags      = 0;
	uint32_t tag       = 0; // session tag, 0 before START
};

enum RejectReason : uint8_t { kRejectProtocol = 1, kRejectAuth = 2, kRejectFull = 3, kRejectBanned = 4, kRejectStarted = 5 };
enum ReqWhat : uint8_t { kReqSlot = 1, kReqReady = 2, kReqLeave = 3 };
enum DropReason : uint8_t { kDropLeft = 1, kDropTimeout = 2, kDropKicked = 3, kDropByHost = 4 };
enum ByeReason : uint8_t { kByeLeft = 1, kByeDone = 2, kByeError = 3, kByeDesync = 4 };
enum MemberFlag : uint8_t { kMemReady = 1, kMemSpectator = 2, kMemContentOk = 4, kMemHost = 8, kMemConnected = 16 };
enum LobbyPhase : uint8_t { kPhaseLobby = 0, kPhaseStarted = 1 };
enum LinkKind : uint8_t { kLinkUnknown = 0, kLinkDirect = 1, kLinkIce = 2, kLinkRelay = 3, kLinkHost = 4, kLinkMem = 5 };

struct Hello {
	uint16_t proto   = kProto;
	uint32_t member  = 0;
	uint64_t secret  = 0;
	bool spectator   = false;
	std::string name;
	uint8_t build[32]   = {};
	uint8_t content[32] = {};
	std::string mods;
};
struct Welcome {
	uint32_t member = 0;
	uint8_t role    = 1; // 1 player, 2 spectator
};
struct Reject {
	uint8_t reason = 0;
	std::string text;
};
struct LobbyMember {
	uint32_t id     = 0;
	uint8_t slot    = kNoSlot;
	uint8_t flags   = 0;
	uint16_t pingMs = 0xFFFF;
	uint8_t delay   = 0;
	uint8_t link    = 0;
	std::string name;
	std::string note;
};
struct Lobby {
	uint32_t generation = 0;
	uint8_t phase       = kPhaseLobby;
	uint8_t mode        = 0;
	uint8_t stage       = 0;
	uint8_t seats       = 2;
	uint8_t handicap[2] = { 0, 0 };
	std::vector<LobbyMember> members;
};
struct Req {
	uint8_t what = 0;
	int8_t value = 0;
};
struct Release {
	uint32_t barrierTick = 0;
	uint16_t count       = 0; // the barrier's number (outermost nonsim leaves since the session start)
	uint64_t releaseHostNs = 0;
};
struct Drop {
	uint8_t slot = 0;
	uint32_t effectiveTick = 0;
	uint8_t reason = 0;
};
struct Desync {
	uint32_t tick = 0;
	uint8_t slot  = kNoSlot;
	uint32_t hostTotal = 0, peerTotal = 0;
};
struct CksLine {
	uint32_t tick = 0;
	std::string line;
};
struct End {
	uint8_t reason = 0;
	std::string text;
};
struct Bye {
	uint8_t reason = 0;
};

constexpr uint32_t kNoLoaded = 0xFFFFFFFFu;

struct InputPkt {
	uint32_t tSendUs = 0, echoUs = 0, holdUs = 0;
	uint32_t ackBundle  = 0;          // every bundle <= it received (0 = none)
	uint32_t firstTag   = 0;
	uint8_t count       = 0;          // 0..32
	uint8_t slot        = kNoSlot;    // 255 spectator
	uint8_t delay       = 0;
	uint8_t flags       = 0;
	uint32_t loadedTick = kNoLoaded;  // last barrier tick reached
	uint16_t loadCount  = 0;          // barriers reached since the session start
	uint32_t cksTick = 0, cksTotal = 0; // (0, 0) none
	uint16_t downP99 = 0;             // 0.1 ms units, 0 unknown
	uint16_t stallPermille = 0;
	Input inputs[kMaxWindow];
};

struct Event {
	uint32_t tick = 0;
	uint8_t id    = 0; // 1 drop notice (arg = seat)
	uint8_t arg   = 0;
	uint16_t seq  = 0;
};

enum BundleFlag : uint8_t { kBundleRelease = 1 };

struct BundlePkt {
	uint32_t tSendUs = 0, echoUs = 0, holdUs = 0;
	uint32_t hostTick = 0, hostDueUs = 0;
	uint32_t firstTick = 0;
	uint8_t count    = 0;
	uint8_t slotMask = 0;
	uint8_t nEvents  = 0;
	uint8_t flags    = 0;
	uint32_t releaseTick = 0, releaseHostUs = 0;
	uint16_t releaseCount = 0;
	uint8_t waitMask = 0;
	uint8_t upP99[kMaxSeats]   = { 255, 255, 255, 255 }; // ms, 255 unknown
	uint8_t downP99[kMaxSeats] = { 255, 255, 255, 255 };
	uint8_t blame[kMaxSeats]   = { 0, 0, 0, 0 };         // permille / 4
	Input bundles[kMaxWindow][kMaxSeats];                 // [i][seat], only the seats of slotMask on the wire
	Event events[kMaxEvents];
};

// ---- encoding (appends to out; the header first) ----
void put_header(std::vector<uint8_t>& out, uint8_t type, uint32_t tag, uint8_t flags = 0);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Hello& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Welcome& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Reject& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Lobby& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Req& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const SessionConfig& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Release& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Drop& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Desync& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const CksLine& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const End& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const Bye& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const InputPkt& m);
void encode(std::vector<uint8_t>& out, uint32_t tag, const BundlePkt& m);

// ---- decoding ----
// The header: Malformed when shorter than 8 bytes, a bad magic or version; Invalid for an unknown type.
Result decode_header(const uint8_t* p, size_t n, Header& h);
// Each decodes the whole datagram (header included; the type must match).
Result decode(const uint8_t* p, size_t n, Hello& m);
Result decode(const uint8_t* p, size_t n, Welcome& m);
Result decode(const uint8_t* p, size_t n, Reject& m);
Result decode(const uint8_t* p, size_t n, Lobby& m);
Result decode(const uint8_t* p, size_t n, Req& m);
Result decode(const uint8_t* p, size_t n, SessionConfig& m);
Result decode(const uint8_t* p, size_t n, Release& m);
Result decode(const uint8_t* p, size_t n, Drop& m);
Result decode(const uint8_t* p, size_t n, Desync& m);
Result decode(const uint8_t* p, size_t n, CksLine& m);
Result decode(const uint8_t* p, size_t n, End& m);
Result decode(const uint8_t* p, size_t n, Bye& m);
Result decode(const uint8_t* p, size_t n, InputPkt& m);
Result decode(const uint8_t* p, size_t n, BundlePkt& m);

// Decodes any datagram of a known type into a scratch object (the fuzz test, logging).
Result decode_any(const uint8_t* p, size_t n);

// At most maxBytes bytes and maxChars code points of s, cut at a UTF-8 code point boundary (every
// encoder cuts its strings this way, so a long name never ends in half a character).
std::string utf8_cut(const std::string& s, size_t maxBytes, size_t maxChars = (size_t)-1);
// A player name the way mppm-server's sanitize_name makes it: valid UTF-8 only (an incomplete last
// character, a name cut by an older encoder, is dropped; other invalid UTF-8 gives ""), control
// characters (C0, DEL, C1) removed, spaces trimmed, at most kMaxNameChars code points. "" when nothing
// is left (the callers put "Player" there).
std::string clean_name(const std::string& in);

inline int popcount4(uint8_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }

} // namespace mppm::session::wire
