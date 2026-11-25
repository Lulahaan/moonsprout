// M5.3b builtin relay datagrams, MPRL v1 (design 5.6, server/PROTOCOL.md "Relay"): the client side's
// codec. Network byte order. Every datagram starts with "MPRL" and version 1.
//
//   REG   (24) client socket -> relay control port: token[16] at 8
//   OK    (18) relay control port -> sender: role at 8 (0 host, 1 client), relay port at 9 (0 for the
//              host), family 4 at 11, the sender's IPv4 address at 12 and port at 16 as the relay saw them
//   ERR    (9) code at 8: 1 unknown token, 2 session closed, 3 quota, 4 no free port
//   PUNCH (16) peer to peer, never to the relay: nonce[8] at 8
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MPRL_VERSION = 1,
    MPRL_REG = 1, MPRL_OK = 2, MPRL_ERR = 3, MPRL_PUNCH = 4,
    MPRL_REG_SIZE = 24, MPRL_OK_SIZE = 18, MPRL_ERR_SIZE = 9, MPRL_PUNCH_SIZE = 16,
    MPRL_ERR_UNKNOWN_TOKEN = 1, MPRL_ERR_SESSION_CLOSED = 2, MPRL_ERR_QUOTA = 3, MPRL_ERR_NO_PORT = 4,
};

typedef struct mprl_msg {
    uint8_t type;
    uint8_t token[16];  // REG
    uint8_t role;       // OK: 0 host, 1 client
    uint16_t relay_port; // OK
    uint8_t ip[4];      // OK: a.b.c.d
    uint16_t port;      // OK
    uint8_t err;        // ERR
    uint8_t nonce[8];   // PUNCH
} mprl_msg;

static inline int mprl_is(const uint8_t* p, size_t n) {
    return n >= 4 && p[0] == 'M' && p[1] == 'P' && p[2] == 'R' && p[3] == 'L';
}

static inline void mprl_header(uint8_t* out, uint8_t type) {
    out[0] = 'M'; out[1] = 'P'; out[2] = 'R'; out[3] = 'L';
    out[4] = MPRL_VERSION; out[5] = type; out[6] = 0; out[7] = 0;
}

static inline size_t mprl_encode_reg(uint8_t out[MPRL_REG_SIZE], const uint8_t token[16]) {
    mprl_header(out, MPRL_REG);
    memcpy(out + 8, token, 16);
    return MPRL_REG_SIZE;
}

static inline size_t mprl_encode_ok(uint8_t out[MPRL_OK_SIZE], uint8_t role, uint16_t relay_port,
                                    const uint8_t ip[4], uint16_t port) {
    mprl_header(out, MPRL_OK);
    out[8] = role;
    out[9] = (uint8_t)(relay_port >> 8); out[10] = (uint8_t)relay_port;
    out[11] = 4;
    memcpy(out + 12, ip, 4);
    out[16] = (uint8_t)(port >> 8); out[17] = (uint8_t)port;
    return MPRL_OK_SIZE;
}

static inline size_t mprl_encode_err(uint8_t out[MPRL_ERR_SIZE], uint8_t code) {
    mprl_header(out, MPRL_ERR);
    out[8] = code;
    return MPRL_ERR_SIZE;
}

static inline size_t mprl_encode_punch(uint8_t out[MPRL_PUNCH_SIZE], const uint8_t nonce[8]) {
    mprl_header(out, MPRL_PUNCH);
    memcpy(out + 8, nonce, 8);
    return MPRL_PUNCH_SIZE;
}

// 0 and `out` filled for a well-formed v1 datagram of a known type with its exact size; -1 otherwise.
static inline int mprl_decode(const uint8_t* p, size_t n, mprl_msg* out) {
    if (n < 8 || !mprl_is(p, n) || p[4] != MPRL_VERSION) return -1;
    memset(out, 0, sizeof *out);
    out->type = p[5];
    switch (p[5]) {
    case MPRL_REG:
        if (n != MPRL_REG_SIZE) return -1;
        memcpy(out->token, p + 8, 16);
        return 0;
    case MPRL_OK:
        if (n != MPRL_OK_SIZE || p[11] != 4 || p[8] > 1) return -1;
        out->role = p[8];
        out->relay_port = (uint16_t)((p[9] << 8) | p[10]);
        memcpy(out->ip, p + 12, 4);
        out->port = (uint16_t)((p[16] << 8) | p[17]);
        return 0;
    case MPRL_ERR:
        if (n != MPRL_ERR_SIZE) return -1;
        out->err = p[8];
        return 0;
    case MPRL_PUNCH:
        if (n != MPRL_PUNCH_SIZE) return -1;
        memcpy(out->nonce, p + 8, 8);
        return 0;
    default:
        return -1;
    }
}

static inline const char* mprl_err_name(uint8_t code) {
    switch (code) {
    case MPRL_ERR_UNKNOWN_TOKEN: return "unknown token";
    case MPRL_ERR_SESSION_CLOSED: return "session closed";
    case MPRL_ERR_QUOTA: return "quota";
    case MPRL_ERR_NO_PORT: return "no free port";
    default: return "unknown error";
    }
}

#ifdef __cplusplus
}
#endif
