// M5.3b transport tests (docs/online/M5_3B_TRANSPORT.md "Tests"): p2transport_test, driven by
// tests/net/transport/transport_test.sh. Every case prints one "RESULT <case> PASS|FAIL ..." line and
// exits 0 on PASS.
//
//   p2transport_test selftest
//   p2transport_test serve  --bind 127.0.0.1 --port P [--impair SPEC] [--clients N] [--timeout-s S]
//   p2transport_test client --connect IP:PORT [--impair SPEC] [--count 2000] [--reliable 200]
//                           [--interval-us 2000] [--connect-data N] [--rtt-ms MS --rtt-tol MS]
//                           [--loss-pct P --loss-tol P] [--name NAME] [--timeout-s S]
//   p2transport_test ice    [--turn IP:PORT --user U --pass P] [--relay-only] [--count N]
//   p2transport_test relay  --relay IP:PORT --host-token HEX32 --client-token HEX32 [--count N] [--no-bad-token]
//   p2transport_test wake   [--rounds 20]
//   p2transport_test teardown [--cycles 50]
#include "transport.hpp"

extern "C" {
#include "enet_juice.h"
#include "mprl.h"
}

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mppm::net;

namespace {

// ---------------------------------------------------------------------------------------------------
// Small helpers.

struct Args {
    std::map<std::string, std::string> kv;
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string str(const std::string& k, const std::string& d = {}) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    long num(const std::string& k, long d) const { return has(k) ? std::strtol(str(k).c_str(), nullptr, 0) : d; }
    double dbl(const std::string& k, double d) const { return has(k) ? std::strtod(str(k).c_str(), nullptr) : d; }
};

Args parse_args(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; i++) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) continue;
        k = k.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a.kv[k] = argv[++i];
        else a.kv[k] = "1";
    }
    return a;
}

uint64_t now_ns() { return Transport::now_ns(); }
double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
void put64(std::vector<uint8_t>& b, size_t off, uint64_t v) { std::memcpy(b.data() + off, &v, 8); }
uint32_t get32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
uint64_t get64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v = 0;
    if (off + 8 <= b.size()) std::memcpy(&v, b.data() + off, 8);
    return v;
}

bool hex16(const std::string& s, uint8_t out[16]) {
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        char* end = nullptr;
        out[i] = static_cast<uint8_t>(std::strtoul(b, &end, 16));
        if (!end || *end) return false;
    }
    return true;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::llround(p * static_cast<double>(v.size() - 1)));
    return v[std::min(i, v.size() - 1)];
}

std::unique_ptr<Transport> make(const TransportConfig& cfg) {
    std::string err;
    auto t = Transport::create(cfg, err);
    if (!t) std::fprintf(stderr, "p2transport_test: Transport::create: %s\n", err.c_str());
    return t;
}

// ---- Test traffic: PING (unreliable, echoed as PONG), REL (reliable, numbered), DONE / STAT.
enum : uint8_t { kPing = 'P', kPong = 'Q', kRel = 'R', kDone = 'D', kStat = 'S' };
constexpr size_t kPingSize = 64;  // about one input record
constexpr size_t kRelSize = 100;

std::vector<uint8_t> msg_ping(uint8_t type, uint32_t seq, uint64_t t) {
    std::vector<uint8_t> b(kPingSize, 0xA5);
    b[0] = type;
    put32(b, 1, seq);
    put64(b, 5, t);
    return b;
}

// ---------------------------------------------------------------------------------------------------
// selftest

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        g_fail++;
    }
}

void selftest_parse() {
    struct Ok { const char* text; uint32_t d, j, l, u, s; };
    const Ok ok[] = {
        {"", 0, 0, 0, 0, 1},
        {"off", 0, 0, 0, 0, 1},
        {"  off ", 0, 0, 0, 0, 1},
        {"delay=46ms,jitter=8ms,loss=2%,dup=0.1%,seed=7", 46000, 8000, 20000, 1000, 7},
        {"seed=7,dup=0.1%,loss=2%,jitter=8ms,delay=46ms", 46000, 8000, 20000, 1000, 7},
        {"delay=500us", 500, 0, 0, 0, 1},
        {"delay=1s", 1000000, 0, 0, 0, 1},
        {"delay=1.5ms", 1500, 0, 0, 0, 1},
        {"loss=250ppm", 0, 0, 250, 0, 1},
        {" delay = 10ms , loss=0% ", 10000, 0, 0, 0, 1},
        {"seed=0", 0, 0, 0, 0, 1},
        {"loss=100%", 0, 0, 1000000, 0, 1},
        {"delay=20ms,jitter=4ms,loss=2%", 20000, 4000, 20000, 0, 1},
    };
    for (const Ok& c : ok) {
        Impair im;
        std::string err;
        bool r = parse_impair(c.text, im, err);
        char what[160];
        std::snprintf(what, sizeof what, "parse_impair(\"%s\") -> %u/%u/%u/%u/%u (%s)", c.text, im.delayUs, im.jitterUs,
                      im.lossPpm, im.dupPpm, im.seed, r ? "ok" : err.c_str());
        check(r && im.delayUs == c.d && im.jitterUs == c.j && im.lossPpm == c.l && im.dupPpm == c.u && im.seed == c.s, what);
    }
    const char* bad[] = {"delay=10", "delay=ms", "foo=1", "loss=2", "loss=101%", "delay=10ms,delay=5ms", "delay=10ms,",
                         "delay", "jitter=-1ms", "seed=x", "seed=-1", "delay=20s", "loss=2 %x", ",", "delay=1e3ms", "on"};
    for (const char* b : bad) {
        Impair im;
        std::string err;
        bool r = parse_impair(b, im, err);
        char what[160];
        std::snprintf(what, sizeof what, "parse_impair(\"%s\") must fail", b);
        check(!r && !err.empty(), what);
    }
    Impair none;
    check(!none.enabled(), "default Impair is disabled");
}

void selftest_synth() {
    ENetAddress a{};
    mppm_synth_make(5, &a);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a.host);
    check(b[0] == 240 && b[1] == 0 && b[2] == 0 && b[3] == 5 && a.port == 1, "synth_make(5) = 240.0.0.5:1");
    check(mppm_synth_is(&a) && mppm_synth_id(&a) == 5, "240.0.0.5:1 is agent 5");
    a.port = 2;
    check(mppm_synth_is(&a) && mppm_synth_id(&a) == 0, "240.0.0.5:2 is synthetic but no agent");
    enet_address_set_host_ip(&a, "240.0.0.0");
    a.port = 1;
    check(mppm_synth_id(&a) == 0, "240.0.0.0 is no agent");
    enet_address_set_host_ip(&a, "241.0.0.5");
    check(mppm_synth_is(&a) && mppm_synth_id(&a) == 0, "241.0.0.5 is synthetic, no agent");
    enet_address_set_host_ip(&a, "10.0.0.1");
    check(!mppm_synth_is(&a), "10.0.0.1 is real");
    enet_address_set_host_ip(&a, "127.0.0.1");
    check(!mppm_synth_is(&a), "127.0.0.1 is real");
    enet_address_set_host_ip(&a, "239.255.255.255");
    check(!mppm_synth_is(&a), "239.255.255.255 is real");
    for (int id = 1; id < 255; id++) {
        mppm_synth_make(id, &a);
        if (mppm_synth_id(&a) != id) { check(false, "synth id round trip"); break; }
    }
}

void selftest_mprl() {
    uint8_t token[16];
    for (int i = 0; i < 16; i++) token[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t b[64];
    size_t n = mprl_encode_reg(b, token);
    const uint8_t head[8] = {0x4D, 0x50, 0x52, 0x4C, 1, 1, 0, 0};
    check(n == 24 && std::memcmp(b, head, 8) == 0 && std::memcmp(b + 8, token, 16) == 0, "REG bytes");
    mprl_msg m;
    check(mprl_decode(b, n, &m) == 0 && m.type == MPRL_REG && std::memcmp(m.token, token, 16) == 0, "REG round trip");
    check(mprl_decode(b, n - 1, &m) != 0, "short REG rejected");
    b[4] = 2;
    check(mprl_decode(b, n, &m) != 0, "version 2 rejected");

    const uint8_t ip[4] = {203, 0, 113, 7};
    n = mprl_encode_ok(b, 1, 47210, ip, 40123);
    const uint8_t ok[18] = {0x4D, 0x50, 0x52, 0x4C, 1, 2, 0, 0, 1, 0xB8, 0x6A, 4, 203, 0, 113, 7, 0x9C, 0xBB};
    check(n == 18 && std::memcmp(b, ok, 18) == 0, "OK bytes (big-endian ports)");
    check(mprl_decode(b, n, &m) == 0 && m.type == MPRL_OK && m.role == 1 && m.relay_port == 47210 && m.port == 40123 &&
                  std::memcmp(m.ip, ip, 4) == 0, "OK round trip");
    b[11] = 6;
    check(mprl_decode(b, n, &m) != 0, "OK with family 6 rejected");
    b[11] = 4;
    b[8] = 2;
    check(mprl_decode(b, n, &m) != 0, "OK with role 2 rejected");

    n = mprl_encode_err(b, MPRL_ERR_NO_PORT);
    check(n == 9 && b[5] == 3 && b[8] == 4, "ERR bytes");
    check(mprl_decode(b, n, &m) == 0 && m.type == MPRL_ERR && m.err == 4, "ERR round trip");
    check(std::strcmp(mprl_err_name(1), "unknown token") == 0, "ERR names");

    const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    n = mprl_encode_punch(b, nonce);
    check(n == 16 && b[5] == 4 && std::memcmp(b + 8, nonce, 8) == 0, "PUNCH bytes");
    check(mprl_decode(b, n, &m) == 0 && m.type == MPRL_PUNCH && std::memcmp(m.nonce, nonce, 8) == 0, "PUNCH round trip");
    b[5] = 9;
    check(mprl_decode(b, n, &m) != 0, "unknown type rejected");
    check(!mprl_is(reinterpret_cast<const uint8_t*>("MPR"), 3) && mprl_is(reinterpret_cast<const uint8_t*>("MPRLx"), 5), "mprl_is");
}

void selftest_impair_stats(uint32_t delay, uint32_t jitter, uint32_t loss, uint32_t dup, uint32_t seed) {
    mppm_impair im;
    mppm_impair_init(&im, delay, jitter, loss, dup, seed);
    const int kDraws = 100000;
    int dropped = 0, dups = 0, copies = 0;
    uint32_t dmin = UINT32_MAX, dmax = 0;
    double dsum = 0;
    for (int i = 0; i < kDraws; i++) {
        uint32_t d[2];
        int c = mppm_impair_draw(&im, d);
        if (c == 0) dropped++;
        if (c == 2) dups++;
        for (int k = 0; k < c; k++) {
            dmin = std::min(dmin, d[k]);
            dmax = std::max(dmax, d[k]);
            dsum += d[k];
            copies++;
        }
    }
    // Duplication is drawn for the datagrams that were not dropped.
    double lossPct = 100.0 * dropped / kDraws, dupPct = 100.0 * dups / kDraws, mean = copies ? dsum / copies : 0;
    double dupWant = (1.0 - loss / 1e6) * dup / 1e4;
    std::printf("  impairment delay=%u jitter=%u loss=%u dup=%u seed=%u: loss %.3f%% dup %.3f%% delay min %u max %u mean %.0f us\n",
                delay, jitter, loss, dup, seed, lossPct, dupPct, dmin, dmax, mean);
    char what[160];
    std::snprintf(what, sizeof what, "loss %.3f%% within 0.2%% of %.3f%%", lossPct, loss / 1e4);
    check(std::fabs(lossPct - loss / 1e4) <= 0.2, what);
    std::snprintf(what, sizeof what, "delays within [%u, %u]", delay, delay + jitter);
    check(dmin >= delay && dmax <= delay + jitter, what);
    std::snprintf(what, sizeof what, "dup %.3f%% within 0.1%% of %.3f%%", dupPct, dupWant);
    check(std::fabs(dupPct - dupWant) <= 0.1, what);
    if (jitter) {
        std::snprintf(what, sizeof what, "mean delay %.0f near %u", mean, delay + jitter / 2);
        check(std::fabs(mean - (delay + jitter / 2.0)) <= jitter * 0.02 + 1, what);
        check(dmax >= delay + jitter * 99 / 100 && dmin <= delay + jitter / 100, "jitter covers its range");
    }
}

// The shim end to end on two plain UDP sockets: delay + jitter through the heap, then release.
void selftest_shim_heap() {
    ENetSocket a = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetSocket b = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress any{};
    enet_address_set_host_ip(&any, "127.0.0.1");
    any.port = 0;
    enet_socket_bind(a, &any);
    enet_socket_bind(b, &any);
    enet_socket_set_option(b, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress to{};
    enet_socket_get_address(b, &to);
    mppm_shim* sh = mppm_shim_attach(a, -1, 0);
    check(sh != nullptr, "shim attach");
    if (!sh) return;
    mppm_impair im;
    mppm_impair_init(&im, 5000, 5000, 0, 0, 3);
    mppm_shim_set_impair(sh, &im);
    const int kN = 400;
    std::vector<uint64_t> sent(kN);
    for (int i = 0; i < kN; i++) {
        uint8_t d[12];
        uint32_t seq = static_cast<uint32_t>(i);
        std::memcpy(d, &seq, 4);
        sent[i] = now_ns();
        ENetBuffer buf{d, sizeof d};
        check(enet_socket_send(a, &to, &buf, 1) == static_cast<int>(sizeof d), "shim send returns the length");
    }
    int got = 0, early = 0, late = 0, reordered = 0;
    int last = -1;
    uint64_t end = now_ns() + 200 * 1000000ull;
    while (got < kN && now_ns() < end) {
        mppm_shim_release_due(sh, now_ns());
        uint8_t d[64];
        ENetBuffer buf{d, sizeof d};
        ENetAddress from{};
        int n;
        while ((n = enet_sys_socket_receive(b, &from, &buf, 1)) > 0) {
            uint32_t seq;
            std::memcpy(&seq, d, 4);
            double lat = ms(now_ns() - sent[seq]);
            if (lat < 5.0) early++;
            if (lat > 10.0 + 2.0) late++;
            if (static_cast<int>(seq) < last) reordered++;
            last = std::max(last, static_cast<int>(seq));
            got++;
        }
        usleep(100);
    }
    std::printf("  shim heap: %d/%d released, %d early, %d late (>12 ms), %d reordered by jitter\n", got, kN, early, late, reordered);
    check(got == kN && early == 0 && late <= kN / 50, "shim heap releases every datagram within [delay, delay+jitter]");
    check(reordered > 0, "jitter reorders some datagrams");
    mppm_shim_addr_stats st;
    mppm_shim_addr_stats_get(sh, &to, &st);
    check(st.sent == static_cast<uint64_t>(kN), "shim counts sent datagrams");
    mppm_shim_detach(sh);
    enet_socket_destroy(a);
    enet_socket_destroy(b);
}

int cmd_selftest() {
    enet_initialize();
    std::printf("selftest: parse_impair\n");
    selftest_parse();
    std::printf("selftest: synthetic addresses\n");
    selftest_synth();
    std::printf("selftest: MPRL codec\n");
    selftest_mprl();
    std::printf("selftest: impairment statistics (100000 draws each)\n");
    selftest_impair_stats(46000, 8000, 20000, 1000, 7);
    selftest_impair_stats(50000, 0, 50000, 0, 1);
    selftest_impair_stats(0, 0, 0, 0, 9);
    selftest_impair_stats(20000, 4000, 20000, 0, 2);
    selftest_impair_stats(0, 1000, 100000, 50000, 11);
    {
        mppm_impair a, b;
        mppm_impair_init(&a, 0, 1000, 0, 0, 1);
        mppm_impair_init(&b, 0, 1000, 0, 0, 0);
        uint32_t da[2], db[2];
        mppm_impair_draw(&a, da);
        mppm_impair_draw(&b, db);
        check(da[0] == db[0], "seed 0 behaves as seed 1");
        mppm_impair_init(&b, 0, 1000, 0, 0, 2);
        int same = 0;
        for (int i = 0; i < 100; i++) {
            mppm_impair_draw(&a, da);
            mppm_impair_draw(&b, db);
            same += da[0] == db[0];
        }
        check(same < 10, "different seeds draw differently");
    }
    std::printf("selftest: shim delay queue\n");
    selftest_shim_heap();
    std::printf("RESULT selftest %s failures=%d\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------
// serve / client (separate processes: direct, fanout)

struct ServePeer {
    uint32_t pings = 0, pongs = 0, rel = 0, nextRel = 0, connectData = 0;
    bool inOrder = true, done = false;
};

int cmd_serve(const Args& a) {
    TransportConfig cfg;
    cfg.listen = true;
    cfg.bindAddress = a.str("bind", "127.0.0.1");
    cfg.port = static_cast<uint16_t>(a.num("port", 0));
    cfg.maxPeers = 16;
    std::string err;
    if (!parse_impair(a.str("impair"), cfg.impair, err)) {
        std::fprintf(stderr, "p2transport_test: %s\n", err.c_str());
        return 2;
    }
    auto t = make(cfg);
    if (!t) return 2;
    int clients = static_cast<int>(a.num("clients", 1));
    uint64_t deadline = now_ns() + static_cast<uint64_t>(a.num("timeout-s", 60)) * 1000000000ull;
    std::printf("serve: listening %s\n", t->local_address().c_str());
    for (const std::string& s : t->local_addresses()) std::printf("serve: reachable at %s\n", s.c_str());
    std::fflush(stdout);
    std::map<PeerId, ServePeer> peers;
    int finished = 0, bad = 0;
    std::vector<Event> ev;
    while (finished < clients && now_ns() < deadline) {
        ev.clear();
        t->service(1000, ev);
        for (Event& e : ev) {
            switch (e.kind) {
            case Event::Kind::Connected:
                peers[e.peer].connectData = e.connectData;
                std::printf("serve: peer %u connected data=0x%08x\n", e.peer, e.connectData);
                break;
            case Event::Kind::Received: {
                ServePeer& p = peers[e.peer];
                if (e.data.empty()) break;
                if (e.channel == Channel::Input && e.data[0] == kPing) {
                    p.pings++;
                    std::vector<uint8_t> pong = e.data;
                    pong[0] = kPong;
                    if (t->send(e.peer, Channel::Input, pong.data(), pong.size(), true)) p.pongs++;
                } else if (e.channel == Channel::Control && e.data[0] == kRel) {
                    uint32_t seq = get32(e.data, 1);
                    if (seq != p.nextRel || e.data.size() != kRelSize) p.inOrder = false;
                    p.nextRel = seq + 1;
                    p.rel++;
                } else if (e.channel == Channel::Control && e.data[0] == kDone) {
                    p.done = true;
                    std::vector<uint8_t> st(32, 0);
                    st[0] = kStat;
                    put32(st, 1, p.pings);
                    put32(st, 5, p.pongs);
                    put32(st, 9, p.rel);
                    st[13] = p.inOrder ? 1 : 0;
                    put32(st, 14, p.connectData);
                    t->send(e.peer, Channel::Control, st.data(), st.size(), true);
                    std::printf("serve: peer %u done: pings=%u pongs=%u reliable=%u in_order=%d\n", e.peer, p.pings, p.pongs,
                                p.rel, p.inOrder ? 1 : 0);
                }
                break;
            }
            case Event::Kind::Disconnected: {
                ServePeer& p = peers[e.peer];
                std::printf("serve: peer %u disconnected (%s)\n", e.peer, e.text.c_str());
                if (p.done) finished++;
                else bad++;
                if (!p.inOrder) bad++;
                break;
            }
            default:
                break;
            }
        }
        std::fflush(stdout);
    }
    // Linger: acknowledgements may still sit in the impairment stage.
    for (uint64_t end = now_ns() + 1000000000ull; now_ns() < end;) {
        ev.clear();
        t->service(1000, ev);
    }
    bool ok = finished == clients && bad == 0;
    std::printf("RESULT serve %s finished=%d/%d bad=%d\n", ok ? "PASS" : "FAIL", finished, clients, bad);
    return ok ? 0 : 1;
}

int cmd_client(const Args& a) {
    TransportConfig cfg;
    cfg.bindAddress = a.str("bind", "127.0.0.1");
    cfg.maxPeers = 2;
    std::string err;
    if (!parse_impair(a.str("impair"), cfg.impair, err)) {
        std::fprintf(stderr, "p2transport_test: %s\n", err.c_str());
        return 2;
    }
    std::string name = a.str("name", "client");
    auto t = make(cfg);
    if (!t) return 2;
    uint32_t count = static_cast<uint32_t>(a.num("count", 2000));
    uint32_t reliable = static_cast<uint32_t>(a.num("reliable", 200));
    uint64_t interval = static_cast<uint64_t>(a.num("interval-us", 2000)) * 1000ull;
    uint32_t connectData = static_cast<uint32_t>(a.num("connect-data", 0xC0FFEE01));
    uint64_t timeout = static_cast<uint64_t>(a.num("timeout-s", 60)) * 1000000000ull;
    uint64_t start = now_ns();
    PeerId peer = t->connect(a.str("connect"), connectData);
    if (!peer) {
        std::printf("RESULT client %s FAIL connect('%s') refused\n", name.c_str(), a.str("connect").c_str());
        return 1;
    }
    std::vector<Event> ev;
    bool connected = false, gotStat = false, disconnected = false;
    uint32_t connectedData = 0;
    std::vector<uint8_t> stat;
    std::vector<uint64_t> sentAt(count, 0);
    std::vector<double> rtts;
    uint32_t pongs = 0, dupPongs = 0;
    std::vector<uint8_t> seen(count, 0);
    auto pump = [&](uint32_t us) {
        ev.clear();
        t->service(us, ev);
        for (Event& e : ev) {
            if (e.kind == Event::Kind::Connected) {
                connected = true;
                connectedData = e.connectData;
            } else if (e.kind == Event::Kind::Disconnected) {
                disconnected = true;
                std::printf("client %s: disconnected (%s)\n", name.c_str(), e.text.c_str());
            } else if (e.kind == Event::Kind::Received && !e.data.empty()) {
                if (e.data[0] == kPong) {
                    uint32_t seq = get32(e.data, 1);
                    if (seq < count) {
                        if (seen[seq]) dupPongs++;
                        else {
                            seen[seq] = 1;
                            pongs++;
                            rtts.push_back(ms(e.recvNs - get64(e.data, 5)));
                        }
                    }
                } else if (e.data[0] == kStat) {
                    gotStat = true;
                    stat = e.data;
                }
            }
        }
    };
    while (!connected && !disconnected && now_ns() - start < 15000000000ull) pump(2000);
    if (!connected) {
        std::printf("RESULT client %s FAIL not connected\n", name.c_str());
        return 1;
    }
    uint64_t connectMs = (now_ns() - start) / 1000000ull;
    uint32_t relEvery = reliable ? std::max<uint32_t>(1, count / reliable) : 0;
    uint32_t relSent = 0;
    uint64_t next = now_ns();
    for (uint32_t i = 0; i < count && !disconnected; i++) {
        while (now_ns() < next) {
            uint64_t left = next - now_ns();
            pump(static_cast<uint32_t>(std::min<uint64_t>(left / 1000, 1000)));
        }
        next += interval;
        sentAt[i] = now_ns();
        std::vector<uint8_t> p = msg_ping(kPing, i, sentAt[i]);
        t->send(peer, Channel::Input, p.data(), p.size(), true);
        if (relEvery && i % relEvery == 0 && relSent < reliable) {
            std::vector<uint8_t> r(kRelSize, 0x5A);
            r[0] = kRel;
            put32(r, 1, relSent++);
            t->send(peer, Channel::Control, r.data(), r.size(), true);
        }
    }
    while (relSent < reliable) { // when count < reliable
        std::vector<uint8_t> r(kRelSize, 0x5A);
        r[0] = kRel;
        put32(r, 1, relSent++);
        t->send(peer, Channel::Control, r.data(), r.size(), true);
    }
    uint64_t settle = now_ns() + 1500000000ull;
    while (now_ns() < settle && !disconnected) pump(1000);
    std::vector<uint8_t> done(16, 0);
    done[0] = kDone;
    put32(done, 1, count);
    put32(done, 5, relSent);
    t->send(peer, Channel::Control, done.data(), done.size(), true);
    uint64_t statBy = now_ns() + 15000000000ull;
    while (!gotStat && !disconnected && now_ns() < statBy) pump(1000);
    PeerStats ps;
    t->stats(peer, ps);
    t->disconnect(peer, 7);
    // Disconnected follows the host's acknowledgement or, when loss ate it and the host already exited,
    // ENet's peer timeout (timeoutMaxMs 10 s): wait past that.
    uint64_t discBy = now_ns() + 12000000000ull;
    while (!disconnected && now_ns() < discBy) pump(1000);

    bool ok = gotStat && now_ns() - start < timeout;
    uint32_t hostPings = get32(stat, 1), hostPongs = get32(stat, 5), hostRel = get32(stat, 9);
    bool inOrder = stat.size() > 13 && stat[13] == 1;
    uint32_t hostData = get32(stat, 14);
    double up = count ? 100.0 * (count - std::min(hostPings, count)) / count : 0;
    double down = hostPongs ? 100.0 * (hostPongs - std::min(pongs, hostPongs)) / hostPongs : 100;
    double med = percentile(rtts, 0.5), p10 = percentile(rtts, 0.1), p90 = percentile(rtts, 0.9);
    std::string why;
    if (!gotStat) why += " no-stat";
    if (connectedData != connectData || hostData != connectData) why += " connect-data";
    if (hostRel != reliable || !inOrder) why += " reliable";
    if (dupPongs) why += " duplicates";
    if (a.has("rtt-ms") && std::fabs(med - a.dbl("rtt-ms", 0)) > a.dbl("rtt-tol", 3)) why += " rtt";
    if (a.has("loss-pct")) {
        double want = a.dbl("loss-pct", 0), tol = a.dbl("loss-tol", 1.5);
        if (std::fabs(up - want) > tol || std::fabs(down - want) > tol) why += " loss";
    } else if (up > 0.5 || down > 0.5) {
        why += " loss";
    }
    if (!disconnected) why += " no-disconnect";
    ok = ok && why.empty();
    std::printf("RESULT client %s %s connect_ms=%" PRIu64 " sent=%u host_got=%u (up loss %.2f%%) host_echoed=%u got=%u (down loss %.2f%%) "
                "reliable=%u/%u in_order=%d rtt_ms median=%.2f p10=%.2f p90=%.2f enet_rtt_ms=%u enet_loss_ppm=%u "
                "datagrams_sent=%" PRIu64 " impair_dropped=%" PRIu64 " data=0x%08x/0x%08x%s%s\n",
                name.c_str(), ok ? "PASS" : "FAIL", connectMs, count, hostPings, up, hostPongs, pongs, down, hostRel, reliable,
                inOrder ? 1 : 0, med, p10, p90, ps.rttUs / 1000, ps.lossPpm, ps.datagramsSent, ps.impairDropped, connectedData,
                hostData, why.empty() ? "" : " why:", why.c_str());
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------
// In-process pairs (ice, turn, relay, teardown): two transports serviced by one thread.

struct Side {
    std::unique_ptr<Transport> t;
    PeerId peer = kNoPeer;          // the peer id this side uses for the other
    bool connected = false, disconnected = false, iceConnected = false, iceFailed = false;
    uint32_t connectData = 0;
    std::string disconnectText;
    std::vector<Event> relay;       // RelayRegistered / RelayFailed
    uint32_t pings = 0, pongs = 0, rel = 0, nextRel = 0;
    bool inOrder = true;
    uint64_t lastIceCand = 0;
    int candidates = 0;
};

// Services both sides; routes ICE signalling (host <-> client); host echoes pings.
void pair_pump(Side& host, Side& client, uint32_t us, bool echo = true) {
    Side* sides[2] = {&host, &client};
    for (int i = 0; i < 2; i++) {
        Side& s = *sides[i];
        Side& o = *sides[1 - i];
        std::vector<Event> ev;
        s.t->service(us, ev);
        for (Event& e : ev) {
            switch (e.kind) {
            case Event::Kind::Connected:
                s.connected = true;
                s.connectData = e.connectData;
                if (!s.peer) s.peer = e.peer;
                break;
            case Event::Kind::Disconnected:
                s.disconnected = true;
                s.disconnectText = e.text;
                break;
            case Event::Kind::IceCandidate:
                s.candidates++;
                if (o.peer) o.t->ice_add_remote_candidate(o.peer, e.text);
                break;
            case Event::Kind::IceGatheringDone:
                if (o.peer) o.t->ice_set_remote_gathering_done(o.peer);
                break;
            case Event::Kind::IceState:
                if (e.text == "connected") s.iceConnected = true;
                if (e.text == "failed") s.iceFailed = true;
                break;
            case Event::Kind::RelayRegistered:
            case Event::Kind::RelayFailed:
                s.relay.push_back(e);
                break;
            case Event::Kind::Received:
                if (e.data.empty()) break;
                if (e.data[0] == kPing) {
                    s.pings++;
                    if (echo) {
                        std::vector<uint8_t> p = e.data;
                        p[0] = kPong;
                        s.t->send(e.peer, Channel::Input, p.data(), p.size(), true);
                    }
                } else if (e.data[0] == kPong) {
                    s.pongs++;
                } else if (e.data[0] == kRel) {
                    if (get32(e.data, 1) != s.nextRel) s.inOrder = false;
                    s.nextRel = get32(e.data, 1) + 1;
                    s.rel++;
                }
                break;
            }
        }
    }
}

bool pump_until(Side& h, Side& c, uint64_t timeoutMs, const std::function<bool()>& done) {
    uint64_t end = now_ns() + timeoutMs * 1000000ull;
    while (now_ns() < end) {
        if (done()) return true;
        pair_pump(h, c, 300);
    }
    return done();
}

// Pings and reliable messages client -> host, echoes back; true when everything arrived.
bool pair_traffic(Side& h, Side& c, uint32_t count, uint32_t reliable, std::string& summary) {
    for (uint32_t i = 0; i < count; i++) {
        std::vector<uint8_t> p = msg_ping(kPing, i, now_ns());
        c.t->send(c.peer, Channel::Input, p.data(), p.size(), true);
        if (i < reliable) {
            std::vector<uint8_t> r(kRelSize, 0x5A);
            r[0] = kRel;
            put32(r, 1, i);
            c.t->send(c.peer, Channel::Control, r.data(), r.size(), true);
        }
        pair_pump(h, c, 200);
        pair_pump(h, c, 0);
    }
    pump_until(h, c, 3000, [&] { return h.pings >= count && c.pongs >= count && h.rel >= reliable; });
    char buf[160];
    std::snprintf(buf, sizeof buf, "pings=%u/%u pongs=%u/%u reliable=%u/%u in_order=%d", h.pings, count, c.pongs, count, h.rel,
                  reliable, h.inOrder ? 1 : 0);
    summary = buf;
    // Loopback without impairment: allow 1% unreliable loss (socket buffers), none of the reliable.
    return h.pings * 100 >= count * 99 && c.pongs * 100 >= count * 98 && h.rel == reliable && h.inOrder;
}

bool ice_connect(Side& h, Side& c, uint32_t connectData, uint64_t timeoutMs, std::string& why) {
    // Client: controlling, sends its description first (it becomes libjuice's controlling agent).
    c.peer = c.t->ice_create(true, connectData);
    h.peer = h.t->ice_create(false, 0);
    if (!c.peer || !h.peer) {
        why = "ice_create failed";
        return false;
    }
    std::string offer = c.t->ice_local_description(c.peer);
    if (offer.empty() || !h.t->ice_set_remote_description(h.peer, offer)) {
        why = "offer";
        return false;
    }
    std::string answer = h.t->ice_local_description(h.peer);
    if (answer.empty() || !c.t->ice_set_remote_description(c.peer, answer)) {
        why = "answer";
        return false;
    }
    if (!h.t->ice_gather(h.peer) || !c.t->ice_gather(c.peer)) {
        why = "gather";
        return false;
    }
    bool ok = pump_until(h, c, timeoutMs, [&] { return (h.connected && c.connected) || h.iceFailed || c.iceFailed; });
    if (!ok || !h.connected || !c.connected) {
        why = h.iceFailed || c.iceFailed ? "ice failed" : "timeout";
        return false;
    }
    return true;
}

void pair_close(Side& h, Side& c) {
    if (c.peer && c.connected) c.t->disconnect(c.peer, 1);
    pump_until(h, c, 2000, [&] { return c.disconnected && h.disconnected; });
}

int cmd_ice(const Args& a) {
    bool relayOnly = a.has("relay-only");
    TransportConfig hc, cc;
    hc.listen = true;
    hc.bindAddress = cc.bindAddress = "127.0.0.1";
    if (a.has("turn")) {
        IceServer s{"turn:" + a.str("turn"), a.str("user", "u"), a.str("pass", "p")};
        hc.iceServers.push_back(s);
        cc.iceServers.push_back(s);
    }
    hc.iceRelayOnly = cc.iceRelayOnly = relayOnly;
    Side h, c;
    h.t = make(hc);
    c.t = make(cc);
    if (!h.t || !c.t) return 2;
    std::string name = relayOnly ? "turn" : "ice";
    uint32_t data = 0x1CE00001;
    std::string why;
    uint64_t t0 = now_ns();
    bool ok = ice_connect(h, c, data, 20000, why);
    uint64_t connectMs = (now_ns() - t0) / 1000000ull;
    std::string traffic;
    PeerStats hs, cs;
    if (ok) {
        if (h.connectData != data || c.connectData != data) { ok = false; why = "connect data"; }
        if (ok && !pair_traffic(h, c, static_cast<uint32_t>(a.num("count", 500)), 50, traffic)) { ok = false; why = "traffic"; }
        h.t->stats(h.peer, hs);
        c.t->stats(c.peer, cs);
        if (ok && (hs.link != Link::Ice || cs.link != Link::Ice)) { ok = false; why = "link"; }
        if (ok && relayOnly && (h.candidates == 0 || c.candidates == 0)) { ok = false; why = "no relay candidates"; }
        pair_close(h, c);
        if (ok && !h.disconnected) { ok = false; why = "no disconnect on host"; }
    }
    std::printf("RESULT %s %s connect_ms=%" PRIu64 " host_remote=\"%s\" client_remote=\"%s\" candidates=%d/%d %s%s%s\n", name.c_str(),
                ok ? "PASS" : "FAIL", connectMs, hs.remote.c_str(), cs.remote.c_str(), h.candidates, c.candidates, traffic.c_str(),
                why.empty() ? "" : " why: ", why.c_str());
    return ok ? 0 : 1;
}

int cmd_relay(const Args& a) {
    uint8_t ht[16], ct[16];
    if (!hex16(a.str("host-token"), ht) || !hex16(a.str("client-token"), ct)) {
        std::fprintf(stderr, "p2transport_test relay: --host-token and --client-token take 32 hex digits\n");
        return 2;
    }
    std::string relay = a.str("relay");
    TransportConfig hc, cc;
    hc.listen = true;
    hc.bindAddress = cc.bindAddress = a.str("bind", "127.0.0.1");
    Side h, c;
    h.t = make(hc);
    c.t = make(cc);
    if (!h.t || !c.t) return 2;
    std::string why, traffic;
    h.t->relay_register(relay, ht);
    c.t->relay_register(relay, ct);
    bool ok = pump_until(h, c, 6000, [&] { return !h.relay.empty() && !c.relay.empty(); });
    std::string hostText, clientText, hostAux, clientAux;
    if (!ok) why = "no relay answer";
    else if (h.relay[0].kind != Event::Kind::RelayRegistered || c.relay[0].kind != Event::Kind::RelayRegistered) {
        ok = false;
        why = "relay failed: " + h.relay[0].text + " / " + c.relay[0].text;
    } else {
        hostText = h.relay[0].text;
        hostAux = h.relay[0].aux;
        clientText = c.relay[0].text;
        clientAux = c.relay[0].aux;
        if (hostText != "host") { ok = false; why = "host text"; }
        if (hostAux != h.t->local_address() || clientAux != c.t->local_address()) { ok = false; why = "public address"; }
    }
    uint32_t data = 0x4E1A0002;
    PeerStats hs, cs;
    if (ok) {
        c.t->punch(h.t->local_address(), 3); // the host's shim swallows these
        c.peer = c.t->connect(clientText, data);
        if (!c.peer) { ok = false; why = "connect('" + clientText + "')"; }
    }
    if (ok && !pump_until(h, c, 10000, [&] { return h.connected && c.connected; })) { ok = false; why = "not connected"; }
    if (ok) {
        if (h.connectData != data || c.connectData != data) { ok = false; why = "connect data"; }
        if (ok && !pair_traffic(h, c, static_cast<uint32_t>(a.num("count", 1000)), 100, traffic)) { ok = false; why = "traffic"; }
        h.t->stats(h.peer, hs);
        c.t->stats(c.peer, cs);
        if (ok && (cs.link != Link::Relay || cs.remote != "relay " + clientText)) { ok = false; why = "client link"; }
        if (ok && hs.remote != clientText) { ok = false; why = "host does not see the relay port"; }
        pair_close(h, c);
    }
    std::string bad = "skipped";
    if (ok && !a.has("no-bad-token")) { // an unknown token: the relay answers ERR 1
        TransportConfig bc;
        bc.bindAddress = hc.bindAddress;
        Side b, unused;
        b.t = make(bc);
        unused.t = make(bc);
        uint8_t token[16];
        for (int i = 0; i < 16; i++) token[i] = static_cast<uint8_t>(0xEE ^ i);
        b.t->relay_register(relay, token);
        uint64_t t0 = now_ns();
        pump_until(b, unused, 7000, [&] { return !b.relay.empty(); });
        if (b.relay.empty() || b.relay[0].kind != Event::Kind::RelayFailed) { ok = false; why = "bad token accepted"; }
        else bad = b.relay[0].text + " after " + std::to_string((now_ns() - t0) / 1000000ull) + " ms";
    }
    std::printf("RESULT relay %s host=\"%s\" (public %s) client=\"%s\" (public %s) client_remote=\"%s\" host_remote=\"%s\" "
                "bytes_sent=%" PRIu64 "/%" PRIu64 " %s bad_token=\"%s\"%s%s\n",
                ok ? "PASS" : "FAIL", hostText.c_str(), hostAux.c_str(), clientText.c_str(), clientAux.c_str(), cs.remote.c_str(),
                hs.remote.c_str(), cs.bytesSent, hs.bytesSent, traffic.c_str(), bad.c_str(), why.empty() ? "" : " why: ", why.c_str());
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------------
// wake, teardown

int cmd_wake(const Args& a) {
    TransportConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    auto t = make(cfg);
    if (!t) return 2;
    int rounds = static_cast<int>(a.num("rounds", 20));
    std::vector<double> lat;
    std::vector<Event> ev;
    for (int i = 0; i < rounds; i++) {
        std::atomic<uint64_t> wokeAt{0};
        std::thread th([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            wokeAt = now_ns();
            t->wake();
        });
        uint64_t t0 = now_ns();
        t->service(100000, ev);
        uint64_t t1 = now_ns();
        th.join();
        double l = wokeAt ? ms(t1 - wokeAt) : ms(t1 - t0);
        lat.push_back(l);
        if (ms(t1 - t0) > 90) lat.back() = 1e9; // slept to the timeout: wake() was not seen
    }
    // An idle service(1000) without wake returns after about 1 ms (the timeout is honoured).
    uint64_t t0 = now_ns();
    t->service(1000, ev);
    double idle = ms(now_ns() - t0);
    double med = percentile(lat, 0.5), mx = percentile(lat, 1.0);
    bool ok = mx <= 1.0 && idle >= 0.9 && idle < 20;
    std::printf("RESULT wake %s rounds=%d wake_to_return_ms median=%.3f max=%.3f idle_service_1ms=%.3f\n", ok ? "PASS" : "FAIL",
                rounds, med, mx, idle);
    return ok ? 0 : 1;
}

int count_fds() {
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

bool teardown_cycle(int i, bool ice, std::string& why) {
    TransportConfig hc, cc;
    hc.listen = true;
    hc.bindAddress = cc.bindAddress = "127.0.0.1";
    Side h, c;
    h.t = make(hc);
    c.t = make(cc);
    if (!h.t || !c.t) {
        why = "create";
        return false;
    }
    if (ice) {
        if (!ice_connect(h, c, static_cast<uint32_t>(i), 10000, why)) return false;
    } else {
        c.peer = c.t->connect(h.t->local_address(), static_cast<uint32_t>(i));
        if (!pump_until(h, c, 5000, [&] { return h.connected && c.connected; })) {
            why = "connect";
            return false;
        }
    }
    std::string s;
    if (!pair_traffic(h, c, 20, 5, s)) {
        why = "traffic " + s;
        return false;
    }
    // Every other cycle destroys the transports with the peers still connected.
    if (i % 2) pair_close(h, c);
    return true;
}

int cmd_teardown(const Args& a) {
    int cycles = static_cast<int>(a.num("cycles", 50));
    std::string why;
    // One warm-up cycle of each kind: libjuice keeps a process-wide poll thread while agents live.
    bool ok = teardown_cycle(0, false, why) && teardown_cycle(1, true, why);
    int before = count_fds();
    int done = 0;
    for (int i = 0; ok && i < cycles; i++) {
        ok = teardown_cycle(i, i % 5 == 0, why);
        if (ok) done++;
    }
    int after = count_fds();
    if (ok && after != before) {
        ok = false;
        why = "fd leak";
    }
    std::printf("RESULT teardown %s cycles=%d/%d (every 5th over ICE) fds_before=%d fds_after=%d%s%s\n", ok ? "PASS" : "FAIL", done,
                cycles, before, after, why.empty() ? "" : " why: ", why.c_str());
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "usage: p2transport_test selftest|serve|client|ice|relay|wake|teardown [options]\n");
        return 2;
    }
    std::string cmd = argv[1];
    Args a = parse_args(argc, argv, 2);
    if (cmd == "selftest") return cmd_selftest();
    if (cmd == "serve") return cmd_serve(a);
    if (cmd == "client") return cmd_client(a);
    if (cmd == "ice") return cmd_ice(a);
    if (cmd == "relay") return cmd_relay(a);
    if (cmd == "wake") return cmd_wake(a);
    if (cmd == "teardown") return cmd_teardown(a);
    std::fprintf(stderr, "p2transport_test: unknown case '%s'\n", cmd.c_str());
    return 2;
}
