// M5.3b transport (docs/online/M5_3B_TRANSPORT.md): ENet 1.3.18 over a socket shim that reaches each
// peer directly (plain UDP), through ICE (libjuice 1.7.4, synthetic 240.0.0.0/4 addresses) or through
// the builtin relay of mppm-server, with an optional in-process impairment stage (tests only).
//
// Owner: the transport package (step 0 file; only the owner changes it, through the coordinator).
// Threading: not thread-safe. Every member except wake() is called from one thread, the net thread of
// port/net/session. No game headers, no Aurora, no SDL.
// Clock: every timestamp is std::chrono::steady_clock in nanoseconds (Transport::now_ns()), the clock of
// the retrace grid (port/sdk/os_thread.cpp p2_vi_retrace_time_ns).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mppm::net {

// Largest payload one send() accepts (MTU-safe through TURN and the relay).
constexpr size_t kMaxDatagram = 1200;

// Test-only impairment of every datagram this endpoint sends, applied between ENet and the socket
// (or the ICE agent). The receiver needs no setting: impair both ends to impair both directions.
struct Impair {
    uint32_t delayUs  = 0;  // added one-way delay
    uint32_t jitterUs = 0;  // plus a uniform 0..jitterUs per datagram (may reorder datagrams)
    uint32_t lossPpm  = 0;  // dropped datagrams, parts per million (2% = 20000)
    uint32_t dupPpm   = 0;  // duplicated datagrams, parts per million
    uint32_t seed     = 1;  // xorshift32 seed; 0 is replaced by 1
    bool enabled() const { return delayUs != 0 || jitterUs != 0 || lossPpm != 0 || dupPpm != 0; }
};
// Parses "delay=46ms,jitter=8ms,loss=2%,dup=0.1%,seed=7" (keys in any order, each optional; times in
// us, ms or s; rates in % or ppm). "" and "off" give no impairment. False plus a message otherwise.
bool parse_impair(const std::string& text, Impair& out, std::string& err);

// One entry of mppm-server's "ice" list, WebRTC style.
struct IceServer {
    std::string url;         // "stun:host:port" or "turn:host:port" (UDP; "?transport=udp" accepted)
    std::string username;    // TURN only
    std::string credential;  // TURN only
};

struct TransportConfig {
    bool listen            = false;      // the session host: accepts incoming ENet connections
    uint16_t maxPeers      = 8;          // ENet peer slots (host: clients + spectators; client: 2)
    std::string bindAddress = "0.0.0.0"; // UDP socket and ICE agents; tests use "127.0.0.1"
    uint16_t port          = 0;          // UDP port of the ENet socket, 0 = any free port
    Impair impair;                       // test only (MPPM_NET_IMPAIR / --net-impair)
    std::vector<IceServer> iceServers;   // STUN/TURN for ice_create
    bool iceRelayOnly      = false;      // test only: signal and accept relay candidates only (forces TURN)
    uint32_t timeoutMinMs  = 5000;       // enet_peer_timeout(peer, 32, timeoutMinMs, timeoutMaxMs)
    uint32_t timeoutMaxMs  = 10000;
};

using PeerId = uint32_t;                 // 1, 2, ...; never reused by one Transport; 0 = none
constexpr PeerId kNoPeer = 0;

// Channel 0: reliable and ordered. Channel 1: unreliable and unsequenced, packet throttling off.
enum class Channel : uint8_t { Control = 0, Input = 1 };
enum class Link : uint8_t { Direct = 0, Ice = 1, Relay = 2 };

struct Event {
    enum class Kind : uint8_t {
        Connected,        // the ENet connection is up (both sides); `connectData` is the connecting side's value
        Disconnected,     // the peer is gone: remote disconnect, timeout, ICE failure; `text` = reason
        Received,         // one datagram on `channel` in `data`; `recvNs` set
        IceCandidate,     // `text` = one local candidate line ("a=candidate:..."), to signal to the remote side
        IceGatheringDone, // no more local candidates for `peer`
        IceState,         // `text` = "connected" or "failed"
        RelayRegistered,  // relay_register succeeded: `text` = "host" or "<relay ip>:<allocated port>",
                          // `aux` = this socket's public "ip:port" as the relay saw it
        RelayFailed,      // relay_register gave up: `text` = reason
    };
    Kind kind            = Kind::Received;
    PeerId peer          = kNoPeer;      // kNoPeer for RelayRegistered / RelayFailed
    Channel channel      = Channel::Control;
    uint32_t connectData = 0;            // Connected
    uint64_t recvNs      = 0;            // Received: steady ns when the transport took it off the socket/agent
    std::vector<uint8_t> data;           // Received
    std::string text;
    std::string aux;
};

struct PeerStats {
    Link link = Link::Direct;
    std::string remote;          // "127.0.0.1:40123", "ice 127.0.0.1:52011", "relay 127.0.0.1:47201"
    uint32_t rttUs     = 0;      // ENet mean round-trip time
    uint32_t rttVarUs  = 0;
    uint32_t lossPpm   = 0;      // ENet packet-loss estimate
    uint64_t bytesSent = 0, bytesReceived = 0;          // payload bytes through send()/Received
    uint64_t datagramsSent = 0, datagramsReceived = 0;  // UDP datagrams at the shim, ENet's own included
    uint64_t impairDropped = 0;  // datagrams dropped by the impairment stage
};

class Transport {
public:
    // Opens the UDP socket (bindAddress:port) and the ENet host. nullptr and `err` on failure.
    static std::unique_ptr<Transport> create(const TransportConfig& cfg, std::string& err);
    // steady_clock now in ns.
    static uint64_t now_ns();
    virtual ~Transport() = default;  // resets every peer, destroys ICE agents, closes the socket

    // "ip:port" of the ENet UDP socket as bound, and the addresses a LAN peer can reach it at:
    // the bound address when it is not 0.0.0.0, else every up non-loopback IPv4 interface, then
    // 127.0.0.1 (each as "ip:port").
    virtual std::string local_address() const = 0;
    virtual std::vector<std::string> local_addresses() const = 0;

    // ---- Direct and relayed peers: plain UDP from the ENet socket.
    // Starts an ENet connection to "ip:port"; Connected (or Disconnected after the ENet connect
    // timeout) follows. `connectData` reaches the accepting side's Connected event.
    virtual PeerId connect(const std::string& ipPort, uint32_t connectData) = 0;
    // Sends `count` MPRL PUNCH datagrams to "ip:port" from the ENet socket, 20 ms apart (NAT hole punching).
    virtual void punch(const std::string& ipPort, int count) = 0;
    // Builtin relay (server/PROTOCOL.md "Relay"): registers the ENet socket with the relay control
    // address `relayIpPort` using this member's 16-byte token. Retries every 250 ms up to 5 s, then
    // RelayFailed; after RelayRegistered it repeats the registration every 15 s to keep it alive.
    virtual void relay_register(const std::string& relayIpPort, const uint8_t token[16]) = 0;

    // ---- ICE (libjuice). The host has one agent per client, all on one UDP port (mux mode).
    // Creates an agent for one remote member. The client side is controlling; once IceState
    // "connected" arrives, the controlling side's transport starts the ENet connect over the agent
    // itself (with `connectData`), and Connected follows on both sides.
    virtual PeerId ice_create(bool controlling, uint32_t connectData) = 0;
    virtual std::string ice_local_description(PeerId peer) = 0;  // SDP (without filtered candidates)
    virtual bool ice_set_remote_description(PeerId peer, const std::string& sdp) = 0;
    virtual bool ice_add_remote_candidate(PeerId peer, const std::string& candidateLine) = 0;
    virtual void ice_set_remote_gathering_done(PeerId peer) = 0;
    virtual bool ice_gather(PeerId peer) = 0;  // IceCandidate events follow, then IceGatheringDone

    // ---- Traffic.
    // Queues one datagram (size <= kMaxDatagram). flush: enet_host_flush at once (inputs, bundles).
    // False when the peer is unknown or not connected, or the size is 0 or too large.
    virtual bool send(PeerId peer, Channel channel, const uint8_t* data, size_t size, bool flush) = 0;
    virtual void disconnect(PeerId peer, uint32_t reason) = 0;  // graceful; Disconnected follows
    virtual void reset(PeerId peer) = 0;                         // immediate, no event

    // Waits up to timeoutUs (0 = do not wait) for socket data, ICE data, a due impaired datagram or
    // wake(); then releases due impaired datagrams, services ENet until it has no event left, and
    // appends every event to `out` (in arrival order).
    virtual void service(uint32_t timeoutUs, std::vector<Event>& out) = 0;
    // Any thread: a service() that waits returns early.
    virtual void wake() = 0;
    virtual bool stats(PeerId peer, PeerStats& out) const = 0;
};

} // namespace mppm::net
