// M5.3b lobby client: one WebSocket to mppm-server (borealis::ws) for create and join, the member
// list, signalling between the host and each member, and the relay and ICE server information
// (docs/online/M5_3B_NETCODE.md; the wire protocol is server/PROTOCOL.md).
//
// Owner: the server package (step 0 file; only the owner changes it, through the coordinator).
// Threading: not thread-safe. Every member is called from one thread, the net thread of
// port/net/session. Never blocks: connect, send and close run in borealis::ws's own thread.
#pragma once

#include "transport.hpp" // IceServer

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mppm::net {

struct RelayInfo {                 // mppm-server --relay-mode builtin; empty ipPort = no relay
    std::string ipPort;            // relay control address, e.g. "127.0.0.1:47101"
    uint8_t token[16] = {};        // this member's relay token
};

struct LobbyMember {
    uint32_t id = 0;               // member id: 0 = the host, then 1, 2, ... in join order (never reused in a session)
    std::string name;              // as sanitized by the server: 1..24 printable characters
    bool spectator = false;
    uint64_t auth = 0;             // host's view: the secret this member presents in its Hello; 0 elsewhere
};

struct LobbyEvent {
    enum class Kind : uint8_t {
        Created,       // create_session succeeded (host): code, session, self, ice, relay
        Joined,        // join_session succeeded: code, session, self, members (host first), ice, relay
        Failed,        // create/join refused, or the server could not be reached: error, message
        MemberJoined,  // host and members: `member` (auth set for the host only)
        MemberLeft,    // `member.id`; message = "left" | "kicked" | "banned" | "timeout"
        Signal,        // from, signalKind, data
        Closed,        // the session or the connection ended: message = "host_left" | "expired" |
                       // "server_shutdown" | "connection_lost" | "kicked" | "banned"
    };
    Kind kind = Kind::Failed;
    std::string code;              // "ABCD-EFGH" (Created, Joined)
    std::string session;           // opaque session id
    LobbyMember self;              // Created / Joined: our member; self.auth = our own secret (put it in the Hello)
    LobbyMember member;            // MemberJoined / MemberLeft
    std::vector<LobbyMember> members;
    std::vector<IceServer> ice;    // may be empty
    RelayInfo relay;
    uint32_t from = 0;             // Signal: sender's member id
    std::string signalKind;        // Signal: "desc" | "cand" | "done" | "addr"
    std::string data;              // Signal: payload, at most 4096 bytes
    std::string error;             // Failed: "unreachable" | "not_found" | "full" | "bad_password" |
                                   // "rate_limited" | "version" | "protocol" | "bad_request"
    std::string message;           // human-readable detail (English, one sentence)
};

struct LobbyConfig {
    std::string serverUrl;         // "https://host[:port]" (wss://host/v1/ws) or "http://host:port" (ws://);
                                   // plaintext only when the host is 127.0.0.0/8, ::1 or localhost, or allowPlaintext
    bool allowPlaintext = false;   // --net-allow-plaintext / MPPM_NET_ALLOW_PLAINTEXT=1
    std::string name;              // player name (the server sanitizes it)
};

class LobbyClient {
public:
    // Checks the URL; does not connect yet. nullptr and `err` for a bad or plaintext non-loopback URL.
    static std::unique_ptr<LobbyClient> create(const LobbyConfig& cfg, std::string& err);
    virtual ~LobbyClient() = default; // closes the WebSocket (a host closes its session)

    // One of these, once: opens the WebSocket and sends "create" / "join". Created, Joined or Failed follows.
    virtual void create_session(const std::string& password, int maxMembers) = 0;
    virtual void join_session(const std::string& code, const std::string& password, bool spectator) = 0;
    // To one member (a member may only signal the host, member 0).
    virtual void signal(uint32_t to, const std::string& kind, const std::string& data) = 0;
    virtual void kick(uint32_t member, bool ban) = 0;   // host only
    virtual void leave() = 0;                            // host: closes the session; member: leaves it
    // Non-blocking: appends the events since the last call (also answers the server's keepalive).
    virtual void poll(std::vector<LobbyEvent>& out) = 0;
    virtual bool connected() const = 0;                  // the WebSocket is open and the session joined
};

// Join codes: 8 Crockford base32 symbols (40 bits), shown "ABCD-EFGH". Accepts any case, spaces and
// hyphens; I and L read as 1, O as 0; U is invalid. Writes the 8 canonical symbols (no hyphen).
bool normalize_code(const std::string& in, std::string& out);

} // namespace mppm::net
