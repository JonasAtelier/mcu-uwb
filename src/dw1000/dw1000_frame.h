/*
 * Header carried by every ranging frame, so several tags and anchors can
 * share a channel and a stale frame from an earlier exchange is dropped.
 *
 *   byte 0     message type
 *   byte 1     sequence number, picked by the initiator per exchange
 *   bytes 2-3  source address, little-endian
 *   bytes 4-5  destination address, little-endian
 *
 * Kept free of ESP-IDF headers so the accept rule is checked on the host;
 * see test/test_dw1000_time.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TWR_HDR_LEN 6

static inline void twr_put_hdr(uint8_t *f, uint8_t type, uint8_t seq, uint16_t src, uint16_t dst)
{
    f[0] = type;
    f[1] = seq;
    f[2] = (uint8_t)src;
    f[3] = (uint8_t)(src >> 8);
    f[4] = (uint8_t)dst;
    f[5] = (uint8_t)(dst >> 8);
}

static inline uint16_t twr_src(const uint8_t *f) { return (uint16_t)(f[2] | f[3] << 8); }
static inline uint16_t twr_dst(const uint8_t *f) { return (uint16_t)(f[4] | f[5] << 8); }

/* True if `f`, `len` bytes long, is a `type` frame of at least `need` bytes
 * belonging to this exchange: sequence `seq`, sent by `src` to `dst`. */
static inline bool twr_frame_ok(const uint8_t *f, size_t len, size_t need, uint8_t type,
                                uint8_t seq, uint16_t src, uint16_t dst)
{
    return len >= need && len >= TWR_HDR_LEN && f[0] == type && f[1] == seq &&
           twr_src(f) == src && twr_dst(f) == dst;
}
