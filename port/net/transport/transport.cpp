// M5.3b transport (docs/online/M5_3B_TRANSPORT.md): mppm::net::Transport over ENet 1.3.18, the socket
// shim (enet_juice.c), libjuice 1.7.4 agents and the builtin relay's client side. The interface and
// every member's semantics are in port/net/transport.hpp. Host C++ only: no game headers.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include "../transport.hpp"

#include "enet_juice.h"
#include "mprl.h"

#include <juice/juice.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>

namespace mppm::net {

namespace {

constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kRelayRetryNs = 250 * kMs;
constexpr uint64_t kRelayGiveUpNs = 5000 * kMs;
constexpr uint64_t kRelayRefreshNs = 15000 * kMs;
constexpr uint64_t kPunchSpacingNs = 20 * kMs;

std::once_flag g_init;

void juice_log(juice_log_level_t level, const char* message) {
    if (level >= JUICE_LOG_LEVEL_WARN) std::fprintf(stderr, "mppm: net: juice: %s\n", message);
}

void global_init() {
    enet_initialize();
    juice_set_log_handler(juice_log);
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);
}

std::string ip_str(uint32_t hostNetOrder) {
    char ip[INET_ADDRSTRLEN] = {};
    in_addr in{};
    in.s_addr = hostNetOrder;
    inet_ntop(AF_INET, &in, ip, sizeof ip);
    return ip;
}

std::string addr_str(const ENetAddress& a) { return ip_str(a.host) + ":" + std::to_string(a.port); }

bool parse_ip(const std::string& s, uint32_t& out) {
    in_addr in{};
    if (inet_pton(AF_INET, s.c_str(), &in) != 1) return false;
    out = in.s_addr;
    return true;
}

// Numeric "a.b.c.d:port" only (no name lookups on the net thread).
bool parse_addr(const std::string& s, ENetAddress& out) {
    size_t c = s.rfind(':');
    if (c == std::string::npos || c == 0 || c + 1 >= s.size()) return false;
    std::string port = s.substr(c + 1);
    if (port.find_first_not_of("0123456789") != std::string::npos || port.size() > 5) return false;
    unsigned long p = std::strtoul(port.c_str(), nullptr, 10);
    if (p == 0 || p > 65535) return false;
    if (!parse_ip(s.substr(0, c), out.host)) return false;
    out.port = static_cast<enet_uint16>(p);
    return true;
}

bool same_addr(const ENetAddress& a, const ENetAddress& b) { return a.host == b.host && a.port == b.port; }

// "stun:host:port", "turn:host:port?transport=udp": host and port (default 3478). False for other
// schemes and for TCP or TLS transports, which libjuice does not do.
bool parse_ice_url(const std::string& url, const char* scheme, std::string& host, uint16_t& port) {
    std::string pre = std::string(scheme) + ":";
    if (url.compare(0, pre.size(), pre) != 0) return false;
    std::string rest = url.substr(pre.size());
    size_t q = rest.find('?');
    if (q != std::string::npos) {
        std::string query = rest.substr(q + 1);
        rest = rest.substr(0, q);
        if (query.find("transport=") != std::string::npos && query.find("transport=udp") == std::string::npos) return false;
    }
    port = 3478;
    size_t c = rest.rfind(':');
    if (c != std::string::npos && rest.find(']') == std::string::npos) {
        unsigned long p = std::strtoul(rest.substr(c + 1).c_str(), nullptr, 10);
        if (p == 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
        rest = rest.substr(0, c);
    }
    if (rest.empty()) return false;
    host = rest;
    return true;
}

// The candidate type of an SDP candidate line ("... typ host ..."), or "?".
std::string candidate_type(const std::string& line) {
    size_t t = line.find(" typ ");
    if (t == std::string::npos) return "?";
    size_t b = t + 5, e = line.find(' ', b);
    return line.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

bool is_candidate_line(const std::string& line) { return line.rfind("a=candidate:", 0) == 0 || line.rfind("candidate:", 0) == 0; }

// Removes the a=candidate lines whose type is not relay (iceRelayOnly).
std::string strip_non_relay(const std::string& sdp) {
    std::string out;
    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t e = sdp.find('\n', pos);
        std::string line = sdp.substr(pos, e == std::string::npos ? std::string::npos : e - pos + 1);
        pos = e == std::string::npos ? sdp.size() : e + 1;
        std::string bare = line;
        while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r')) bare.pop_back();
        if (is_candidate_line(bare) && candidate_type(bare) != "relay") continue;
        out += line;
    }
    return out;
}

// A free UDP port on `bindIp` (host network order), for the host's mux agents. 0 on failure.
uint16_t pick_udp_port(uint32_t bindIp) {
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
#endif
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = bindIp;
    sin.sin_port = 0;
    uint16_t port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&sin), sizeof sin) == 0) {
        socklen_t len = sizeof sin;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&sin), &len) == 0) port = ntohs(sin.sin_port);
    }
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
    return port;
}

std::vector<uint32_t> interface_ips() { // up, non-loopback IPv4 interfaces, host network order
    std::vector<uint32_t> ips;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return ips;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
        }
    }
    return ips;
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return ips;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t ip = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
        if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
    }
    freeifaddrs(list);
    return ips;
#endif
}

bool is_loopback(uint32_t hostNetOrder) { return (reinterpret_cast<const uint8_t*>(&hostNetOrder)[0]) == 127; }

class TransportImpl;

struct AgentCtx { // libjuice user pointer; lives until juice_destroy has returned
    TransportImpl* t;
    PeerId peer;
    int agentId;
};

struct CbItem { // what a libjuice callback queued for the net thread
    enum class Kind : uint8_t { State, Candidate, GatheringDone } kind;
    PeerId peer;
    juice_state_t state;
    std::string text;
};

struct PeerRec {
    PeerId id = kNoPeer;
    Link link = Link::Direct;
    ENetPeer* enet = nullptr;
    ENetAddress addr{};          // the ENet address (synthetic for ICE)
    bool connected = false;
    bool outgoing = false;
    uint32_t connectData = 0;
    bool localDisconnect = false;
    uint32_t localReason = 0;
    juice_agent_t* agent = nullptr;
    AgentCtx* ctx = nullptr;
    int agentId = 0;
    bool controlling = false;
    bool iceConnected = false;
    bool iceFailed = false;
    std::string iceRemote;       // selected remote address, "ip:port"
    uint64_t bytesSent = 0, bytesReceived = 0;
};

class TransportImpl final : public Transport {
public:
    TransportImpl() = default;
    ~TransportImpl() override;

    bool init(const TransportConfig& cfg, std::string& err);

    std::string local_address() const override { return addr_str(bound_); }
    std::vector<std::string> local_addresses() const override;
    PeerId connect(const std::string& ipPort, uint32_t connectData) override;
    void punch(const std::string& ipPort, int count) override;
    void relay_register(const std::string& relayIpPort, const uint8_t token[16]) override;
    PeerId ice_create(bool controlling, uint32_t connectData) override;
    std::string ice_local_description(PeerId peer) override;
    bool ice_set_remote_description(PeerId peer, const std::string& sdp) override;
    bool ice_add_remote_candidate(PeerId peer, const std::string& candidateLine) override;
    void ice_set_remote_gathering_done(PeerId peer) override;
    bool ice_gather(PeerId peer) override;
    bool send(PeerId peer, Channel channel, const uint8_t* data, size_t size, bool flush) override;
    void disconnect(PeerId peer, uint32_t reason) override;
    void reset(PeerId peer) override;
    void service(uint32_t timeoutUs, std::vector<Event>& out) override;
    void wake() override;
    bool stats(PeerId peer, PeerStats& out) const override;

    // libjuice callbacks (libjuice's thread).
    void queue_cb(CbItem item);
    void agent_recv(int agentId, const char* data, size_t size) { mppm_shim_agent_recv(shim_, agentId, data, size); }

private:
    PeerRec* find(PeerId id);
    PeerRec* find_agent(int agentId);
    void configure_new(ENetPeer* p);
    void configure_connected(ENetPeer* p);
    void destroy_agent(PeerRec& r);
    void erase(PeerId id);
    void emit(Event::Kind kind, PeerId peer, std::string text = {}, std::string aux = {});
    void handle_enet(const ENetEvent& e);
    void process_callbacks();
    void process_mprl(uint64_t now);
    void run_timers(uint64_t now);
    uint64_t next_timer_ns() const;
    bool relay_link(const ENetAddress& a) const;
    juice_agent_t* make_agent(AgentCtx* ctx, bool retryPort);

    TransportConfig cfg_;
    ENetHost* host_ = nullptr;
    mppm_shim* shim_ = nullptr;
#if defined(_WIN32)
    // A UDP socket on 127.0.0.1 connected to itself: wake() sends it one byte, and service() waits on
    // it next to the ENet socket.
    SOCKET wakeSock_ = INVALID_SOCKET;
#else
    int wakeRead_ = -1, wakeWrite_ = -1;
    bool wakeIsPipe_ = false;
#endif
    ENetAddress bound_{};
    std::vector<uint32_t> localIps_;
    std::map<PeerId, PeerRec> peers_;
    PeerId nextId_ = 1;
    int nextAgentId_ = 1;
    std::vector<Event> pending_;
    std::mutex cbLock_;
    std::deque<CbItem> cbQueue_;

    // ICE servers (kept alive for every juice_create).
    std::string stunHost_;
    uint16_t stunPort_ = 0;
    std::vector<std::string> turnHosts_, turnUsers_, turnPasses_;
    std::vector<juice_turn_server_t> turn_;
    uint16_t muxPort_ = 0;

    struct Relay {
        bool active = false, registered = false;
        ENetAddress ctrl{};
        uint8_t token[16] = {};
        uint64_t deadline = 0, nextSend = 0;
        uint8_t role = 0;
        std::string text, aux;
        ENetAddress allocated{};
        bool hasAllocated = false;
    } relay_;

    struct Punch {
        ENetAddress to;
        int remaining;
        uint64_t next;
    };
    std::vector<Punch> punches_;
    std::mt19937_64 rng_{std::random_device{}()};
};

// ---- libjuice callbacks: queue and wake only.

void cb_state(juice_agent_t*, juice_state_t state, void* user) {
    auto* c = static_cast<AgentCtx*>(user);
    c->t->queue_cb({CbItem::Kind::State, c->peer, state, {}});
}
void cb_candidate(juice_agent_t*, const char* sdp, void* user) {
    auto* c = static_cast<AgentCtx*>(user);
    c->t->queue_cb({CbItem::Kind::Candidate, c->peer, JUICE_STATE_DISCONNECTED, sdp ? sdp : ""});
}
void cb_gathering_done(juice_agent_t*, void* user) {
    auto* c = static_cast<AgentCtx*>(user);
    c->t->queue_cb({CbItem::Kind::GatheringDone, c->peer, JUICE_STATE_DISCONNECTED, {}});
}
void cb_recv(juice_agent_t*, const char* data, size_t size, void* user) {
    auto* c = static_cast<AgentCtx*>(user);
    c->t->agent_recv(c->agentId, data, size);
}

// ---- Setup and teardown.

bool TransportImpl::init(const TransportConfig& cfg, std::string& err) {
    cfg_ = cfg;
    if (cfg_.maxPeers == 0) cfg_.maxPeers = 1;
    ENetAddress bind{};
    if (!parse_ip(cfg_.bindAddress, bind.host)) {
        err = "bad bind address '" + cfg_.bindAddress + "' (numeric IPv4)";
        return false;
    }
    bind.port = cfg_.port;
    host_ = enet_host_create(&bind, cfg_.maxPeers, 2, 0, 0);
    if (!host_) {
        err = "cannot open UDP " + addr_str(bind) + ": " + std::strerror(errno);
        return false;
    }
    // Section 10: a fixed maximum packet size. Every message is <= kMaxDatagram (send() enforces
    // it), so ENet refuses anything larger before buffering it (its default is 32 MiB per packet),
    // and a peer may keep at most 64 KiB waiting for dispatch. duplicatePeers stays at maxPeers:
    // every relayed peer arrives from the relay's address and local tests share 127.0.0.1.
    host_->maximumPacketSize = kMaxDatagram;
    host_->maximumWaitingData = 64 * 1024;
    host_->duplicatePeers = cfg_.maxPeers;
    if (enet_socket_get_address(host_->socket, &bound_) != 0) bound_ = bind;
    if (bound_.host == 0) bound_.host = bind.host;

#if defined(_WIN32)
    wakeSock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int selfLen = sizeof self;
    u_long nonBlocking = 1;
    if (wakeSock_ == INVALID_SOCKET || ::bind(wakeSock_, reinterpret_cast<sockaddr*>(&self), sizeof self) != 0 ||
        ::getsockname(wakeSock_, reinterpret_cast<sockaddr*>(&self), &selfLen) != 0 ||
        ::connect(wakeSock_, reinterpret_cast<sockaddr*>(&self), sizeof self) != 0 ||
        ::ioctlsocket(wakeSock_, FIONBIO, &nonBlocking) != 0) {
        err = "cannot open the wake-up socket (WSA error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
#elif defined(__linux__)
    wakeRead_ = wakeWrite_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeRead_ < 0) {
        err = std::string("eventfd: ") + std::strerror(errno);
        return false;
    }
#else
    int fds[2];
    if (pipe(fds) != 0) {
        err = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    for (int fd : fds) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wakeRead_ = fds[0];
    wakeWrite_ = fds[1];
    wakeIsPipe_ = true;
#endif
    shim_ = mppm_shim_attach(host_->socket, [](void* self) { static_cast<TransportImpl*>(self)->wake(); }, this);
    if (!shim_) {
        err = "too many transports in this process (8)";
        return false;
    }
    if (cfg_.impair.enabled()) {
        mppm_impair im;
        mppm_impair_init(&im, cfg_.impair.delayUs, cfg_.impair.jitterUs, cfg_.impair.lossPpm, cfg_.impair.dupPpm,
                         cfg_.impair.seed ? cfg_.impair.seed : 1);
        mppm_shim_set_impair(shim_, &im);
        std::fprintf(stderr, "mppm: net: impairment delay_us=%u jitter_us=%u loss_ppm=%u dup_ppm=%u seed=%u (test only)\n",
                     cfg_.impair.delayUs, cfg_.impair.jitterUs, cfg_.impair.lossPpm, cfg_.impair.dupPpm,
                     cfg_.impair.seed ? cfg_.impair.seed : 1);
    }

    localIps_ = interface_ips();
    for (const IceServer& s : cfg_.iceServers) {
        std::string h;
        uint16_t p;
        if (stunHost_.empty() && parse_ice_url(s.url, "stun", h, p)) {
            stunHost_ = h;
            stunPort_ = p;
        } else if (parse_ice_url(s.url, "turn", h, p)) {
            turnHosts_.push_back(h);
            turnUsers_.push_back(s.username);
            turnPasses_.push_back(s.credential);
            turn_.push_back({nullptr, nullptr, nullptr, p});
        }
    }
    for (size_t i = 0; i < turn_.size(); i++) { // pointers after the vectors stopped growing
        turn_[i].host = turnHosts_[i].c_str();
        turn_[i].username = turnUsers_[i].c_str();
        turn_[i].password = turnPasses_[i].c_str();
    }
    return true;
}

TransportImpl::~TransportImpl() {
    for (auto& [id, r] : peers_) destroy_agent(r);
    if (shim_) mppm_shim_detach(shim_); // before the socket closes: its fd number may be reused at once
    if (host_) enet_host_destroy(host_);
#if defined(_WIN32)
    if (wakeSock_ != INVALID_SOCKET) ::closesocket(wakeSock_);
#else
    if (wakeRead_ >= 0) ::close(wakeRead_);
    if (wakeWrite_ >= 0 && wakeWrite_ != wakeRead_) ::close(wakeWrite_);
#endif
}

std::vector<std::string> TransportImpl::local_addresses() const {
    std::vector<std::string> out;
    std::string port = std::to_string(bound_.port);
    if (bound_.host != 0) {
        out.push_back(ip_str(bound_.host) + ":" + port);
        return out;
    }
    for (uint32_t ip : interface_ips()) out.push_back(ip_str(ip) + ":" + port);
    out.push_back("127.0.0.1:" + port);
    return out;
}

// ---- Peers.

PeerRec* TransportImpl::find(PeerId id) {
    auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : &it->second;
}

PeerRec* TransportImpl::find_agent(int agentId) {
    for (auto& [id, r] : peers_)
        if (r.agent && r.agentId == agentId) return &r;
    return nullptr;
}

void TransportImpl::configure_new(ENetPeer* p) {
    enet_peer_timeout(p, 32, cfg_.timeoutMinMs, cfg_.timeoutMaxMs);
    enet_peer_ping_interval(p, 250);
}

void TransportImpl::configure_connected(ENetPeer* p) {
    configure_new(p);
    enet_peer_throttle_configure(p, 5000, 0, 0); // throttling off: unreliable packets are never dropped
    p->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
}

void TransportImpl::destroy_agent(PeerRec& r) {
    if (!r.agent) return;
    juice_destroy(r.agent); // no callback runs after this returns
    mppm_shim_set_agent(shim_, r.agentId, nullptr);
    delete r.ctx;
    r.agent = nullptr;
    r.ctx = nullptr;
}

void TransportImpl::erase(PeerId id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    destroy_agent(it->second);
    if (it->second.enet) it->second.enet->data = nullptr;
    peers_.erase(it);
}

void TransportImpl::emit(Event::Kind kind, PeerId peer, std::string text, std::string aux) {
    Event e;
    e.kind = kind;
    e.peer = peer;
    e.text = std::move(text);
    e.aux = std::move(aux);
    pending_.push_back(std::move(e));
}

bool TransportImpl::relay_link(const ENetAddress& a) const {
    if (relay_.hasAllocated && same_addr(a, relay_.allocated)) return true;
    // The host cannot tell relay ports from direct clients on its own machine; a relayed client
    // comes from the relay's address, which is not one of ours.
    if (relay_.registered && relay_.role == 0 && a.host == relay_.ctrl.host && !is_loopback(a.host) &&
        std::find(localIps_.begin(), localIps_.end(), a.host) == localIps_.end())
        return true;
    return false;
}

PeerId TransportImpl::connect(const std::string& ipPort, uint32_t connectData) {
    ENetAddress a{};
    if (!parse_addr(ipPort, a) || mppm_synth_is(&a)) return kNoPeer;
    ENetPeer* p = enet_host_connect(host_, &a, 2, connectData);
    if (!p) return kNoPeer;
    PeerRec r;
    r.id = nextId_++;
    r.link = relay_link(a) ? Link::Relay : Link::Direct;
    r.enet = p;
    r.addr = a;
    r.outgoing = true;
    r.connectData = connectData;
    p->data = reinterpret_cast<void*>(static_cast<uintptr_t>(r.id));
    configure_new(p);
    peers_[r.id] = r;
    return r.id;
}

void TransportImpl::punch(const std::string& ipPort, int count) {
    ENetAddress a{};
    if (count <= 0 || !parse_addr(ipPort, a) || mppm_synth_is(&a)) return;
    punches_.push_back({a, count, Transport::now_ns()});
    run_timers(Transport::now_ns());
}

void TransportImpl::relay_register(const std::string& relayIpPort, const uint8_t token[16]) {
    ENetAddress a{};
    if (!parse_addr(relayIpPort, a)) {
        emit(Event::Kind::RelayFailed, kNoPeer, "bad relay address '" + relayIpPort + "'");
        return;
    }
    relay_ = Relay{};
    relay_.active = true;
    relay_.ctrl = a;
    std::memcpy(relay_.token, token, 16);
    uint64_t now = Transport::now_ns();
    relay_.deadline = now + kRelayGiveUpNs;
    relay_.nextSend = now;
    run_timers(now);
}

void TransportImpl::run_timers(uint64_t now) {
    if (relay_.active && now >= relay_.nextSend) {
        if (!relay_.registered && now >= relay_.deadline) {
            relay_.active = false;
            emit(Event::Kind::RelayFailed, kNoPeer, "no answer from the relay " + addr_str(relay_.ctrl) + " within 5 s");
        } else {
            uint8_t reg[MPRL_REG_SIZE];
            mprl_encode_reg(reg, relay_.token);
            mppm_shim_send(shim_, &relay_.ctrl, reg, sizeof reg);
            relay_.nextSend = now + (relay_.registered ? kRelayRefreshNs : kRelayRetryNs);
            if (!relay_.registered && relay_.nextSend > relay_.deadline) relay_.nextSend = relay_.deadline;
        }
    }
    for (auto it = punches_.begin(); it != punches_.end();) {
        if (now >= it->next) {
            uint8_t nonce[8], dgram[MPRL_PUNCH_SIZE];
            uint64_t r = rng_();
            std::memcpy(nonce, &r, 8);
            mprl_encode_punch(dgram, nonce);
            mppm_shim_send(shim_, &it->to, dgram, sizeof dgram);
            it->next += kPunchSpacingNs;
            if (--it->remaining <= 0) {
                it = punches_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

uint64_t TransportImpl::next_timer_ns() const {
    uint64_t t = UINT64_MAX;
    if (relay_.active) t = relay_.nextSend;
    for (const Punch& p : punches_) t = std::min(t, p.next);
    return t;
}

void TransportImpl::process_mprl(uint64_t now) {
    ENetAddress from{};
    uint8_t buf[64];
    size_t n;
    while ((n = mppm_shim_pop_mprl(shim_, &from, buf, sizeof buf)) != 0) {
        mprl_msg m;
        if (mprl_decode(buf, n, &m) != 0) continue;
        if (m.type == MPRL_PUNCH) continue; // a punch only opens NAT mappings
        if (!relay_.active || !same_addr(from, relay_.ctrl)) continue;
        if (m.type == MPRL_OK) {
            char aux[32];
            std::snprintf(aux, sizeof aux, "%u.%u.%u.%u:%u", m.ip[0], m.ip[1], m.ip[2], m.ip[3], m.port);
            std::string text = m.role == 0 ? std::string("host") : ip_str(relay_.ctrl.host) + ":" + std::to_string(m.relay_port);
            relay_.role = m.role;
            if (m.role == 1) {
                relay_.allocated.host = relay_.ctrl.host;
                relay_.allocated.port = m.relay_port;
                relay_.hasAllocated = true;
            }
            bool first = !relay_.registered;
            if (first) relay_.nextSend = now + kRelayRefreshNs;
            relay_.registered = true;
            if (first || text != relay_.text || aux != relay_.aux) {
                relay_.text = text;
                relay_.aux = aux;
                emit(Event::Kind::RelayRegistered, kNoPeer, text, aux);
            }
        } else if (m.type == MPRL_ERR) {
            relay_.active = false;
            char why[64];
            std::snprintf(why, sizeof why, "relay error %u (%s)", m.err, mprl_err_name(m.err));
            emit(Event::Kind::RelayFailed, kNoPeer, why);
        }
    }
}

// ---- ICE.

juice_agent_t* TransportImpl::make_agent(AgentCtx* ctx, bool retryPort) {
    juice_config_t jc{};
    // libjuice does no TURN in mux mode (agent.c "TURN servers are not supported in mux mode"): the
    // host falls back to poll mode (one port per agent) when TURN servers are configured.
    bool mux = cfg_.listen && turn_.empty();
    jc.concurrency_mode = mux ? JUICE_CONCURRENCY_MODE_MUX : JUICE_CONCURRENCY_MODE_POLL;
    // NULL lets libjuice list the interfaces; a bound address becomes the only host candidate.
    jc.bind_address = bound_.host == 0 ? nullptr : cfg_.bindAddress.c_str();
    if (mux) {
        if (muxPort_ == 0 || retryPort) muxPort_ = pick_udp_port(bound_.host);
        jc.local_port_range_begin = jc.local_port_range_end = muxPort_;
    }
    if (!stunHost_.empty()) {
        jc.stun_server_host = stunHost_.c_str();
        jc.stun_server_port = stunPort_;
    }
    if (!turn_.empty()) {
        jc.turn_servers = turn_.data();
        jc.turn_servers_count = static_cast<int>(turn_.size());
    }
    jc.cb_state_changed = cb_state;
    jc.cb_candidate = cb_candidate;
    jc.cb_gathering_done = cb_gathering_done;
    jc.cb_recv = cb_recv;
    jc.user_ptr = ctx;
    return juice_create(&jc);
}

PeerId TransportImpl::ice_create(bool controlling, uint32_t connectData) {
    int agentId = 0;
    for (int tries = 0; tries < MPPM_SHIM_MAX_AGENTS - 1 && !agentId; tries++) {
        int id = nextAgentId_;
        nextAgentId_ = nextAgentId_ % (MPPM_SHIM_MAX_AGENTS - 1) + 1; // 1..254
        if (!find_agent(id)) agentId = id;
    }
    if (!agentId) return kNoPeer;
    PeerId id = nextId_++;
    auto* ctx = new AgentCtx{this, id, agentId};
    juice_agent_t* agent = make_agent(ctx, false);
    if (!agent && cfg_.listen && turn_.empty()) agent = make_agent(ctx, true); // the mux port was taken meanwhile
    if (!agent) {
        delete ctx;
        return kNoPeer;
    }
    mppm_shim_set_agent(shim_, agentId, agent);
    PeerRec r;
    r.id = id;
    r.link = Link::Ice;
    mppm_synth_make(agentId, &r.addr);
    r.agent = agent;
    r.ctx = ctx;
    r.agentId = agentId;
    r.controlling = controlling;
    r.connectData = connectData;
    r.outgoing = controlling;
    peers_[id] = r;
    return id;
}

std::string TransportImpl::ice_local_description(PeerId peer) {
    PeerRec* r = find(peer);
    if (!r || !r->agent) return {};
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(r->agent, sdp, sizeof sdp) != 0) return {};
    return cfg_.iceRelayOnly ? strip_non_relay(sdp) : std::string(sdp);
}

bool TransportImpl::ice_set_remote_description(PeerId peer, const std::string& sdp) {
    PeerRec* r = find(peer);
    if (!r || !r->agent) return false;
    std::string s = cfg_.iceRelayOnly ? strip_non_relay(sdp) : sdp;
    return juice_set_remote_description(r->agent, s.c_str()) == 0;
}

bool TransportImpl::ice_add_remote_candidate(PeerId peer, const std::string& candidateLine) {
    PeerRec* r = find(peer);
    if (!r || !r->agent) return false;
    if (cfg_.iceRelayOnly && candidate_type(candidateLine) != "relay") return true; // accepted, ignored
    return juice_add_remote_candidate(r->agent, candidateLine.c_str()) == 0;
}

void TransportImpl::ice_set_remote_gathering_done(PeerId peer) {
    PeerRec* r = find(peer);
    if (r && r->agent) juice_set_remote_gathering_done(r->agent);
}

bool TransportImpl::ice_gather(PeerId peer) {
    PeerRec* r = find(peer);
    return r && r->agent && juice_gather_candidates(r->agent) == 0;
}

void TransportImpl::queue_cb(CbItem item) {
    {
        std::lock_guard<std::mutex> lock(cbLock_);
        cbQueue_.push_back(std::move(item));
    }
    wake();
}

void TransportImpl::process_callbacks() {
    std::deque<CbItem> items;
    {
        std::lock_guard<std::mutex> lock(cbLock_);
        items.swap(cbQueue_);
    }
    for (CbItem& it : items) {
        PeerRec* r = find(it.peer);
        if (!r || !r->agent) continue; // reset or destroyed meanwhile
        switch (it.kind) {
        case CbItem::Kind::Candidate:
            if (cfg_.iceRelayOnly && candidate_type(it.text) != "relay") break;
            emit(Event::Kind::IceCandidate, r->id, it.text);
            break;
        case CbItem::Kind::GatheringDone:
            emit(Event::Kind::IceGatheringDone, r->id);
            break;
        case CbItem::Kind::State:
            if ((it.state == JUICE_STATE_CONNECTED || it.state == JUICE_STATE_COMPLETED) && !r->iceConnected && !r->iceFailed) {
                r->iceConnected = true;
                char lc[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {}, rc[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
                char la[JUICE_MAX_ADDRESS_STRING_LEN] = {}, ra[JUICE_MAX_ADDRESS_STRING_LEN] = {};
                juice_get_selected_candidates(r->agent, lc, sizeof lc, rc, sizeof rc);
                juice_get_selected_addresses(r->agent, la, sizeof la, ra, sizeof ra);
                r->iceRemote = ra;
                std::fprintf(stderr, "mppm: net: ice connected peer=%u local=%s remote=%s\n", r->id,
                             candidate_type(lc).c_str(), candidate_type(rc).c_str());
                emit(Event::Kind::IceState, r->id, "connected");
                if (r->controlling && !r->enet) {
                    ENetPeer* p = enet_host_connect(host_, &r->addr, 2, r->connectData);
                    if (!p) {
                        PeerId gone = r->id;
                        emit(Event::Kind::Disconnected, gone, "no free ENet peer");
                        erase(gone);
                        break;
                    }
                    r->enet = p;
                    p->data = reinterpret_cast<void*>(static_cast<uintptr_t>(r->id));
                    configure_new(p);
                }
            } else if (it.state == JUICE_STATE_FAILED && !r->iceFailed) {
                r->iceFailed = true;
                emit(Event::Kind::IceState, r->id, "failed");
                if (r->enet) {
                    PeerId gone = r->id;
                    enet_peer_reset(r->enet);
                    emit(Event::Kind::Disconnected, gone, "ice failed");
                    erase(gone);
                }
            }
            break;
        }
    }
}

// ---- Traffic.

bool TransportImpl::send(PeerId peer, Channel channel, const uint8_t* data, size_t size, bool flush) {
    if (size == 0 || size > kMaxDatagram || !data) return false;
    PeerRec* r = find(peer);
    if (!r || !r->enet || !r->connected || r->enet->state != ENET_PEER_STATE_CONNECTED) return false;
    enet_uint32 flags = channel == Channel::Control ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* pk = enet_packet_create(data, size, flags);
    if (!pk) return false;
    if (enet_peer_send(r->enet, static_cast<enet_uint8>(channel), pk) < 0) {
        if (pk->referenceCount == 0) enet_packet_destroy(pk);
        return false;
    }
    r->bytesSent += size;
    if (flush) enet_host_flush(host_);
    return true;
}

void TransportImpl::disconnect(PeerId peer, uint32_t reason) {
    PeerRec* r = find(peer);
    if (!r) return;
    char text[48];
    std::snprintf(text, sizeof text, "local data=%u", reason);
    if (r->enet && r->connected && r->enet->state == ENET_PEER_STATE_CONNECTED) {
        r->localDisconnect = true;
        r->localReason = reason;
        enet_peer_disconnect(r->enet, reason); // Disconnected follows the remote's acknowledgement (or a timeout)
        return;
    }
    if (r->enet) enet_peer_reset(r->enet);
    emit(Event::Kind::Disconnected, peer, text);
    erase(peer);
}

void TransportImpl::reset(PeerId peer) {
    PeerRec* r = find(peer);
    if (!r) return;
    if (r->enet) enet_peer_reset(r->enet);
    erase(peer);
}

void TransportImpl::handle_enet(const ENetEvent& e) {
    PeerId id = static_cast<PeerId>(reinterpret_cast<uintptr_t>(e.peer ? e.peer->data : nullptr));
    switch (e.type) {
    case ENET_EVENT_TYPE_CONNECT: {
        PeerRec* r = id ? find(id) : nullptr;
        if (!r) { // incoming
            if (mppm_synth_is(&e.peer->address)) {
                r = find_agent(mppm_synth_id(&e.peer->address));
                // No agent for it, the agent already carries a connection, or the remote claims
                // another member than the one that signalled this agent: the signalled id stands.
                if (!r || r->enet || (r->connectData && e.data != r->connectData)) {
                    enet_peer_reset(e.peer);
                    return;
                }
            } else {
                if (!cfg_.listen) { // only the session host accepts connections
                    enet_peer_reset(e.peer);
                    return;
                }
                PeerRec n;
                n.id = nextId_++;
                n.link = relay_link(e.peer->address) ? Link::Relay : Link::Direct;
                n.addr = e.peer->address;
                peers_[n.id] = n;
                r = &peers_[n.id];
            }
            r->enet = e.peer;
            if (!r->connectData) r->connectData = e.data;
            e.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(r->id));
        }
        r->connected = true;
        configure_connected(e.peer);
        Event ev;
        ev.kind = Event::Kind::Connected;
        ev.peer = r->id;
        ev.connectData = r->connectData;
        pending_.push_back(std::move(ev));
        break;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
        PeerRec* r = id ? find(id) : nullptr;
        e.peer->data = nullptr;
        if (!r) return;
        r->enet = nullptr; // ENet has reset it
        char text[64];
        if (r->localDisconnect) {
            std::snprintf(text, sizeof text, "local data=%u", r->localReason);
        } else if (!r->connected) {
            std::snprintf(text, sizeof text, "connect failed");
        } else if (e.data != 0) {
            std::snprintf(text, sizeof text, "remote data=%u", e.data);
        } else {
            mppm_shim_addr_stats st;
            mppm_shim_addr_stats_get(shim_, &r->addr, &st);
            uint64_t now = Transport::now_ns();
            bool silent = st.last_recv_ns == 0 || now - st.last_recv_ns >= 2000 * kMs;
            std::snprintf(text, sizeof text, "%s", silent ? "timeout" : "remote data=0");
        }
        emit(Event::Kind::Disconnected, id, text);
        erase(id);
        break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
        PeerRec* r = id ? find(id) : nullptr;
        if (r && e.packet->dataLength > 0 && e.packet->dataLength <= kMaxDatagram) {
            Event ev;
            ev.kind = Event::Kind::Received;
            ev.peer = id;
            ev.channel = e.channelID == 0 ? Channel::Control : Channel::Input;
            mppm_shim_addr_stats st;
            mppm_shim_addr_stats_get(shim_, &r->addr, &st);
            ev.recvNs = st.last_recv_ns ? st.last_recv_ns : Transport::now_ns();
            ev.data.assign(e.packet->data, e.packet->data + e.packet->dataLength);
            r->bytesReceived += e.packet->dataLength;
            pending_.push_back(std::move(ev));
        }
        enet_packet_destroy(e.packet);
        break;
    }
    default:
        break;
    }
}

void TransportImpl::service(uint32_t timeoutUs, std::vector<Event>& out) {
    uint64_t now = Transport::now_ns();
    bool ready = !pending_.empty() || mppm_shim_agent_pending(shim_);
    if (!ready) {
        std::lock_guard<std::mutex> lock(cbLock_);
        ready = !cbQueue_.empty();
    }
    if (timeoutUs > 0 && !ready) {
        uint64_t until = now + static_cast<uint64_t>(timeoutUs) * 1000ull;
        until = std::min({until, mppm_shim_next_release_ns(shim_), next_timer_ns()});
        if (until > now) {
            uint64_t wait = until - now;
#if defined(_WIN32)
            WSAPOLLFD fds[2] = {{host_->socket, POLLRDNORM, 0}, {wakeSock_, POLLRDNORM, 0}};
            WSAPoll(fds, 2, static_cast<INT>((wait + 999999) / 1000000));
#elif defined(__linux__)
            pollfd fds[2] = {{host_->socket, POLLIN, 0}, {wakeRead_, POLLIN, 0}};
            timespec ts{static_cast<time_t>(wait / 1000000000ull), static_cast<long>(wait % 1000000000ull)};
            ppoll(fds, 2, &ts, nullptr);
#else
            pollfd fds[2] = {{host_->socket, POLLIN, 0}, {wakeRead_, POLLIN, 0}};
            poll(fds, 2, static_cast<int>((wait + 999999) / 1000000));
#endif
        }
    }
#if defined(_WIN32)
    {
        char buf[64];
        while (::recv(wakeSock_, buf, sizeof buf, 0) > 0) {}
    }
#else
    if (wakeIsPipe_) {
        uint8_t buf[64];
        while (::read(wakeRead_, buf, sizeof buf) > 0) {}
    } else {
        uint64_t v;
        ssize_t r = ::read(wakeRead_, &v, sizeof v);
        (void)r;
    }
#endif
    process_callbacks();
    now = Transport::now_ns();
    mppm_shim_release_due(shim_, now);
    run_timers(now);
    ENetEvent e;
    for (int guard = 0; guard < 100000 && enet_host_service(host_, &e, 0) > 0; guard++) handle_enet(e);
    process_mprl(Transport::now_ns());
    if (!pending_.empty()) {
        out.insert(out.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_.clear();
    }
}

void TransportImpl::wake() {
#if defined(_WIN32)
    const char b = 1;
    ::send(wakeSock_, &b, 1, 0);
#else
    if (wakeIsPipe_) {
        uint8_t b = 1;
        ssize_t r = ::write(wakeWrite_, &b, 1);
        (void)r;
    } else {
        uint64_t one = 1;
        ssize_t r = ::write(wakeWrite_, &one, sizeof one);
        (void)r;
    }
#endif
}

bool TransportImpl::stats(PeerId peer, PeerStats& out) const {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    const PeerRec& r = it->second;
    out = PeerStats{};
    out.link = r.link;
    switch (r.link) {
    case Link::Direct: out.remote = addr_str(r.addr); break;
    case Link::Ice: out.remote = "ice " + (r.iceRemote.empty() ? std::string("-") : r.iceRemote); break;
    case Link::Relay: out.remote = "relay " + addr_str(r.addr); break;
    }
    if (r.enet) {
        out.rttUs = r.enet->roundTripTime * 1000u;
        out.rttVarUs = r.enet->roundTripTimeVariance * 1000u;
        out.lossPpm = static_cast<uint32_t>(static_cast<uint64_t>(r.enet->packetLoss) * 1000000ull / ENET_PEER_PACKET_LOSS_SCALE);
    }
    out.bytesSent = r.bytesSent;
    out.bytesReceived = r.bytesReceived;
    mppm_shim_addr_stats st;
    mppm_shim_addr_stats_get(shim_, &r.addr, &st);
    out.datagramsSent = st.sent;
    out.datagramsReceived = st.received;
    out.impairDropped = st.dropped;
    return true;
}

} // namespace

std::unique_ptr<Transport> Transport::create(const TransportConfig& cfg, std::string& err) {
    std::call_once(g_init, global_init);
    auto t = std::make_unique<TransportImpl>();
    if (!t->init(cfg, err)) return nullptr;
    return t;
}

} // namespace mppm::net
