// M5.3b transport: the ENet socket shim (docs/online/M5_3B_TRANSPORT.md "Shim", design 7.2 and 7.4).
// Defines ENet's public enet_socket_* functions over the renamed enet_sys_socket_* of unix.c; see
// enet_juice.h for the routing rules. Host C only: no game headers.
#define _GNU_SOURCE 1
#include "enet_juice.h"

#include <juice/juice.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t mppm_steady_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------------------------------
// Synthetic addresses. ENetAddress::host is in network byte order: its first byte is the first octet.

int mppm_synth_is(const ENetAddress* a) {
    const uint8_t* b = (const uint8_t*)&a->host;
    return (b[0] & 0xF0) == 0xF0;
}

int mppm_synth_id(const ENetAddress* a) {
    const uint8_t* b = (const uint8_t*)&a->host;
    if (b[0] != 240 || b[1] != 0 || b[2] != 0 || b[3] == 0 || b[3] == 255 || a->port != MPPM_SYNTH_PORT) return 0;
    return b[3];
}

void mppm_synth_make(int id, ENetAddress* out) {
    uint8_t* b = (uint8_t*)&out->host;
    b[0] = 240; b[1] = 0; b[2] = 0; b[3] = (uint8_t)id;
    out->port = MPPM_SYNTH_PORT;
}

// ---------------------------------------------------------------------------------------------------
// Impairment.

uint32_t mppm_xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void mppm_impair_init(mppm_impair* im, uint32_t delay_us, uint32_t jitter_us, uint32_t loss_ppm,
                      uint32_t dup_ppm, uint32_t seed) {
    im->delay_us = delay_us;
    im->jitter_us = jitter_us;
    im->loss_ppm = loss_ppm > 1000000 ? 1000000 : loss_ppm;
    im->dup_ppm = dup_ppm > 1000000 ? 1000000 : dup_ppm;
    im->rng = seed ? seed : 1;
}

static uint32_t uniform_below(mppm_impair* im, uint32_t n) { // 0 .. n-1 (n >= 1)
    return (uint32_t)(((uint64_t)mppm_xorshift32(&im->rng) * n) >> 32);
}

static uint32_t draw_delay(mppm_impair* im) {
    return im->delay_us + (im->jitter_us ? uniform_below(im, im->jitter_us + 1) : 0);
}

int mppm_impair_draw(mppm_impair* im, uint32_t delays_us[2]) {
    if (im->loss_ppm && uniform_below(im, 1000000) < im->loss_ppm) return 0;
    delays_us[0] = draw_delay(im);
    if (im->dup_ppm && uniform_below(im, 1000000) < im->dup_ppm) {
        delays_us[1] = draw_delay(im);
        return 2;
    }
    return 1;
}

static int impair_on(const mppm_impair* im) {
    return im->delay_us || im->jitter_us || im->loss_ppm || im->dup_ppm;
}

// ---------------------------------------------------------------------------------------------------
// Per-socket state.

typedef struct held_dgram { // a datagram held by the impairment stage (min-heap by due, then seq)
    uint64_t due_ns, seq;
    ENetAddress to;
    size_t size;
    uint8_t* data;
} held_dgram;

typedef struct agent_dgram {
    struct agent_dgram* next;
    int id;
    uint64_t ns;
    size_t size;
    uint8_t data[];
} agent_dgram;

enum { STATS_SLOTS = 64, MPRL_SLOTS = 16, MPRL_MAX = 32, AGENT_QUEUE_MAX = 2048 };

typedef struct stats_slot {
    ENetAddress addr;
    int used;
    uint64_t touched;
    mppm_shim_addr_stats s;
} stats_slot;

struct mppm_shim {
    ENetSocket socket;
    int wake_fd, wake_is_pipe;
    mppm_impair impair;
    int impair_enabled;
    held_dgram* heap;
    size_t heap_n, heap_cap;
    uint64_t seq;
    struct juice_agent* agents[MPPM_SHIM_MAX_AGENTS];
    pthread_mutex_t qlock; // agent queue (cb_recv runs on libjuice's thread)
    agent_dgram *qhead, *qtail;
    size_t qlen;
    struct { ENetAddress from; size_t size; uint8_t data[MPRL_MAX]; } mprl[MPRL_SLOTS];
    size_t mprl_head, mprl_n;
    stats_slot stats[STATS_SLOTS];
    uint64_t stats_clock;
    uint64_t last_recv_ns;
    uint64_t send_errors;
};

static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { ENetSocket socket; mppm_shim* sh; } g_table[MPPM_SHIM_MAX_SOCKETS];

static mppm_shim* lookup(ENetSocket s) {
    mppm_shim* sh = NULL;
    pthread_mutex_lock(&g_table_lock);
    for (int i = 0; i < MPPM_SHIM_MAX_SOCKETS; i++) {
        if (g_table[i].sh && g_table[i].socket == s) { sh = g_table[i].sh; break; }
    }
    pthread_mutex_unlock(&g_table_lock);
    return sh;
}

mppm_shim* mppm_shim_attach(ENetSocket socket, int wake_fd, int wake_is_pipe) {
    mppm_shim* sh = (mppm_shim*)calloc(1, sizeof *sh);
    if (!sh) return NULL;
    sh->socket = socket;
    sh->wake_fd = wake_fd;
    sh->wake_is_pipe = wake_is_pipe;
    pthread_mutex_init(&sh->qlock, NULL);
    int ok = 0;
    pthread_mutex_lock(&g_table_lock);
    for (int i = 0; i < MPPM_SHIM_MAX_SOCKETS; i++) {
        if (!g_table[i].sh) { g_table[i].socket = socket; g_table[i].sh = sh; ok = 1; break; }
    }
    pthread_mutex_unlock(&g_table_lock);
    if (!ok) {
        pthread_mutex_destroy(&sh->qlock);
        free(sh);
        return NULL;
    }
    return sh;
}

void mppm_shim_detach(mppm_shim* sh) {
    if (!sh) return;
    pthread_mutex_lock(&g_table_lock);
    for (int i = 0; i < MPPM_SHIM_MAX_SOCKETS; i++) {
        if (g_table[i].sh == sh) { g_table[i].sh = NULL; g_table[i].socket = 0; }
    }
    pthread_mutex_unlock(&g_table_lock);
    for (size_t i = 0; i < sh->heap_n; i++) free(sh->heap[i].data);
    free(sh->heap);
    pthread_mutex_lock(&sh->qlock);
    for (agent_dgram* d = sh->qhead; d;) {
        agent_dgram* n = d->next;
        free(d);
        d = n;
    }
    sh->qhead = sh->qtail = NULL;
    pthread_mutex_unlock(&sh->qlock);
    pthread_mutex_destroy(&sh->qlock);
    free(sh);
}

void mppm_shim_set_impair(mppm_shim* sh, const mppm_impair* im) {
    if (im && impair_on(im)) {
        sh->impair = *im;
        if (!sh->impair.rng) sh->impair.rng = 1;
        sh->impair_enabled = 1;
    } else {
        memset(&sh->impair, 0, sizeof sh->impair);
        sh->impair.rng = 1;
        sh->impair_enabled = 0;
    }
}

// ---- Address statistics: a small table, the least recently touched slot is reused.

static mppm_shim_addr_stats* stats_for(mppm_shim* sh, const ENetAddress* a) {
    stats_slot* victim = &sh->stats[0];
    for (int i = 0; i < STATS_SLOTS; i++) {
        stats_slot* s = &sh->stats[i];
        if (s->used && s->addr.host == a->host && s->addr.port == a->port) {
            s->touched = ++sh->stats_clock;
            return &s->s;
        }
        if (!s->used) { if (victim->used) victim = s; }
        else if (victim->used && s->touched < victim->touched) victim = s;
    }
    memset(victim, 0, sizeof *victim);
    victim->used = 1;
    victim->addr = *a;
    victim->touched = ++sh->stats_clock;
    return &victim->s;
}

void mppm_shim_addr_stats_get(const mppm_shim* sh, const ENetAddress* a, mppm_shim_addr_stats* out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < STATS_SLOTS; i++) {
        const stats_slot* s = &sh->stats[i];
        if (s->used && s->addr.host == a->host && s->addr.port == a->port) { *out = s->s; return; }
    }
}

uint64_t mppm_shim_last_recv_ns(const mppm_shim* sh) { return sh->last_recv_ns; }
uint64_t mppm_shim_send_errors(const mppm_shim* sh) { return sh->send_errors; }

// ---- Agents.

static void wake(mppm_shim* sh) {
    if (sh->wake_fd < 0) return;
    if (sh->wake_is_pipe) {
        uint8_t b = 1;
        ssize_t r = write(sh->wake_fd, &b, 1);
        (void)r;
    } else {
        uint64_t one = 1;
        ssize_t r = write(sh->wake_fd, &one, sizeof one);
        (void)r;
    }
}

void mppm_shim_set_agent(mppm_shim* sh, int id, struct juice_agent* agent) {
    if (id <= 0 || id >= MPPM_SHIM_MAX_AGENTS) return;
    sh->agents[id] = agent;
    if (agent) return;
    pthread_mutex_lock(&sh->qlock); // drop what the old agent queued
    agent_dgram* keep_head = NULL;
    agent_dgram* keep_tail = NULL;
    size_t n = 0;
    for (agent_dgram* d = sh->qhead; d;) {
        agent_dgram* next = d->next;
        if (d->id == id) {
            free(d);
        } else {
            d->next = NULL;
            if (keep_tail) keep_tail->next = d; else keep_head = d;
            keep_tail = d;
            n++;
        }
        d = next;
    }
    sh->qhead = keep_head;
    sh->qtail = keep_tail;
    sh->qlen = n;
    pthread_mutex_unlock(&sh->qlock);
}

void mppm_shim_agent_recv(mppm_shim* sh, int id, const void* data, size_t size) {
    if (size == 0 || size > ENET_PROTOCOL_MAXIMUM_MTU) return;
    agent_dgram* d = (agent_dgram*)malloc(sizeof *d + size);
    if (!d) return;
    d->next = NULL;
    d->id = id;
    d->ns = mppm_steady_ns();
    d->size = size;
    memcpy(d->data, data, size);
    pthread_mutex_lock(&sh->qlock);
    if (sh->qlen >= AGENT_QUEUE_MAX) { // the net thread is not keeping up: drop, like a full socket buffer
        pthread_mutex_unlock(&sh->qlock);
        free(d);
        return;
    }
    if (sh->qtail) sh->qtail->next = d; else sh->qhead = d;
    sh->qtail = d;
    sh->qlen++;
    pthread_mutex_unlock(&sh->qlock);
    wake(sh);
}

int mppm_shim_agent_pending(mppm_shim* sh) {
    pthread_mutex_lock(&sh->qlock);
    int r = sh->qhead != NULL;
    pthread_mutex_unlock(&sh->qlock);
    return r;
}

static agent_dgram* agent_pop(mppm_shim* sh) {
    pthread_mutex_lock(&sh->qlock);
    agent_dgram* d = sh->qhead;
    if (d) {
        sh->qhead = d->next;
        if (!sh->qhead) sh->qtail = NULL;
        sh->qlen--;
    }
    pthread_mutex_unlock(&sh->qlock);
    return d;
}

// ---- Sending: impairment stage, then the socket or the agent.

// A send error is treated as loss: ENet aborts its whole send pass (all peers) on one failed send,
// and one unreachable candidate address must not stall the others.
static void route_now(mppm_shim* sh, const ENetAddress* to, const uint8_t* data, size_t size) {
    mppm_shim_addr_stats* st = stats_for(sh, to);
    st->sent++;
    if (mppm_synth_is(to)) {
        int id = mppm_synth_id(to);
        struct juice_agent* agent = id ? sh->agents[id] : NULL;
        // Before ICE has selected a pair (the controlled side may still be checking when the first
        // ENet datagram arrives) a send is loss; ENet retransmits.
        juice_state_t state = agent ? juice_get_state(agent) : JUICE_STATE_FAILED;
        if ((state != JUICE_STATE_CONNECTED && state != JUICE_STATE_COMPLETED) ||
            juice_send(agent, (const char*)data, size) < 0)
            sh->send_errors++;
        return;
    }
    ENetBuffer b;
    b.data = (void*)data;
    b.dataLength = size;
    if (enet_sys_socket_send(sh->socket, to, &b, 1) < 0) sh->send_errors++;
}

static int held_less(const held_dgram* a, const held_dgram* b) {
    return a->due_ns < b->due_ns || (a->due_ns == b->due_ns && a->seq < b->seq);
}

static void heap_push(mppm_shim* sh, held_dgram h) {
    if (sh->heap_n == sh->heap_cap) {
        size_t cap = sh->heap_cap ? sh->heap_cap * 2 : 64;
        held_dgram* nh = (held_dgram*)realloc(sh->heap, cap * sizeof *nh);
        if (!nh) { free(h.data); return; }
        sh->heap = nh;
        sh->heap_cap = cap;
    }
    size_t i = sh->heap_n++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!held_less(&h, &sh->heap[p])) break;
        sh->heap[i] = sh->heap[p];
        i = p;
    }
    sh->heap[i] = h;
}

static held_dgram heap_pop(mppm_shim* sh) {
    held_dgram top = sh->heap[0];
    held_dgram last = sh->heap[--sh->heap_n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const held_dgram* best = &last;
        if (l < sh->heap_n && held_less(&sh->heap[l], best)) { m = l; best = &sh->heap[l]; }
        if (r < sh->heap_n && held_less(&sh->heap[r], best)) { m = r; }
        if (m == i) break;
        sh->heap[i] = sh->heap[m];
        i = m;
    }
    if (sh->heap_n) sh->heap[i] = last;
    return top;
}

void mppm_shim_send(mppm_shim* sh, const ENetAddress* to, const void* data, size_t size) {
    if (!sh->impair_enabled) {
        route_now(sh, to, (const uint8_t*)data, size);
        return;
    }
    uint32_t delays[2];
    int copies = mppm_impair_draw(&sh->impair, delays);
    if (copies == 0) {
        stats_for(sh, to)->dropped++;
        return;
    }
    uint64_t now = mppm_steady_ns();
    for (int c = 0; c < copies; c++) {
        if (delays[c] == 0) {
            route_now(sh, to, (const uint8_t*)data, size);
            continue;
        }
        held_dgram h;
        h.due_ns = now + (uint64_t)delays[c] * 1000ull;
        h.seq = ++sh->seq;
        h.to = *to;
        h.size = size;
        h.data = (uint8_t*)malloc(size);
        if (!h.data) continue;
        memcpy(h.data, data, size);
        heap_push(sh, h);
    }
}

uint64_t mppm_shim_next_release_ns(const mppm_shim* sh) {
    return sh->heap_n ? sh->heap[0].due_ns : UINT64_MAX;
}

void mppm_shim_release_due(mppm_shim* sh, uint64_t now_ns) {
    while (sh->heap_n && sh->heap[0].due_ns <= now_ns) {
        held_dgram h = heap_pop(sh);
        route_now(sh, &h.to, h.data, h.size);
        free(h.data);
    }
}

size_t mppm_shim_pop_mprl(mppm_shim* sh, ENetAddress* from, uint8_t* buf, size_t cap) {
    if (!sh->mprl_n) return 0;
    size_t i = sh->mprl_head;
    size_t n = sh->mprl[i].size < cap ? sh->mprl[i].size : cap;
    *from = sh->mprl[i].from;
    memcpy(buf, sh->mprl[i].data, n);
    sh->mprl_head = (sh->mprl_head + 1) % MPRL_SLOTS;
    sh->mprl_n--;
    return n;
}

static void mprl_push(mppm_shim* sh, const ENetAddress* from, const uint8_t* data, size_t size) {
    if (sh->mprl_n == MPRL_SLOTS) { // full: drop the oldest
        sh->mprl_head = (sh->mprl_head + 1) % MPRL_SLOTS;
        sh->mprl_n--;
    }
    size_t i = (sh->mprl_head + sh->mprl_n) % MPRL_SLOTS;
    sh->mprl[i].from = *from;
    sh->mprl[i].size = size < MPRL_MAX ? size : MPRL_MAX;
    memcpy(sh->mprl[i].data, data, sh->mprl[i].size);
    sh->mprl_n++;
}

// ---------------------------------------------------------------------------------------------------
// ENet's public socket functions.

ENetSocket enet_socket_create(ENetSocketType type) { return enet_sys_socket_create(type); }
int enet_socket_bind(ENetSocket s, const ENetAddress* a) { return enet_sys_socket_bind(s, a); }
int enet_socket_get_address(ENetSocket s, ENetAddress* a) { return enet_sys_socket_get_address(s, a); }
int enet_socket_set_option(ENetSocket s, ENetSocketOption o, int v) { return enet_sys_socket_set_option(s, o, v); }
int enet_socket_wait(ENetSocket s, enet_uint32* c, enet_uint32 t) { return enet_sys_socket_wait(s, c, t); }
void enet_socket_destroy(ENetSocket s) { enet_sys_socket_destroy(s); }

int enet_socket_send(ENetSocket s, const ENetAddress* to, const ENetBuffer* bufs, size_t count) {
    mppm_shim* sh = lookup(s);
    if (!sh) return enet_sys_socket_send(s, to, bufs, count);
    uint8_t dgram[ENET_PROTOCOL_MAXIMUM_MTU];
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (total + bufs[i].dataLength > sizeof dgram) return -1;
        memcpy(dgram + total, bufs[i].data, bufs[i].dataLength);
        total += bufs[i].dataLength;
    }
    mppm_shim_send(sh, to, dgram, total);
    return (int)total;
}

static size_t scatter(ENetBuffer* bufs, size_t count, const uint8_t* data, size_t size) {
    size_t off = 0;
    for (size_t i = 0; i < count && off < size; i++) {
        size_t n = size - off < bufs[i].dataLength ? size - off : bufs[i].dataLength;
        memcpy(bufs[i].data, data + off, n);
        off += n;
    }
    return off;
}

int enet_socket_receive(ENetSocket s, ENetAddress* from, ENetBuffer* bufs, size_t count) {
    mppm_shim* sh = lookup(s);
    if (!sh) return enet_sys_socket_receive(s, from, bufs, count);
    agent_dgram* d = agent_pop(sh);
    if (d) {
        ENetAddress a;
        mppm_synth_make(d->id, &a);
        if (from) *from = a;
        size_t n = scatter(bufs, count, d->data, d->size);
        mppm_shim_addr_stats* st = stats_for(sh, &a);
        st->received++;
        st->last_recv_ns = d->ns;
        sh->last_recv_ns = d->ns;
        int truncated = n < d->size;
        free(d);
        return truncated ? -2 : (int)n;
    }
    for (;;) {
        ENetAddress a;
        int n = enet_sys_socket_receive(s, &a, bufs, count);
        if (n == -1) return 0; // a pending socket error: nothing to read now (see route_now)
        if (n <= 0) return n;
        uint64_t now = mppm_steady_ns();
        if (count > 0 && mprl_is((const uint8_t*)bufs[0].data, bufs[0].dataLength < (size_t)n ? bufs[0].dataLength : (size_t)n)) {
            mprl_push(sh, &a, (const uint8_t*)bufs[0].data, (size_t)n);
            continue; // never given to ENet
        }
        if (mppm_synth_is(&a)) continue; // a real datagram must not pose as an agent
        mppm_shim_addr_stats* st = stats_for(sh, &a);
        st->received++;
        st->last_recv_ns = now;
        sh->last_recv_ns = now;
        if (from) *from = a;
        return n;
    }
}
