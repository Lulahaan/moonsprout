// M5.3b lobby client: mppm-server over one WebSocket (borealis::ws). The protocol is
// server/PROTOCOL.md; the interface and threading rules are in lobby_client.hpp.
#include "lobby_client.hpp"

#include <borealis/ws.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace mppm::net {
namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxSendBytes = 16 * 1024;    // the server refuses larger frames
constexpr size_t kMaxReceiveBytes = 64 * 1024; // a 4096-byte signal can grow with JSON escapes
constexpr size_t kMaxSignalData = 4096;
constexpr size_t kMaxNameChars = 24;
constexpr size_t kMaxPasswordBytes = 64;
constexpr size_t kMaxIceServers = 16;
constexpr auto kPingInterval = std::chrono::seconds(20);
constexpr auto kServerSilence = std::chrono::seconds(65); // the server answers every ping
constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr auto kAnswerTimeout = std::chrono::seconds(10); // open socket, no created/joined yet
constexpr const char* kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void log_line(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "mppm: lobby: %s\n", buf);
}

// At most `maxBytes` bytes of `s`, cut at a UTF-8 character boundary.
std::string utf8_prefix(const std::string& s, size_t maxBytes) {
	if (s.size() <= maxBytes) {
		return s;
	}
	size_t n = maxBytes;
	while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
		--n;
	}
	return s.substr(0, n);
}

// Invalid UTF-8 (a name typed in another encoding) becomes U+FFFD instead of throwing.
std::string dump(const nlohmann::json& j) {
	return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool is_hex(const std::string& s, size_t bytes) {
	if (s.size() != bytes * 2) {
		return false;
	}
	return std::all_of(s.begin(), s.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	});
}

int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return c - 'A' + 10;
}

uint64_t hex_u64(const std::string& s) {
	uint64_t v = 0;
	for (char c : s) {
		v = (v << 4) | static_cast<uint64_t>(hex_digit(c));
	}
	return v;
}

// 1..max code points of valid UTF-8 (the JSON parser already checked the encoding), no C0/DEL.
bool valid_text(const std::string& s, size_t maxChars) {
	size_t chars = 0;
	for (unsigned char c : s) {
		if (c < 0x20 || c == 0x7f) {
			return false;
		}
		if ((c & 0xC0) != 0x80) {
			++chars;
		}
	}
	return chars >= 1 && chars <= maxChars;
}

bool printable_ascii(const std::string& s, size_t maxBytes) {
	if (s.empty() || s.size() > maxBytes) {
		return false;
	}
	return std::all_of(s.begin(), s.end(), [](char c) { return c > 0x20 && c < 0x7f; });
}

// "a.b.c.d" with each part 0..255; `first` receives the first octet.
bool parse_ipv4(const std::string& s, int* first = nullptr) {
	int parts = 0;
	size_t i = 0;
	while (i <= s.size()) {
		size_t j = i;
		int v = 0;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9' && j - i < 3) {
			v = v * 10 + (s[j] - '0');
			++j;
		}
		if (j == i || v > 255) {
			return false;
		}
		if (parts == 0 && first) {
			*first = v;
		}
		++parts;
		if (j == s.size()) {
			break;
		}
		if (s[j] != '.' || parts == 4) {
			return false;
		}
		i = j + 1;
	}
	return parts == 4;
}

bool parse_port(const std::string& s) {
	if (s.empty() || s.size() > 5 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		return false;
	}
	const long v = std::strtol(s.c_str(), nullptr, 10);
	return v >= 1 && v <= 65535;
}

bool is_loopback_host(std::string host) {
	std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1") {
		return true;
	}
	int first = 0;
	return parse_ipv4(host, &first) && first == 127;
}

// The fields we read, all optional-typed: nullptr when missing or of another type.
const json* field(const json& o, const char* key) {
	const auto it = o.find(key);
	return it == o.end() ? nullptr : &*it;
}

bool get_string(const json& o, const char* key, std::string& out) {
	const json* v = field(o, key);
	if (!v || !v->is_string()) {
		return false;
	}
	out = v->get_ref<const std::string&>();
	return true;
}

bool get_u32(const json& o, const char* key, uint32_t& out) {
	const json* v = field(o, key);
	if (!v || !v->is_number_unsigned()) {
		return false;
	}
	const uint64_t x = v->get<uint64_t>();
	if (x > 0xFFFFFFFFu) {
		return false;
	}
	out = static_cast<uint32_t>(x);
	return true;
}

bool get_bool(const json& o, const char* key, bool& out) {
	const json* v = field(o, key);
	if (!v || !v->is_boolean()) {
		return false;
	}
	out = v->get<bool>();
	return true;
}

bool valid_signal_kind(const std::string& k) {
	return k == "desc" || k == "cand" || k == "done" || k == "addr";
}

// `ice`: [{"urls":[...]|"...","username"?,"credential"?}] -> one IceServer per URL.
bool parse_ice(const json& o, std::vector<IceServer>& out) {
	const json* ice = field(o, "ice");
	if (!ice || ice->is_null()) {
		return true;
	}
	if (!ice->is_array() || ice->size() > kMaxIceServers) {
		return false;
	}
	for (const json& s : *ice) {
		if (!s.is_object()) {
			return false;
		}
		std::string user, cred;
		if (field(s, "username") && !get_string(s, "username", user)) return false;
		if (field(s, "credential") && !get_string(s, "credential", cred)) return false;
		if (user.size() > 256 || cred.size() > 256) return false;
		const json* urls = field(s, "urls");
		std::vector<std::string> list;
		if (urls && urls->is_string()) {
			list.push_back(urls->get_ref<const std::string&>());
		} else if (urls && urls->is_array() && !urls->empty() && urls->size() <= kMaxIceServers) {
			for (const json& u : *urls) {
				if (!u.is_string()) return false;
				list.push_back(u.get_ref<const std::string&>());
			}
		} else {
			return false;
		}
		for (const std::string& u : list) {
			if (!printable_ascii(u, 256) || (u.rfind("stun:", 0) != 0 && u.rfind("turn:", 0) != 0 &&
			                                 u.rfind("stuns:", 0) != 0 && u.rfind("turns:", 0) != 0)) {
				return false;
			}
			if (out.size() >= kMaxIceServers) return false;
			out.push_back(IceServer{u, user, cred});
		}
	}
	return true;
}

// `relay`: null or {"addr":"a.b.c.d:port","token":"<32 hex>"}.
bool parse_relay(const json& o, RelayInfo& out) {
	const json* r = field(o, "relay");
	if (!r || r->is_null()) {
		return true;
	}
	std::string addr, token;
	if (!r->is_object() || !get_string(*r, "addr", addr) || !get_string(*r, "token", token) || !is_hex(token, 16)) {
		return false;
	}
	const size_t colon = addr.rfind(':');
	if (colon == std::string::npos || !parse_ipv4(addr.substr(0, colon)) || !parse_port(addr.substr(colon + 1))) {
		return false;
	}
	out.ipPort = addr;
	for (size_t i = 0; i < 16; ++i) {
		out.token[i] = static_cast<uint8_t>(hex_digit(token[2 * i]) << 4 | hex_digit(token[2 * i + 1]));
	}
	return true;
}

// The display form "ABCD-EFGH" exactly as the server sends it.
bool valid_code(const std::string& s) {
	std::string canon;
	return s.size() == 9 && normalize_code(s, canon) && s == canon.substr(0, 4) + "-" + canon.substr(4);
}

bool valid_session_id(const std::string& s) {
	return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	});
}

const char* failed_code(const std::string& server) {
	static const char* const kPass[] = {"not_found", "full", "bad_password", "rate_limited", "version", "bad_request"};
	for (const char* c : kPass) {
		if (server == c) return c;
	}
	if (server == "forbidden") return "bad_request"; // banned from this session; the message says so
	return "protocol";
}

std::string default_message(const std::string& code) {
	if (code == "not_found") return "No session has this code.";
	if (code == "full") return "The session is full.";
	if (code == "bad_password") return "The password is wrong.";
	if (code == "rate_limited") return "The server is busy; try again in a few minutes.";
	if (code == "version") return "The server speaks a different protocol version; update the game.";
	if (code == "unreachable") return "The server could not be reached.";
	return "The server sent something unexpected.";
}

class LobbyClientImpl final : public LobbyClient {
public:
	LobbyClientImpl(LobbyConfig cfg, std::string url, bool plaintext)
	    : m_cfg(std::move(cfg)), m_url(std::move(url)), m_plaintext(plaintext) {}

	~LobbyClientImpl() override {
		if (m_conn) {
			m_conn.close(1000);
		}
	}

	void create_session(const std::string& password, int maxMembers) override {
		if (m_state != State::Idle) {
			log_line("create_session ignored: the client was already used");
			return;
		}
		m_host = true;
		json msg = {{"t", "create"}, {"v", kProtocolVersion}, {"name", utf8_prefix(m_cfg.name, 96)},
		            {"password", utf8_prefix(password, kMaxPasswordBytes)}, {"maxMembers", std::clamp(maxMembers, 2, 8)}};
		start(dump(msg));
	}

	void join_session(const std::string& code, const std::string& password, bool spectator) override {
		if (m_state != State::Idle) {
			log_line("join_session ignored: the client was already used");
			return;
		}
		m_host = false;
		std::string canon;
		if (!normalize_code(code, canon)) {
			m_state = State::Ended;
			fail("not_found", "That is not a valid join code.");
			return;
		}
		json msg = {{"t", "join"},
		            {"v", kProtocolVersion},
		            {"code", canon.substr(0, 4) + "-" + canon.substr(4)},
		            {"name", utf8_prefix(m_cfg.name, 96)},
		            {"password", utf8_prefix(password, kMaxPasswordBytes)},
		            {"spectator", spectator}};
		start(dump(msg));
	}

	void signal(uint32_t to, const std::string& kind, const std::string& data) override {
		if (!valid_signal_kind(kind) || data.size() > kMaxSignalData) {
			log_line("signal dropped: bad kind or data over 4096 bytes");
			return;
		}
		send_in_session(json{{"t", "signal"}, {"to", to}, {"kind", kind}, {"data", data}}, "signal");
	}

	void kick(uint32_t member, bool ban) override {
		if (!m_host) {
			log_line("kick dropped: only the host can kick");
			return;
		}
		send_in_session(json{{"t", "kick"}, {"member", member}, {"ban", ban}}, "kick");
	}

	void leave() override {
		if (m_state == State::InSession) {
			send_in_session(json{{"t", "leave"}}, "leave");
		}
		if (m_conn) {
			m_conn.close(1000);
		}
		m_state = State::Ended;
	}

	void poll(std::vector<LobbyEvent>& out) override {
		if (m_conn && m_state != State::Ended) {
			borealis::ws::Event ev;
			while (m_state != State::Ended && m_conn.poll(ev)) {
				handle(ev);
			}
			const auto now = Clock::now();
			if (m_state == State::Joining && now - m_lastRecv >= kAnswerTimeout) {
				end_with_failure("unreachable", "The server did not answer.");
			}
			if (m_state == State::InSession) {
				if (now - m_lastPing >= kPingInterval) {
					m_lastPing = now;
					m_conn.send(borealis::ws::MessageKind::Text, R"({"t":"ping"})");
				}
				if (now - m_lastRecv >= kServerSilence) {
					close_session("connection_lost", "The server stopped answering.");
				}
			}
		}
		for (auto& e : m_events) {
			out.push_back(std::move(e));
		}
		m_events.clear();
	}

	bool connected() const override {
		return m_state == State::InSession && m_conn.state() == borealis::ws::Connection::State::Open;
	}

private:
	enum class State : uint8_t { Idle, Connecting, Joining, InSession, Ended };

	void start(std::string firstMessage) {
		m_first = std::move(firstMessage);
		borealis::ws::Options o;
		o.url = m_url;
		o.connectTimeout = kConnectTimeout;
		o.keepaliveInterval = kPingInterval;
		o.maxMessageBytes = kMaxReceiveBytes;
		o.allowPlaintext = m_plaintext;
		m_conn = borealis::ws::connect(std::move(o));
		m_state = State::Connecting;
	}

	void send_in_session(const json& msg, const char* what) {
		if (m_state != State::InSession || !m_conn) {
			log_line("%s dropped: not connected to a session", what);
			return;
		}
		const std::string text = dump(msg);
		if (text.size() > kMaxSendBytes) {
			log_line("%s dropped: over 16 KiB as JSON", what);
			return;
		}
		const auto r = m_conn.send(borealis::ws::MessageKind::Text, text);
		if (r != borealis::ws::SendResult::Ok) {
			log_line("%s dropped: the WebSocket refused it", what);
		}
	}

	void fail(const std::string& code, std::string message) {
		LobbyEvent e;
		e.kind = LobbyEvent::Kind::Failed;
		e.error = code;
		e.message = message.empty() ? default_message(code) : std::move(message);
		log_line("failed: %s (%s)", code.c_str(), e.message.c_str());
		m_events.push_back(std::move(e));
	}

	void end_with_failure(const std::string& code, std::string message) {
		fail(code, std::move(message));
		m_state = State::Ended;
		if (m_conn) {
			m_conn.close(1000);
		}
	}

	void close_session(const std::string& reason, std::string message, const std::string& error = {}) {
		LobbyEvent e;
		e.kind = LobbyEvent::Kind::Closed;
		e.message = reason;
		e.error = error;
		log_line("closed: %s%s%s%s", reason.c_str(), message.empty() ? "" : " (", message.c_str(), message.empty() ? "" : ")");
		m_events.push_back(std::move(e));
		m_state = State::Ended;
		if (m_conn) {
			m_conn.close(1000);
		}
	}

	void protocol_error(const std::string& what) {
		log_line("protocol error: %s", what.c_str());
		if (m_state == State::InSession) {
			close_session("connection_lost", "The server sent something unexpected.", "protocol");
		} else {
			end_with_failure("protocol", "The server sent something unexpected.");
		}
	}

	void handle(const borealis::ws::Event& ev) {
		using K = borealis::ws::Event::Kind;
		switch (ev.kind) {
		case K::Open:
			if (m_state == State::Connecting) {
				m_state = State::Joining;
				m_lastRecv = m_lastPing = Clock::now();
				if (m_conn.send(borealis::ws::MessageKind::Text, m_first) != borealis::ws::SendResult::Ok) {
					end_with_failure("unreachable", "Could not send to the server.");
				}
			}
			break;
		case K::Message:
			m_lastRecv = Clock::now();
			if (ev.messageKind != borealis::ws::MessageKind::Text) {
				protocol_error("binary message");
				break;
			}
			on_message(ev.data);
			break;
		case K::Closed:
			on_closed(ev);
			break;
		}
	}

	void on_closed(const borealis::ws::Event& ev) {
		if (m_state == State::Connecting || m_state == State::Joining) {
			if (!m_pendingError.empty()) {
				end_with_failure(failed_code(m_pendingError), m_pendingMessage);
			} else if (ev.status == 429) {
				end_with_failure("rate_limited", "");
			} else {
				end_with_failure("unreachable", ev.message.empty() ? std::string() : "The server could not be reached (" + ev.message + ").");
			}
		} else if (m_state == State::InSession) {
			close_session("connection_lost", !m_pendingMessage.empty() ? m_pendingMessage : ev.message);
		}
	}

	void on_message(const std::string& text) {
		json j;
		try {
			j = json::parse(text, nullptr, false);
		} catch (...) {
			j = json(json::value_t::discarded);
		}
		if (j.is_discarded() || !j.is_object()) {
			protocol_error("not a JSON object");
			return;
		}
		std::string t;
		if (!get_string(j, "t", t)) {
			protocol_error("no message type");
			return;
		}
		try {
			if (m_state == State::Joining) {
				on_joining(t, j);
			} else if (m_state == State::InSession) {
				on_session(t, j);
			}
		} catch (...) {
			protocol_error("bad field type in " + t);
		}
	}

	void on_error_message(const json& j) {
		std::string code, msg;
		bool fatal = false;
		if (!get_string(j, "code", code) || !printable_ascii(code, 32)) {
			protocol_error("error without a code");
			return;
		}
		get_string(j, "msg", msg);
		if (msg.size() > 256) msg.resize(256);
		get_bool(j, "fatal", fatal);
		if (m_state == State::Joining) {
			// Any error ends the attempt (the server closes the socket after it).
			end_with_failure(failed_code(code), msg);
			return;
		}
		if (fatal) {
			m_pendingError = code;
			m_pendingMessage = msg;
		}
		log_line("server refused: %s (%s)", code.c_str(), msg.c_str());
	}

	void on_joining(const std::string& t, const json& j) {
		if (t == "error") {
			on_error_message(j);
			return;
		}
		const bool created = t == "created";
		if (!(created && m_host) && !(t == "joined" && !m_host)) {
			protocol_error("unexpected " + t + " before the session started");
			return;
		}
		LobbyEvent e;
		e.kind = created ? LobbyEvent::Kind::Created : LobbyEvent::Kind::Joined;
		std::string secret;
		uint32_t member = 0;
		if (!get_string(j, "code", e.code) || !valid_code(e.code) || !get_string(j, "session", e.session) ||
		    !valid_session_id(e.session) || !get_u32(j, "member", member) || !get_string(j, "secret", secret) ||
		    !is_hex(secret, 8) || !parse_ice(j, e.ice) || !parse_relay(j, e.relay)) {
			protocol_error("malformed " + t);
			return;
		}
		e.self.id = member;
		e.self.auth = hex_u64(secret);
		if (created) {
			if (member != 0) {
				protocol_error("created with a member id other than 0");
				return;
			}
			e.self.name = m_cfg.name;
			LobbyMember host = e.self;
			host.auth = 0;
			e.members.push_back(host);
		} else {
			uint32_t host = 1;
			const json* list = field(j, "members");
			if (!get_u32(j, "host", host) || host != 0 || member == 0 || !list || !list->is_array() || list->size() > 64) {
				protocol_error("malformed joined");
				return;
			}
			bool sawSelf = false, sawHost = false;
			for (const json& m : *list) {
				LobbyMember lm;
				if (!m.is_object() || !get_u32(m, "member", lm.id) || !get_string(m, "name", lm.name) ||
				    !valid_text(lm.name, kMaxNameChars) || !get_bool(m, "spectator", lm.spectator)) {
					protocol_error("malformed member list");
					return;
				}
				if (std::any_of(e.members.begin(), e.members.end(), [&](const LobbyMember& o) { return o.id == lm.id; })) {
					protocol_error("duplicate member id");
					return;
				}
				sawHost |= lm.id == 0;
				if (lm.id == member) {
					sawSelf = true;
					e.self.name = lm.name;
					e.self.spectator = lm.spectator;
				}
				e.members.push_back(std::move(lm));
			}
			if (!sawSelf || !sawHost) {
				protocol_error("member list without the host or ourselves");
				return;
			}
			std::sort(e.members.begin(), e.members.end(), [](const LobbyMember& a, const LobbyMember& b) { return a.id < b.id; });
		}
		m_selfId = member;
		m_state = State::InSession;
		log_line("%s session %s", created ? "created" : "joined", e.code.c_str());
		m_events.push_back(std::move(e));
	}

	void on_session(const std::string& t, const json& j) {
		if (t == "pong") {
			return;
		}
		if (t == "error") {
			on_error_message(j);
			return;
		}
		if (t == "member_joined") {
			LobbyEvent e;
			e.kind = LobbyEvent::Kind::MemberJoined;
			if (!get_u32(j, "member", e.member.id) || e.member.id == 0 || e.member.id == m_selfId ||
			    !get_string(j, "name", e.member.name) || !valid_text(e.member.name, kMaxNameChars) ||
			    !get_bool(j, "spectator", e.member.spectator)) {
				protocol_error("malformed member_joined");
				return;
			}
			if (m_host) {
				std::string auth;
				if (!get_string(j, "auth", auth) || !is_hex(auth, 8)) {
					protocol_error("member_joined without auth");
					return;
				}
				e.member.auth = hex_u64(auth);
			}
			m_events.push_back(std::move(e));
			return;
		}
		if (t == "member_left") {
			LobbyEvent e;
			e.kind = LobbyEvent::Kind::MemberLeft;
			if (!get_u32(j, "member", e.member.id) || !get_string(j, "reason", e.message) ||
			    (e.message != "left" && e.message != "kicked" && e.message != "banned" && e.message != "timeout")) {
				protocol_error("malformed member_left");
				return;
			}
			m_events.push_back(std::move(e));
			return;
		}
		if (t == "signal") {
			LobbyEvent e;
			e.kind = LobbyEvent::Kind::Signal;
			if (!get_u32(j, "from", e.from) || !get_string(j, "kind", e.signalKind) || !valid_signal_kind(e.signalKind) ||
			    !get_string(j, "data", e.data) || e.data.size() > kMaxSignalData || e.from == m_selfId) {
				protocol_error("malformed signal");
				return;
			}
			if (!m_host && e.from != 0) {
				protocol_error("signal from a member other than the host");
				return;
			}
			m_events.push_back(std::move(e));
			return;
		}
		if (t == "closed") {
			std::string reason;
			if (!get_string(j, "reason", reason) ||
			    (reason != "host_left" && reason != "expired" && reason != "server_shutdown" && reason != "kicked" &&
			     reason != "banned")) {
				protocol_error("malformed closed");
				return;
			}
			close_session(reason, "");
			return;
		}
		if (t == "created" || t == "joined") {
			protocol_error("second " + t);
			return;
		}
		// Unknown message types are ignored (newer servers may add some).
	}

	LobbyConfig m_cfg;
	std::string m_url;
	bool m_plaintext = false;
	borealis::ws::Connection m_conn;
	State m_state = State::Idle;
	bool m_host = false;
	uint32_t m_selfId = 0;
	std::string m_first;
	std::string m_pendingError;
	std::string m_pendingMessage;
	Clock::time_point m_lastPing{};
	Clock::time_point m_lastRecv{};
	std::vector<LobbyEvent> m_events;
};

} // namespace

std::unique_ptr<LobbyClient> LobbyClient::create(const LobbyConfig& cfg, std::string& err) {
	const std::string& url = cfg.serverUrl;
	std::string scheme, rest;
	if (const size_t p = url.find("://"); p != std::string::npos) {
		scheme = url.substr(0, p);
		rest = url.substr(p + 3);
	}
	std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	bool plaintext;
	if (scheme == "https" || scheme == "wss") {
		plaintext = false;
	} else if (scheme == "http" || scheme == "ws") {
		plaintext = true;
	} else {
		err = "The server URL must start with https:// (or http:// for a server on this computer).";
		return nullptr;
	}
	if (rest.empty() || rest.size() > 512 ||
	    std::any_of(rest.begin(), rest.end(), [](char c) { return c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '@' || c == '\\'; })) {
		err = "The server URL is not valid.";
		return nullptr;
	}
	const size_t slash = rest.find('/');
	const std::string authority = rest.substr(0, slash);
	std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
	while (!path.empty() && path.back() == '/') {
		path.pop_back();
	}
	std::string host, port;
	if (!authority.empty() && authority[0] == '[') {
		const size_t close = authority.find(']');
		if (close == std::string::npos) {
			err = "The server URL is not valid.";
			return nullptr;
		}
		host = authority.substr(1, close - 1);
		const std::string after = authority.substr(close + 1);
		if (!after.empty()) {
			if (after[0] != ':' || !parse_port(after.substr(1))) {
				err = "The server URL has a bad port.";
				return nullptr;
			}
			port = after.substr(1);
		}
	} else {
		const size_t colon = authority.find(':');
		host = authority.substr(0, colon);
		if (colon != std::string::npos) {
			port = authority.substr(colon + 1);
			if (!parse_port(port)) {
				err = "The server URL has a bad port.";
				return nullptr;
			}
		}
	}
	if (host.empty()) {
		err = "The server URL has no host.";
		return nullptr;
	}
	if (plaintext && !is_loopback_host(host) && !cfg.allowPlaintext) {
		err = "Unencrypted (http://) servers are only allowed on this computer (127.0.0.1, ::1 or localhost); use https://.";
		return nullptr;
	}
	if (path.size() < 6 || path.compare(path.size() - 6, 6, "/v1/ws") != 0) {
		path += "/v1/ws";
	}
	const std::string wsUrl = std::string(plaintext ? "ws://" : "wss://") + authority + path;
	err.clear();
	return std::make_unique<LobbyClientImpl>(cfg, wsUrl, plaintext);
}

bool normalize_code(const std::string& in, std::string& out) {
	out.clear();
	if (in.size() > 64) {
		return false;
	}
	for (char raw : in) {
		char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
		if (c == ' ' || c == '-') {
			continue;
		}
		if (c == 'I' || c == 'L') {
			c = '1';
		} else if (c == 'O') {
			c = '0';
		}
		bool ok = false;
		for (const char* a = kCodeAlphabet; *a; ++a) {
			ok |= *a == c;
		}
		if (!ok || out.size() == 8) {
			out.clear();
			return false;
		}
		out.push_back(c);
	}
	if (out.size() != 8) {
		out.clear();
		return false;
	}
	return true;
}

} // namespace mppm::net
