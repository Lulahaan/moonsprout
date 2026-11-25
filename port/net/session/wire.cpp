// M5.3b session wire format: encoders and bounds-checked decoders (see wire.hpp).
#include "wire.hpp"

#include <cstring>

namespace mppm::session::wire {

namespace {

struct W {
	std::vector<uint8_t>& o;
	void u8(uint8_t v) { o.push_back(v); }
	void u16(uint16_t v)
	{
		o.push_back((uint8_t)v);
		o.push_back((uint8_t)(v >> 8));
	}
	void u32(uint32_t v)
	{
		for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i)));
	}
	void u64(uint64_t v)
	{
		for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (8 * i)));
	}
	void bytes(const uint8_t* p, size_t n) { o.insert(o.end(), p, p + n); }
	void str8(const std::string& s, size_t max, size_t maxChars = (size_t)-1)
	{
		const std::string c = utf8_cut(s, max, maxChars);
		u8((uint8_t)c.size());
		bytes((const uint8_t*)c.data(), c.size());
	}
	void str16(const std::string& s, size_t max)
	{
		const std::string c = utf8_cut(s, max);
		u16((uint16_t)c.size());
		bytes((const uint8_t*)c.data(), c.size());
	}
	void input(const Input& in) { bytes(in.b, 8); }
};

// Reader: every read checks the remaining length; `bad` latches.
struct R {
	const uint8_t* p;
	size_t n;
	size_t i  = 0;
	bool bad  = false;
	bool need(size_t k)
	{
		if (bad || n - i < k) {
			bad = true;
			return false;
		}
		return true;
	}
	uint8_t u8()
	{
		if (!need(1)) return 0;
		return p[i++];
	}
	uint16_t u16()
	{
		if (!need(2)) return 0;
		uint16_t v = (uint16_t)(p[i] | (p[i + 1] << 8));
		i += 2;
		return v;
	}
	uint32_t u32()
	{
		if (!need(4)) return 0;
		uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
		i += 4;
		return v;
	}
	uint64_t u64()
	{
		uint64_t lo = u32();
		uint64_t hi = u32();
		return lo | (hi << 32);
	}
	void bytes(uint8_t* out, size_t k)
	{
		if (!need(k)) {
			std::memset(out, 0, k);
			return;
		}
		std::memcpy(out, p + i, k);
		i += k;
	}
	// A length-prefixed string; `over` is set when the length is above max (the bytes are still skipped).
	std::string str(size_t len, size_t max, bool& over)
	{
		if (len > max) over = true;
		if (!need(len)) return {};
		std::string s((const char*)p + i, len);
		i += len;
		return s;
	}
	std::string str8(size_t max, bool& over) { return str(u8(), max, over); }
	std::string str16(size_t max, bool& over) { return str(u16(), max, over); }
	void input(Input& in) { bytes(in.b, 8); }
	bool done() const { return !bad && i == n; }
};

Result begin(R& r, uint8_t type)
{
	Header h;
	Result res = decode_header(r.p, r.n, h);
	if (res != Result::Ok) return res;
	if (h.type != type) return Result::Invalid;
	r.i = kHeader;
	return Result::Ok;
}

Result finish(const R& r, bool invalid)
{
	if (!r.done()) return Result::Malformed;
	return invalid ? Result::Invalid : Result::Ok;
}

} // namespace

std::string utf8_cut(const std::string& s, size_t maxBytes, size_t maxChars)
{
	size_t i = 0, chars = 0;
	while (i < s.size() && chars < maxChars) {
		size_t j = i + 1; // the next lead byte (continuation bytes belong to the character before)
		while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) ++j;
		if (j > maxBytes) break;
		i = j;
		++chars;
	}
	return s.substr(0, i);
}

std::string clean_name(const std::string& in)
{
	std::string out;
	const auto* p  = (const unsigned char*)in.data();
	const size_t n = in.size();
	for (size_t i = 0; i < n;) {
		const unsigned c = p[i];
		const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (len == 0) return "";
		if (i + len > n) {
			// The last character is cut short (a byte-count cut): drop it if what is there is a valid start.
			for (size_t k = i + 1; k < n; ++k)
				if ((p[k] & 0xC0) != 0x80) return "";
			break;
		}
		uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
		for (size_t k = 1; k < len; ++k) {
			if ((p[i + k] & 0xC0) != 0x80) return "";
			cp = (cp << 6) | (p[i + k] & 0x3F);
		}
		const uint32_t minOf[5] = { 0, 0, 0x80, 0x800, 0x10000 };
		if (cp < minOf[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return ""; // overlong, surrogate
		if (!(cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))) out.append(in, i, len);
		i += len;
	}
	const size_t a = out.find_first_not_of(' ');
	if (a == std::string::npos) return "";
	out = utf8_cut(out.substr(a), out.size(), kMaxNameChars); // the server trims, then keeps 24, then trims the end
	return out.substr(0, out.find_last_not_of(' ') + 1);
}

void put_header(std::vector<uint8_t>& out, uint8_t type, uint32_t tag, uint8_t flags)
{
	W w{ out };
	w.u8(kMagic);
	w.u8(kVersion);
	w.u8(type);
	w.u8(flags);
	w.u32(tag);
}

Result decode_header(const uint8_t* p, size_t n, Header& h)
{
	if (p == nullptr || n < kHeader) return Result::Malformed;
	if (p[0] != kMagic || p[1] != kVersion) return Result::Malformed;
	h.type  = p[2];
	h.flags = p[3];
	h.tag   = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
	if (!is_control(h.type) && !is_input_channel(h.type)) return Result::Invalid;
	return Result::Ok;
}

// ---- HELLO ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Hello& m)
{
	put_header(out, HELLO, tag);
	W w{ out };
	w.u16(m.proto);
	w.u32(m.member);
	w.u64(m.secret);
	w.u8(m.spectator ? 1 : 0);
	w.str8(m.name, kMaxName, kMaxNameChars);
	w.bytes(m.build, 32);
	w.bytes(m.content, 32);
	w.str16(m.mods, kMaxMods);
}
Result decode(const uint8_t* p, size_t n, Hello& m)
{
	R r{ p, n };
	if (Result res = begin(r, HELLO); res != Result::Ok) return res;
	bool over = false;
	m.proto     = r.u16();
	m.member    = r.u32();
	m.secret    = r.u64();
	uint8_t sp  = r.u8();
	m.spectator = sp != 0;
	m.name      = r.str8(kMaxName, over);
	r.bytes(m.build, 32);
	r.bytes(m.content, 32);
	m.mods = r.str16(kMaxMods, over);
	return finish(r, over || sp > 1);
}

// ---- WELCOME ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Welcome& m)
{
	put_header(out, WELCOME, tag);
	W w{ out };
	w.u32(m.member);
	w.u8(m.role);
}
Result decode(const uint8_t* p, size_t n, Welcome& m)
{
	R r{ p, n };
	if (Result res = begin(r, WELCOME); res != Result::Ok) return res;
	m.member = r.u32();
	m.role   = r.u8();
	return finish(r, m.role != 1 && m.role != 2);
}

// ---- REJECT ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Reject& m)
{
	put_header(out, REJECT, tag);
	W w{ out };
	w.u8(m.reason);
	w.str16(m.text, kMaxText);
}
Result decode(const uint8_t* p, size_t n, Reject& m)
{
	R r{ p, n };
	if (Result res = begin(r, REJECT); res != Result::Ok) return res;
	bool over = false;
	m.reason  = r.u8();
	m.text    = r.str16(kMaxText, over);
	return finish(r, over || m.reason < 1 || m.reason > 5);
}

// ---- LOBBY ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Lobby& m)
{
	put_header(out, LOBBY, tag);
	W w{ out };
	w.u32(m.generation);
	w.u8(m.phase);
	w.u8(m.mode);
	w.u8(m.stage);
	w.u8(m.seats);
	w.u8(m.handicap[0]);
	w.u8(m.handicap[1]);
	const size_t count = m.members.size() > (size_t)kMaxMembers ? (size_t)kMaxMembers : m.members.size();
	w.u8((uint8_t)count);
	for (size_t i = 0; i < count; ++i) {
		const LobbyMember& mm = m.members[i];
		w.u32(mm.id);
		w.u8(mm.slot);
		w.u8(mm.flags);
		w.u16(mm.pingMs);
		w.u8(mm.delay);
		w.u8(mm.link);
		w.str8(mm.name, kMaxName, kMaxNameChars);
		w.str8(mm.note, kMaxNote);
	}
}
Result decode(const uint8_t* p, size_t n, Lobby& m)
{
	R r{ p, n };
	if (Result res = begin(r, LOBBY); res != Result::Ok) return res;
	bool over    = false;
	m.generation = r.u32();
	m.phase      = r.u8();
	m.mode       = r.u8();
	m.stage      = r.u8();
	m.seats      = r.u8();
	m.handicap[0] = r.u8();
	m.handicap[1] = r.u8();
	const uint8_t count = r.u8();
	if (count > kMaxMembers) over = true;
	m.members.clear();
	for (int i = 0; i < count && !r.bad; ++i) {
		LobbyMember mm;
		mm.id     = r.u32();
		mm.slot   = r.u8();
		mm.flags  = r.u8();
		mm.pingMs = r.u16();
		mm.delay  = r.u8();
		mm.link   = r.u8();
		mm.name   = r.str8(kMaxName, over);
		mm.note   = r.str8(kMaxNote, over);
		if (mm.slot != kNoSlot && mm.slot >= kMaxSeats) over = true;
		if (m.members.size() < (size_t)kMaxMembers) m.members.push_back(std::move(mm));
	}
	const bool bad = over || m.mode > 2 || (m.seats != 2 && m.seats != 4) || m.phase > 1 || m.handicap[0] > 10 || m.handicap[1] > 10;
	return finish(r, bad);
}

// ---- REQ ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Req& m)
{
	put_header(out, REQ, tag);
	W w{ out };
	w.u8(m.what);
	w.u8((uint8_t)m.value);
}
Result decode(const uint8_t* p, size_t n, Req& m)
{
	R r{ p, n };
	if (Result res = begin(r, REQ); res != Result::Ok) return res;
	m.what  = r.u8();
	m.value = (int8_t)r.u8();
	return finish(r, m.what < kReqSlot || m.what > kReqLeave);
}

// ---- START ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const SessionConfig& m)
{
	put_header(out, START, tag);
	W w{ out };
	w.u32(m.sessionTag);
	w.u32(m.seed);
	w.u8((uint8_t)m.mode);
	w.u8(m.stage);
	w.u8(m.seats);
	w.u8(m.slotMask);
	for (int i = 0; i < kMaxSeats; ++i) w.u32(m.slotMember[i]);
	w.u8(m.handicap[0]);
	w.u8(m.handicap[1]);
	w.u8(m.tickOrder);
	uint32_t kBits;
	std::memcpy(&kBits, &m.simK, 4);
	w.u32(kBits);
	w.u16(m.cksInterval);
	w.u8(m.maxDelay);
	w.u32(m.syncedHash);
	w.str16(m.syncedJson, kMaxSynced);
	w.bytes(m.build, 32);
	w.bytes(m.content, 32);
	w.str16(m.mods, kMaxMods);
}
Result decode(const uint8_t* p, size_t n, SessionConfig& m)
{
	R r{ p, n };
	if (Result res = begin(r, START); res != Result::Ok) return res;
	bool over    = false;
	m.sessionTag = r.u32();
	m.seed       = r.u32();
	uint8_t mode = r.u8();
	m.mode       = (Mode)mode;
	m.stage      = r.u8();
	m.seats      = r.u8();
	m.slotMask   = r.u8();
	for (int i = 0; i < kMaxSeats; ++i) m.slotMember[i] = r.u32();
	m.handicap[0] = r.u8();
	m.handicap[1] = r.u8();
	m.tickOrder   = r.u8();
	uint32_t kBits = r.u32();
	std::memcpy(&m.simK, &kBits, 4);
	m.cksInterval = r.u16();
	m.maxDelay    = r.u8();
	m.syncedHash  = r.u32();
	m.syncedJson  = r.str16(kMaxSynced, over);
	r.bytes(m.build, 32);
	r.bytes(m.content, 32);
	m.mods = r.str16(kMaxMods, over);
	const bool bad = over || mode > 2 || (m.seats != 2 && m.seats != 4) || m.seats != mode_seats(m.mode) ||
	                 (m.slotMask & ~((1u << m.seats) - 1)) != 0 || m.tickOrder > 1 || !(m.simK >= 1.0f && m.simK <= 3.6f) ||
	                 m.cksInterval == 0 || m.maxDelay < 1 || m.maxDelay > kMaxDelay || m.handicap[0] > 10 || m.handicap[1] > 10 ||
	                 m.sessionTag == 0;
	return finish(r, bad);
}

// ---- RELEASE ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Release& m)
{
	put_header(out, RELEASE, tag);
	W w{ out };
	w.u32(m.barrierTick);
	w.u16(m.count);
	w.u64(m.releaseHostNs);
}
Result decode(const uint8_t* p, size_t n, Release& m)
{
	R r{ p, n };
	if (Result res = begin(r, RELEASE); res != Result::Ok) return res;
	m.barrierTick   = r.u32();
	m.count         = r.u16();
	m.releaseHostNs = r.u64();
	return finish(r, m.count == 0);
}

// ---- DROP ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Drop& m)
{
	put_header(out, DROP, tag);
	W w{ out };
	w.u8(m.slot);
	w.u32(m.effectiveTick);
	w.u8(m.reason);
}
Result decode(const uint8_t* p, size_t n, Drop& m)
{
	R r{ p, n };
	if (Result res = begin(r, DROP); res != Result::Ok) return res;
	m.slot          = r.u8();
	m.effectiveTick = r.u32();
	m.reason        = r.u8();
	return finish(r, m.slot >= kMaxSeats || m.reason < 1 || m.reason > 4);
}

// ---- DESYNC ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Desync& m)
{
	put_header(out, DESYNC, tag);
	W w{ out };
	w.u32(m.tick);
	w.u8(m.slot);
	w.u32(m.hostTotal);
	w.u32(m.peerTotal);
}
Result decode(const uint8_t* p, size_t n, Desync& m)
{
	R r{ p, n };
	if (Result res = begin(r, DESYNC); res != Result::Ok) return res;
	m.tick      = r.u32();
	m.slot      = r.u8();
	m.hostTotal = r.u32();
	m.peerTotal = r.u32();
	return finish(r, m.slot != kNoSlot && m.slot >= kMaxSeats);
}

// ---- CKSLINE ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const CksLine& m)
{
	put_header(out, CKSLINE, tag);
	W w{ out };
	w.u32(m.tick);
	w.str16(m.line, kMaxCks);
}
Result decode(const uint8_t* p, size_t n, CksLine& m)
{
	R r{ p, n };
	if (Result res = begin(r, CKSLINE); res != Result::Ok) return res;
	bool over = false;
	m.tick    = r.u32();
	m.line    = r.str16(kMaxCks, over);
	return finish(r, over);
}

// ---- END ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const End& m)
{
	put_header(out, END, tag);
	W w{ out };
	w.u8(m.reason);
	w.str16(m.text, kMaxText);
}
Result decode(const uint8_t* p, size_t n, End& m)
{
	R r{ p, n };
	if (Result res = begin(r, END); res != Result::Ok) return res;
	bool over = false;
	m.reason  = r.u8();
	m.text    = r.str16(kMaxText, over);
	return finish(r, over);
}

// ---- BYE ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const Bye& m)
{
	put_header(out, BYE, tag);
	W w{ out };
	w.u8(m.reason);
}
Result decode(const uint8_t* p, size_t n, Bye& m)
{
	R r{ p, n };
	if (Result res = begin(r, BYE); res != Result::Ok) return res;
	m.reason = r.u8();
	return finish(r, false);
}

// ---- INPUT ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const InputPkt& m)
{
	put_header(out, INPUT, tag);
	W w{ out };
	w.u32(m.tSendUs);
	w.u32(m.echoUs);
	w.u32(m.holdUs);
	w.u32(m.ackBundle);
	w.u32(m.firstTag);
	const uint8_t count = m.count > kMaxWindow ? (uint8_t)kMaxWindow : m.count;
	w.u8(count);
	w.u8(m.slot);
	w.u8(m.delay);
	w.u8(m.flags);
	w.u32(m.loadedTick);
	w.u16(m.loadCount);
	w.u32(m.cksTick);
	w.u32(m.cksTotal);
	w.u16(m.downP99);
	w.u16(m.stallPermille);
	for (int i = 0; i < count; ++i) w.input(m.inputs[i]);
}
Result decode(const uint8_t* p, size_t n, InputPkt& m)
{
	R r{ p, n };
	if (Result res = begin(r, INPUT); res != Result::Ok) return res;
	m.tSendUs   = r.u32();
	m.echoUs    = r.u32();
	m.holdUs    = r.u32();
	m.ackBundle = r.u32();
	m.firstTag  = r.u32();
	m.count     = r.u8();
	m.slot      = r.u8();
	m.delay     = r.u8();
	m.flags     = r.u8();
	m.loadedTick = r.u32();
	m.loadCount  = r.u16();
	m.cksTick    = r.u32();
	m.cksTotal   = r.u32();
	m.downP99    = r.u16();
	m.stallPermille = r.u16();
	if (m.count > kMaxWindow) {
		r.bad = true; // the length the fields imply cannot be checked: structurally malformed
		return Result::Malformed;
	}
	for (int i = 0; i < m.count; ++i) r.input(m.inputs[i]);
	const bool bad = (m.slot != kNoSlot && m.slot >= kMaxSeats) || (m.slot == kNoSlot && m.count != 0) || m.delay > kMaxDelay ||
	                 (m.count > 0 && m.firstTag == 0);
	return finish(r, bad);
}

// ---- BUNDLE ----
void encode(std::vector<uint8_t>& out, uint32_t tag, const BundlePkt& m)
{
	put_header(out, BUNDLE, tag);
	W w{ out };
	w.u32(m.tSendUs);
	w.u32(m.echoUs);
	w.u32(m.holdUs);
	w.u32(m.hostTick);
	w.u32(m.hostDueUs);
	w.u32(m.firstTick);
	const uint8_t count  = m.count > kMaxWindow ? (uint8_t)kMaxWindow : m.count;
	const uint8_t events = m.nEvents > kMaxEvents ? (uint8_t)kMaxEvents : m.nEvents;
	w.u8(count);
	w.u8(m.slotMask);
	w.u8(events);
	w.u8(m.flags);
	w.u32(m.releaseTick);
	w.u32(m.releaseHostUs);
	w.u16(m.releaseCount);
	w.u8(m.waitMask);
	w.u8(0);
	w.u32(0); // reserved
	for (int s = 0; s < kMaxSeats; ++s) w.u8(m.upP99[s]);
	for (int s = 0; s < kMaxSeats; ++s) w.u8(m.downP99[s]);
	for (int s = 0; s < kMaxSeats; ++s) w.u8(m.blame[s]);
	for (int i = 0; i < count; ++i)
		for (int s = 0; s < kMaxSeats; ++s)
			if (m.slotMask & (1u << s)) w.input(m.bundles[i][s]);
	for (int e = 0; e < events; ++e) {
		w.u32(m.events[e].tick);
		w.u8(m.events[e].id);
		w.u8(m.events[e].arg);
		w.u16(m.events[e].seq);
	}
}
Result decode(const uint8_t* p, size_t n, BundlePkt& m)
{
	R r{ p, n };
	if (Result res = begin(r, BUNDLE); res != Result::Ok) return res;
	m.tSendUs   = r.u32();
	m.echoUs    = r.u32();
	m.holdUs    = r.u32();
	m.hostTick  = r.u32();
	m.hostDueUs = r.u32();
	m.firstTick = r.u32();
	m.count     = r.u8();
	m.slotMask  = r.u8();
	m.nEvents   = r.u8();
	m.flags     = r.u8();
	m.releaseTick   = r.u32();
	m.releaseHostUs = r.u32();
	m.releaseCount  = r.u16();
	m.waitMask      = r.u8();
	(void)r.u8();
	(void)r.u32();
	for (int s = 0; s < kMaxSeats; ++s) m.upP99[s] = r.u8();
	for (int s = 0; s < kMaxSeats; ++s) m.downP99[s] = r.u8();
	for (int s = 0; s < kMaxSeats; ++s) m.blame[s] = r.u8();
	if (m.count > kMaxWindow || m.nEvents > kMaxEvents || (m.slotMask & 0xF0) != 0) {
		r.bad = true;
		return Result::Malformed;
	}
	for (int i = 0; i < m.count; ++i)
		for (int s = 0; s < kMaxSeats; ++s) {
			if (m.slotMask & (1u << s)) r.input(m.bundles[i][s]);
			else m.bundles[i][s] = Input{};
		}
	for (int e = 0; e < m.nEvents; ++e) {
		m.events[e].tick = r.u32();
		m.events[e].id   = r.u8();
		m.events[e].arg  = r.u8();
		m.events[e].seq  = r.u16();
	}
	const bool bad = (m.count > 0 && m.firstTick == 0) || ((m.flags & kBundleRelease) && m.releaseCount == 0) || (m.waitMask & 0xF0) != 0;
	return finish(r, bad);
}

Result decode_any(const uint8_t* p, size_t n)
{
	Header h;
	Result res = decode_header(p, n, h);
	if (res != Result::Ok) return res;
	switch (h.type) {
	case HELLO: { Hello m; return decode(p, n, m); }
	case WELCOME: { Welcome m; return decode(p, n, m); }
	case REJECT: { Reject m; return decode(p, n, m); }
	case LOBBY: { Lobby m; return decode(p, n, m); }
	case REQ: { Req m; return decode(p, n, m); }
	case START: { SessionConfig m; return decode(p, n, m); }
	case RELEASE: { Release m; return decode(p, n, m); }
	case DROP: { Drop m; return decode(p, n, m); }
	case DESYNC: { Desync m; return decode(p, n, m); }
	case CKSLINE: { CksLine m; return decode(p, n, m); }
	case END: { End m; return decode(p, n, m); }
	case BYE: { Bye m; return decode(p, n, m); }
	case CHUNK: return (n >= kHeader + 2 && n <= kHeader + 2 + kChunkBytes && p[kHeader + 1] >= 2 && p[kHeader + 1] <= kMaxChunks &&
	                    p[kHeader] < p[kHeader + 1]) ? Result::Ok : Result::Malformed;
	case INPUT: { InputPkt m; return decode(p, n, m); }
	case BUNDLE: { BundlePkt m; return decode(p, n, m); }
	default: return Result::Invalid;
	}
}

} // namespace mppm::session::wire
