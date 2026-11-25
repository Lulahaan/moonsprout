// M5.3b lobby client test (tests/net/lobby/lobby_test.sh runs it against a local mppm-server).
//   p2lobby_test http://127.0.0.1:<port> [closed-port]
// A host and two members (three LobbyClient objects driven from this one thread) go through create,
// join, member_joined (auth at the host only), signals both ways (one of 4096 bytes), a refused
// member-to-member signal, kick, host leave, a bad code and the URL rules. Prints one line per check
// and exits 0 only when every check passed.
//   p2lobby_test --relay http://127.0.0.1:<port> <relay ip:port> <stun url>
// Against a server started with --relay --stun <url>: the relay address, tokens and ICE list.
#include "lobby_client.hpp"

#include <borealis/task.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace mppm::net;
using Kind = LobbyEvent::Kind;
using Clock = std::chrono::steady_clock;

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string& what) {
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
	std::fflush(stdout);
	(ok ? g_passed : g_failed)++;
}

const char* kind_name(Kind k) {
	switch (k) {
	case Kind::Created: return "Created";
	case Kind::Joined: return "Joined";
	case Kind::Failed: return "Failed";
	case Kind::MemberJoined: return "MemberJoined";
	case Kind::MemberLeft: return "MemberLeft";
	case Kind::Signal: return "Signal";
	case Kind::Closed: return "Closed";
	}
	return "?";
}

struct Peer {
	std::string label;
	std::unique_ptr<LobbyClient> c;
	std::deque<LobbyEvent> q;
};

std::vector<Peer*> g_peers;

void pump_once() {
	for (Peer* p : g_peers) {
		if (!p->c) continue;
		std::vector<LobbyEvent> ev;
		p->c->poll(ev);
		for (auto& e : ev) {
			std::printf("  [%s] %s member=%u from=%u msg=%s err=%s\n", p->label.c_str(), kind_name(e.kind), e.member.id,
			            e.from, e.message.c_str(), e.error.c_str());
			p->q.push_back(std::move(e));
		}
	}
}

void pump_for(int ms) {
	const auto end = Clock::now() + std::chrono::milliseconds(ms);
	while (Clock::now() < end) {
		pump_once();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

// The first queued event of `kind` for `p` (other events stay queued), waiting up to `ms`.
std::optional<LobbyEvent> expect(Peer& p, Kind kind, int ms = 5000) {
	const auto end = Clock::now() + std::chrono::milliseconds(ms);
	for (;;) {
		for (auto it = p.q.begin(); it != p.q.end(); ++it) {
			if (it->kind == kind) {
				LobbyEvent e = std::move(*it);
				p.q.erase(it);
				return e;
			}
		}
		if (Clock::now() >= end) {
			return std::nullopt;
		}
		pump_once();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

bool has(Peer& p, Kind kind) {
	for (auto& e : p.q) {
		if (e.kind == kind) return true;
	}
	return false;
}

std::unique_ptr<LobbyClient> make(const std::string& url, const std::string& name, bool allowPlaintext = false) {
	LobbyConfig cfg;
	cfg.serverUrl = url;
	cfg.name = name;
	cfg.allowPlaintext = allowPlaintext;
	std::string err;
	auto c = LobbyClient::create(cfg, err);
	if (!c) std::printf("  create(%s): %s\n", url.c_str(), err.c_str());
	return c;
}

void test_codes() {
	std::string out;
	check(normalize_code("abcd-efgh", out) && out == "ABCDEFGH", "normalize_code: lower case and hyphen");
	check(normalize_code(" ilo0 1234 ", out) && out == "11001234", "normalize_code: I/L -> 1, O -> 0, spaces");
	check(!normalize_code("ABCD-EFGU", out), "normalize_code: U is invalid");
	check(!normalize_code("ABCD-EFG", out) && !normalize_code("ABCD-EFGHJ", out), "normalize_code: exactly 8 symbols");
	check(!normalize_code("ABCD_EFGH", out), "normalize_code: no other characters");
}

void test_urls() {
	std::string err;
	LobbyConfig cfg;
	cfg.serverUrl = "http://example.com:8080";
	check(!LobbyClient::create(cfg, err) && !err.empty(), "plaintext URL to a non-loopback host refused: " + err);
	cfg.serverUrl = "http://10.0.0.1";
	check(!LobbyClient::create(cfg, err), "plaintext URL to 10.0.0.1 refused");
	cfg.serverUrl = "ftp://127.0.0.1";
	check(!LobbyClient::create(cfg, err), "ftp:// refused");
	cfg.serverUrl = "https://user@example.com";
	check(!LobbyClient::create(cfg, err), "userinfo refused");
	cfg.serverUrl = "http://127.0.0.1:99999";
	check(!LobbyClient::create(cfg, err), "bad port refused");
	for (const char* ok : {"http://127.0.0.1:8080", "http://127.1.2.3", "http://localhost:1/", "http://[::1]:8080",
	                       "https://lobby.example.org", "https://lobby.example.org:8443/prefix/"}) {
		cfg.serverUrl = ok;
		check(LobbyClient::create(cfg, err) != nullptr, std::string("URL accepted: ") + ok);
	}
	cfg.serverUrl = "http://example.com:8080";
	cfg.allowPlaintext = true;
	check(LobbyClient::create(cfg, err) != nullptr, "plaintext non-loopback URL accepted with allowPlaintext");
}

int relay_mode(const std::string& url, const std::string& relayAddr, const std::string& stun) {
	Peer host{"host", make(url, "Olimar"), {}};
	Peer a{"m1", make(url, "Louie"), {}};
	g_peers = {&host, &a};
	host.c->create_session("", 4);
	auto created = expect(host, Kind::Created);
	check(created.has_value(), "relay: host Created");
	if (!created) return 1;
	auto nonzero = [](const RelayInfo& r) {
		for (uint8_t x : r.token) if (x) return true;
		return false;
	};
	check(created->relay.ipPort == relayAddr && nonzero(created->relay), "relay: Created carries relay " + created->relay.ipPort);
	check(created->ice.size() == 1 && created->ice[0].url == stun && created->ice[0].username.empty(), "relay: ICE list = the STUN URL");
	a.c->join_session(created->code, "", false);
	auto joined = expect(a, Kind::Joined);
	check(joined && joined->relay.ipPort == relayAddr && nonzero(joined->relay) &&
	          std::memcmp(joined->relay.token, created->relay.token, 16) != 0,
	      "relay: Joined carries its own relay token");
	host.c->leave();
	auto closed = expect(a, Kind::Closed);
	check(closed && closed->message == "host_left", "relay: m1 Closed host_left");
	g_peers.clear();
	host.c.reset();
	a.c.reset();
	borealis::shutdown();
	std::printf("%s: %d passed, %d failed\n", g_failed ? "FAIL" : "PASS", g_passed, g_failed);
	return g_failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc == 5 && std::string(argv[1]) == "--relay") {
		return relay_mode(argv[2], argv[3], argv[4]);
	}
	if (argc < 2) {
		std::fprintf(stderr, "usage: p2lobby_test http://127.0.0.1:<port> [closed-port]\n");
		return 2;
	}
	const std::string url = argv[1];
	const std::string closedPort = argc > 2 ? argv[2] : "1";

	test_codes();
	test_urls();

	Peer host{"host", make(url, "Olimar"), {}};
	Peer a{"m1", make(url, "Louie"), {}};
	Peer b{"m2", make(url, "  President\x07  "), {}};
	g_peers = {&host, &a, &b};
	check(host.c && a.c && b.c, "clients created for " + url);
	if (!host.c || !a.c || !b.c) return 1;

	// Create.
	host.c->create_session("", 8);
	auto created = expect(host, Kind::Created);
	check(created.has_value(), "host: Created");
	if (!created) return 1;
	std::string canon;
	check(created->code.size() == 9 && created->code[4] == '-' && normalize_code(created->code, canon), "Created: code " + created->code);
	check(created->self.id == 0 && created->self.auth != 0 && !created->session.empty(), "Created: member 0, secret, session id");
	check(created->relay.ipPort.empty() && created->ice.empty(), "Created: no relay or ICE servers in mode none");
	check(host.c->connected(), "host: connected()");

	// Join (typed in lower case without the hyphen).
	std::string typed = canon;
	for (char& ch : typed) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	a.c->join_session(typed, "", false);
	auto joinedA = expect(a, Kind::Joined);
	check(joinedA.has_value(), "m1: Joined");
	if (!joinedA) return 1;
	check(joinedA->self.id == 1 && joinedA->self.auth != 0 && joinedA->code == created->code, "Joined: member 1 with a secret");
	check(joinedA->members.size() == 2 && joinedA->members[0].id == 0 && joinedA->members[0].name == "Olimar" &&
	          joinedA->members[1].id == 1 && joinedA->members[1].name == "Louie",
	      "Joined: member list host first");
	check(joinedA->members[0].auth == 0 && joinedA->members[1].auth == 0, "Joined: no auth values in the list");
	auto mjA = expect(host, Kind::MemberJoined);
	check(mjA && mjA->member.id == 1 && mjA->member.name == "Louie" && mjA->member.auth == joinedA->self.auth,
	      "host: MemberJoined(1) with auth = m1's secret");

	b.c->join_session(created->code, "", true);
	auto joinedB = expect(b, Kind::Joined);
	check(joinedB && joinedB->self.id == 2 && joinedB->self.spectator && joinedB->self.name == "President" &&
	          joinedB->members.size() == 3,
	      "m2: Joined as spectator 2 (name sanitized)");
	auto mjB = expect(host, Kind::MemberJoined);
	check(mjB && mjB->member.id == 2 && joinedB && mjB->member.auth == joinedB->self.auth && mjB->member.spectator,
	      "host: MemberJoined(2) with auth");
	auto mjB1 = expect(a, Kind::MemberJoined);
	check(mjB1 && mjB1->member.id == 2 && mjB1->member.auth == 0, "m1: MemberJoined(2) without auth");

	// Signals both ways.
	host.c->signal(1, "desc", "hello m1");
	auto s1 = expect(a, Kind::Signal);
	check(s1 && s1->from == 0 && s1->signalKind == "desc" && s1->data == "hello m1", "m1: Signal from the host");
	const std::string big(4096, 'x');
	a.c->signal(0, "cand", big);
	auto s2 = expect(host, Kind::Signal);
	check(s2 && s2->from == 1 && s2->signalKind == "cand" && s2->data == big, "host: 4096-byte Signal from m1");
	host.c->signal(2, "addr", big);
	auto s3 = expect(b, Kind::Signal);
	check(s3 && s3->from == 0 && s3->data.size() == 4096, "m2: 4096-byte Signal from the host");
	a.c->signal(0, "cand", big + "y");
	pump_for(200);
	check(!has(host, Kind::Signal), "a 4097-byte signal is not sent");

	// Member to member: refused by the server, the sender stays connected.
	a.c->signal(2, "addr", "sneaky");
	pump_for(500);
	check(!has(b, Kind::Signal), "member-to-member signal refused (m2 got nothing)");
	check(a.c->connected() && !has(a, Kind::Closed), "m1 still connected after the refusal");
	a.c->signal(0, "done", "ok");
	auto s4 = expect(host, Kind::Signal);
	check(s4 && s4->from == 1 && s4->signalKind == "done", "host: Signal after the refusal");

	// Kick m2.
	host.c->kick(2, false);
	auto kicked = expect(b, Kind::Closed);
	check(kicked && kicked->message == "kicked", "m2: Closed kicked");
	check(!b.c->connected(), "m2: not connected after the kick");
	auto leftA = expect(a, Kind::MemberLeft);
	check(leftA && leftA->member.id == 2 && leftA->message == "kicked", "m1: MemberLeft(2) kicked");
	auto leftH = expect(host, Kind::MemberLeft);
	check(leftH && leftH->member.id == 2 && leftH->message == "kicked", "host: MemberLeft(2) kicked");

	// Host leaves.
	host.c->leave();
	auto closedA = expect(a, Kind::Closed);
	check(closedA && closedA->message == "host_left", "m1: Closed host_left");
	check(!host.c->connected(), "host: not connected after leave");

	// Bad codes.
	Peer c{"m3", make(url, "Late"), {}};
	g_peers.push_back(&c);
	c.c->join_session(created->code, "", false);
	auto nf = expect(c, Kind::Failed);
	check(nf && nf->error == "not_found", "join of the ended session: Failed not_found");
	Peer d{"m4", make(url, "Typo"), {}};
	g_peers.push_back(&d);
	d.c->join_session("ABCD-EFGU", "", false);
	auto bad = expect(d, Kind::Failed);
	check(bad && bad->error == "not_found", "invalid code: Failed not_found");

	// Unreachable server.
	Peer e{"m5", make("http://127.0.0.1:" + closedPort, "Nobody"), {}};
	g_peers.push_back(&e);
	e.c->join_session(created->code, "", false);
	auto un = expect(e, Kind::Failed, 8000);
	check(un && un->error == "unreachable", "closed port: Failed unreachable");

	// Signals when not connected are dropped (log line only).
	c.c->signal(0, "desc", "x");
	pump_for(50);
	check(c.q.empty(), "signal while not connected: dropped without an event");

	g_peers.clear();
	host.c.reset();
	a.c.reset();
	b.c.reset();
	c.c.reset();
	d.c.reset();
	e.c.reset();
	borealis::shutdown();
	std::printf("%s: %d passed, %d failed\n", g_failed ? "FAIL" : "PASS", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
