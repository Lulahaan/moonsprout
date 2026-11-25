// M5.3b transport: the ENet socket shim (docs/online/M5_3B_TRANSPORT.md "Shim"). Internal C interface
// between port/net/transport/enet_juice.c (which defines ENet's public enet_socket_* functions) and
// port/net/transport/transport.cpp. Not a game header.
//
// ENet's unix.c is compiled with its 8 socket functions renamed to enet_sys_socket_* (CMake fragment
// cmake/mppm_net_transport.cmake). The shim defines the public names: a socket attached here routes
// each datagram ENet sends through the impairment stage to the real UDP socket or, for a synthetic
// 240.0.0.<id>:1 address, to libjuice agent <id>; it receives the agents' datagrams before the real
// socket's, and keeps the relay's MPRL datagrams away from ENet. A socket that is not attached
// behaves exactly like ENet's own.
#pragma once

#include <enet/enet.h>
#include <stddef.h>
#include <stdint.h>

#include "mprl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct juice_agent;

// ENet's renamed platform functions (unix.c).
ENetSocket enet_sys_socket_create(ENetSocketType);
int enet_sys_socket_bind(ENetSocket, const ENetAddress*);
int enet_sys_socket_get_address(ENetSocket, ENetAddress*);
int enet_sys_socket_set_option(ENetSocket, ENetSocketOption, int);
int enet_sys_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer*, size_t);
int enet_sys_socket_receive(ENetSocket, ENetAddress*, ENetBuffer*, size_t);
int enet_sys_socket_wait(ENetSocket, enet_uint32*, enet_uint32);
void enet_sys_socket_destroy(ENetSocket);

// ---- Synthetic addresses: 240.0.0.<agent id> port 1 names libjuice agent <id> (1..254) of a socket.
enum { MPPM_SYNTH_PORT = 1, MPPM_SHIM_MAX_AGENTS = 255, MPPM_SHIM_MAX_SOCKETS = 8 };
int mppm_synth_is(const ENetAddress* a);           // any 240.0.0.0/4 address
int mppm_synth_id(const ENetAddress* a);           // agent id of a valid synthetic address, else 0
void mppm_synth_make(int id, ENetAddress* out);    // 240.0.0.<id>:1

// ---- Impairment (send side). Pure, so the self-test can draw from it.
typedef struct mppm_impair {
    uint32_t delay_us, jitter_us, loss_ppm, dup_ppm;
    uint32_t rng; // xorshift32 state, never 0
} mppm_impair;
void mppm_impair_init(mppm_impair* im, uint32_t delay_us, uint32_t jitter_us, uint32_t loss_ppm,
                      uint32_t dup_ppm, uint32_t seed);
uint32_t mppm_xorshift32(uint32_t* state);
// One datagram: returns how many copies to send (0 = dropped, 1, or 2 = duplicated) and each copy's
// delay in microseconds (delay_us + U(0, jitter_us)).
int mppm_impair_draw(mppm_impair* im, uint32_t delays_us[2]);

// ---- Per-socket shim state.
typedef struct mppm_shim mppm_shim;

typedef struct mppm_shim_addr_stats {
    uint64_t sent;          // datagrams handed to the socket or agent (after the impairment stage)
    uint64_t received;      // datagrams given to ENet
    uint64_t dropped;       // dropped by the impairment stage
    uint64_t last_recv_ns;  // steady ns of the last datagram from this address (0 = never)
} mppm_shim_addr_stats;

// Attaches an ENet socket (host->socket after enet_host_create). wake_fd: written (8 bytes, eventfd
// style, or one byte to a pipe when wake_is_pipe) whenever an agent queues a datagram. NULL when the
// table (MPPM_SHIM_MAX_SOCKETS) is full.
mppm_shim* mppm_shim_attach(ENetSocket socket, int wake_fd, int wake_is_pipe);
// Detaches and frees (held datagrams are dropped). Agents must already be unregistered.
void mppm_shim_detach(mppm_shim* sh);
void mppm_shim_set_impair(mppm_shim* sh, const mppm_impair* im); // NULL or all zero = none

// Agents (net thread only): register an agent before it can deliver; unregister it (agent NULL) after
// juice_destroy, which also drops the datagrams it queued.
void mppm_shim_set_agent(mppm_shim* sh, int id, struct juice_agent* agent);
// libjuice cb_recv (any thread): queues the datagram for enet_socket_receive and wakes.
void mppm_shim_agent_recv(mppm_shim* sh, int id, const void* data, size_t size);
int mppm_shim_agent_pending(mppm_shim* sh); // 1 when agent datagrams are queued

// Sends one datagram from the ENet socket through the impairment stage (relay REG, PUNCH).
void mppm_shim_send(mppm_shim* sh, const ENetAddress* to, const void* data, size_t size);
// Steady ns of the next held datagram, or UINT64_MAX.
uint64_t mppm_shim_next_release_ns(const mppm_shim* sh);
// Sends every held datagram due at `now_ns`.
void mppm_shim_release_due(mppm_shim* sh, uint64_t now_ns);

// MPRL datagrams that arrived on the real socket (never given to ENet), oldest first. Returns the
// length (0 = none); a datagram longer than `cap` is truncated.
size_t mppm_shim_pop_mprl(mppm_shim* sh, ENetAddress* from, uint8_t* buf, size_t cap);

void mppm_shim_addr_stats_get(const mppm_shim* sh, const ENetAddress* a, mppm_shim_addr_stats* out);
uint64_t mppm_shim_last_recv_ns(const mppm_shim* sh); // when the last datagram given to ENet arrived
uint64_t mppm_shim_send_errors(const mppm_shim* sh);  // socket send failures (treated as loss)

uint64_t mppm_steady_ns(void); // CLOCK_MONOTONIC = std::chrono::steady_clock on Linux

#ifdef __cplusplus
}
#endif
