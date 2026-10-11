/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "uwb_priv.h"

#if CONFIG_UWB_DW1000

#include <string.h>

#include "dw1000_frame.h"
#include "dw1000_priv.h"
#include "dw1000_regs.h"
#include "dw1000_time.h"
#include "dw1000_twr.h"

/*
 * Four-frame asymmetric double-sided two-way ranging.
 *
 *   initiator                     responder
 *      t1  ------ POLL  ------>      t2
 *      t4  <----- RESP  ------       t3
 *      t5  ------ FINAL ------>      t6      (FINAL carries t1, t4, t5)
 *          <----- REPORT -----               (REPORT carries the distance)
 *
 * RESP and FINAL are sent with the delayed-transmit hardware so that the
 * reply delays are known exactly rather than being whatever the software
 * happened to take. The responder does the arithmetic because it is the only
 * side holding all six timestamps, then reports the result back so the
 * initiator gets the same number without a second exchange.
 *
 * Every frame opens with the header in dw1000_frame.h. The initiator names
 * one responder and a fresh sequence number; a frame with the wrong sender,
 * destination or sequence number is not part of this exchange and is dropped.
 */

#define MSG_POLL   0xA1
#define MSG_RESP   0xA2
#define MSG_FINAL  0xA3
#define MSG_REPORT 0xA4

/* Reply delay, in device ticks, ~1.5 ms. Each reply goes out this long after
 * the clock is read at send time, not after the arrival timestamp of the frame
 * it answers: without an IRQ pin a frame is noticed up to a tick (10 ms at the
 * IDF default) after it lands, already past a slot counted from its arrival.
 * The asymmetric double-sided formula needs neither equal nor fixed reply
 * delays, only exact ones, and those are known once the send is scheduled. */
#define REPLY_DELAY_TICKS 100000000ULL

/* How long a delayed frame is expected to take to actually leave the antenna. */
#define TX_TIMEOUT_MS 100
#define RX_TIMEOUT_MS 100

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

static void put40(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 5; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get40(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 4; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Send a reply at a scheduled time rather than immediately. The frame will
 * carry the programmed time plus the antenna delay; see tx_ts_of. WAIT4RESP
 * turns the receiver on in hardware the moment the frame is away, so the
 * answer cannot beat software to it. */
static int send_at(dw1000_handle_t d, const void *data, size_t len, uint64_t dx)
{
    TRY(dw_prepare_tx(d, data, len));

    uint8_t b[5];
    put40(b, dx);
    TRY(dw_write(d, DW_DX_TIME, 0, b, 5));

    TRY(dw_write32(d, DW_SYS_CTRL, 0, SYS_CTRL_TXSTRT | SYS_CTRL_TXDLYS | SYS_CTRL_WAIT4RESP));

    /* A delayed send whose time has already passed never fires, and the chip
     * says so at once: HPDWARN, or TXPUTE (UM p.90, p.92). Give up now
     * rather than wait out TX_TIMEOUT_MS; the warning bits are sticky, so
     * clear them for the next send. */
    uint8_t st[2];
    TRY(dw_read(d, DW_SYS_STATUS, 3, st, 2));
    if ((st[0] | st[1] << 8) & SYS_STATUS_TXERR_AT3) {
        dw_write16(d, DW_SYS_STATUS, 3, SYS_STATUS_TXERR_AT3);
        dw_trx_off(d);
        return -ETIME;
    }

    return dw_wait_tx_done(d, TX_TIMEOUT_MS);
}

/* The timestamp a frame scheduled for `dx` will actually carry. */
static inline uint64_t tx_ts_of(dw1000_handle_t d, uint64_t dx)
{
    return dw_ts_add(dx, d->antenna_delay);
}

static int twr_range(dw1000_handle_t d, uint16_t dst, float *distance_m)
{
    uint8_t frame[32];
    size_t len;
    uint64_t t1, t4, now;
    uint8_t seq = ++d->seq;

    /* --- POLL --- */
    twr_put_hdr(frame, MSG_POLL, seq, d->addr, dst);
    TRY(dw_prepare_tx(d, frame, TWR_HDR_LEN));
    /* WAIT4RESP turns the receiver on the moment the frame is away, so the
     * responder's reply is not missed while software catches up. */
    TRY(dw_write32(d, DW_SYS_CTRL, 0, SYS_CTRL_TXSTRT | SYS_CTRL_WAIT4RESP));
    TRY(dw_wait_tx_done(d, TX_TIMEOUT_MS));

    TRY(dw_read40(d, DW_TX_TIME, 0, &t1));

    /* --- RESP --- */
    TRY(dw_wait_rx(d, frame, sizeof frame, &len, RX_TIMEOUT_MS));
    if (!twr_frame_ok(frame, len, TWR_HDR_LEN, MSG_RESP, seq, dst, d->addr)) {
        return -EBADMSG;
    }

    TRY(dw_read40(d, DW_RX_TIME, 0, &t4));

    /* --- FINAL, carrying the three timestamps only the initiator knows --- */
    TRY(dw_read40(d, DW_SYS_TIME, 0, &now));
    uint64_t dx = dw_delayed_tx_time(now, REPLY_DELAY_TICKS);
    uint64_t t5 = tx_ts_of(d, dx);

    uint8_t final[TWR_HDR_LEN + 15];
    twr_put_hdr(final, MSG_FINAL, seq, d->addr, dst);
    put40(final + TWR_HDR_LEN, t1);
    put40(final + TWR_HDR_LEN + 5, t4);
    put40(final + TWR_HDR_LEN + 10, t5);

    TRY(send_at(d, final, sizeof final, dx));

    /* --- REPORT --- */
    TRY(dw_wait_rx(d, frame, sizeof frame, &len, RX_TIMEOUT_MS));
    if (!twr_frame_ok(frame, len, TWR_HDR_LEN + 4, MSG_REPORT, seq, dst, d->addr)) {
        return -EBADMSG;
    }

    if (distance_m) {
        memcpy(distance_m, frame + TWR_HDR_LEN, sizeof(float));
    }
    return 0;
}

static int twr_respond(dw1000_handle_t d, float *distance_m, uint32_t timeout_ms)
{
    uint8_t frame[32];
    size_t len;
    uint64_t t2, t6, now;

    /* --- POLL: one for another responder is ignored, and we listen on --- */
    int64_t end = uwb_time_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int64_t left = end - uwb_time_us();
        if (left <= 0) return -ETIMEDOUT;
        TRY(dw_recv(d, frame, sizeof frame, &len, (uint32_t)(left / 1000)));
        if (len >= TWR_HDR_LEN && frame[0] == MSG_POLL && twr_dst(frame) == d->addr) break;
    }

    /* The POLL fixes the sequence number and the initiator; every later frame
     * must carry both. */
    uint8_t seq = frame[1];
    uint16_t peer = twr_src(frame);
    TRY(dw_read40(d, DW_RX_TIME, 0, &t2));

    /* --- RESP --- */
    TRY(dw_read40(d, DW_SYS_TIME, 0, &now));
    uint64_t dx = dw_delayed_tx_time(now, REPLY_DELAY_TICKS);
    uint64_t t3 = tx_ts_of(d, dx);
    twr_put_hdr(frame, MSG_RESP, seq, d->addr, peer);
    TRY(send_at(d, frame, TWR_HDR_LEN, dx));

    /* --- FINAL --- */
    TRY(dw_wait_rx(d, frame, sizeof frame, &len, RX_TIMEOUT_MS));
    if (!twr_frame_ok(frame, len, TWR_HDR_LEN + 15, MSG_FINAL, seq, peer, d->addr)) {
        return -EBADMSG;
    }

    TRY(dw_read40(d, DW_RX_TIME, 0, &t6));
    uint64_t t1 = get40(frame + TWR_HDR_LEN);
    uint64_t t4 = get40(frame + TWR_HDR_LEN + 5);
    uint64_t t5 = get40(frame + TWR_HDR_LEN + 10);

    float dist = (float)dw_ticks_to_metres(dw_tof_ticks(t1, t2, t3, t4, t5, t6));

    /* --- REPORT --- */
    uint8_t report[TWR_HDR_LEN + 4];
    twr_put_hdr(report, MSG_REPORT, seq, d->addr, peer);
    memcpy(report + TWR_HDR_LEN, &dist, sizeof dist);
    TRY(dw_send(d, report, sizeof report));

    if (distance_m) *distance_m = dist;
    return 0;
}

uwb_handle_ret dw1000_twr_range(dw1000_handle_t d, uint16_t dst, float *distance_m)
{
    if (!d) return UWB_HANDLE_RET(-EINVAL);
    return UWB_HANDLE_RET(twr_range(d, dst, distance_m));
}

uwb_handle_ret dw1000_twr_respond(dw1000_handle_t d, float *distance_m, uint32_t timeout_ms)
{
    if (!d) return UWB_HANDLE_RET(-EINVAL);
    return UWB_HANDLE_RET(twr_respond(d, distance_m, timeout_ms));
}

#endif /* CONFIG_UWB_DW1000 */
