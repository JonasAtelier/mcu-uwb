/*
 * Host-side check of the timestamp arithmetic and the ranging formula:
 *
 *     cc -Isrc -o /tmp/t test/test_dw1000_time.c -lm && /tmp/t
 *
 * The 40-bit counter wraps about every 17.2 seconds, so roughly one exchange
 * in a few thousand straddles a wrap. Getting that wrong yields a plausible
 * looking but wildly wrong distance, which is why it is tested here directly.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../src/dw1000/dw1000_frame.h"
#include "../src/dw1000/dw1000_regs.h"
#include "../src/dw1000/dw1000_time.h"

#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) < (tol))

/* Builds a synthetic exchange with a known time of flight and reply delays. */
static double round_trip(uint64_t start, double tof, uint64_t reply1, uint64_t reply2)
{
    uint64_t t = (uint64_t)tof;
    uint64_t t1 = start;
    uint64_t t2 = dw_ts_add(t1, t);
    uint64_t t3 = dw_ts_add(t2, reply1);
    uint64_t t4 = dw_ts_add(t3, t);
    uint64_t t5 = dw_ts_add(t4, reply2);
    uint64_t t6 = dw_ts_add(t5, t);
    return dw_tof_ticks(t1, t2, t3, t4, t5, t6);
}

int main(void)
{
    /* --- wraparound in the primitives --- */
    assert(dw_ts_diff(10, 5) == 5);
    assert(dw_ts_diff(5, 10) == DW_TS_MASK - 4);        /* wrapped */
    assert(dw_ts_diff(4, DW_TS_MASK) == 5);             /* crossed zero */
    assert(dw_ts_add(DW_TS_MASK, 1) == 0);
    assert(dw_ts_add(DW_TS_MASK, 3) == 2);

    /* --- delayed transmit drops the low 9 bits --- */
    assert((dw_delayed_tx_time(0, 1000) & 0x1FF) == 0);
    assert(dw_delayed_tx_time(0, 1023) == 512);
    assert(dw_delayed_tx_time(0, 511) == 0);

    /* --- the ranging formula recovers the time of flight exactly --- */
    double tof = round_trip(1000, 3335, 100000000ULL, 100000000ULL);
    assert(NEAR(tof, 3335, 1e-6));

    /* Asymmetric reply delays must not bias the result; that is the whole
     * point of choosing the asymmetric double-sided form. */
    tof = round_trip(1000, 3335, 100000000ULL, 250000000ULL);
    assert(NEAR(tof, 3335, 1e-6));

    /* An exchange that straddles the 40-bit wrap gives the same answer. */
    tof = round_trip(DW_TS_MASK - 500, 3335, 100000000ULL, 250000000ULL);
    assert(NEAR(tof, 3335, 1e-6));
    tof = round_trip(DW_TS_MASK - 150000000ULL, 3335, 100000000ULL, 250000000ULL);
    assert(NEAR(tof, 3335, 1e-6));

    /* --- ticks to metres --- */
    /* 3335 ticks * 15.65 ps * c is a bit over 15 m. */
    assert(NEAR(dw_ticks_to_metres(3335), 15.65, 0.02));
    /* One tick is about 4.7 mm, the quantisation floor of the whole scheme. */
    assert(NEAR(dw_ticks_to_metres(1), 0.00469, 1e-5));

    /* End to end: a 10 m link should measure as 10 m. */
    double ticks = 10.0 / (DW_TIME_UNIT * DW_SPEED_OF_LIGHT);
    tof = round_trip(1000, floor(ticks), 100000000ULL, 100000000ULL);
    assert(NEAR(dw_ticks_to_metres(tof), 10.0, 0.01));

    /* --- register packing: TXBR 14:13, TXPRF 17:16, TXPSR 19:18, PE 21:20 --- */
    /* 6.8 Mbps, PRF 64 MHz, preamble 128 (channel has no bits in either). */
    assert(dw_tx_fctrl_base(2, 1, 128) == 0x00164000UL);
    /* SYS_CFG keeps its reset default 0x00001200 (DIS_DRXB, HIRQ_POL), plus
     * DIS_STXP, bit 18, so the manual TX power values apply. */
    assert(dw_sys_cfg(2) == 0x00041200UL);
    /* 110 kbps, PRF 16 MHz, preamble 1024: RXM110K is SYS_CFG bit 22. */
    assert(dw_tx_fctrl_base(0, 0, 1024) == 0x00090000UL);
    assert(dw_sys_cfg(0) == 0x00441200UL);
    /* LDE_REPC is divided by 8 at 110 kbps: code 9 is 0x28F4, so 0x051E. */
    assert(dw_lde_repc_val(9, 0) == 0x051E);
    assert(dw_lde_repc_val(9, 2) == 0x28F4);

    /* --- fast SPI clock: 0 means 8 MHz, the chip takes at most 20 MHz --- */
    assert(dw_fast_spi_hz(0) == 8000000UL);
    assert(dw_fast_spi_hz(20000000UL) == 20000000UL);
    assert(dw_fast_spi_hz(20000001UL) == 0);

    /* --- delayed-send errors: HPDWARN, TXPUTE as a 16-bit read at offset 3 --- */
    assert(SYS_STATUS_TXERR_AT3 == ((SYS_STATUS_HPDWARN | SYS_STATUS_TXPUTE) >> 24));

    /* --- ranging frame header: a frame from another exchange is dropped --- */
    uint8_t f[TWR_HDR_LEN + 4];
    twr_put_hdr(f, 0xA2, 7, 0x0102, 0x0304); /* type 0xA2, seq 7, 0x0102 -> 0x0304 */
    assert(twr_dst(f) == 0x0304 && twr_src(f) == 0x0102);
    assert(twr_frame_ok(f, sizeof f, sizeof f, 0xA2, 7, 0x0102, 0x0304));
    assert(!twr_frame_ok(f, sizeof f, sizeof f, 0xA2, 8, 0x0102, 0x0304)); /* stale seq */
    assert(!twr_frame_ok(f, sizeof f, sizeof f, 0xA2, 7, 0x0999, 0x0304)); /* other sender */
    assert(!twr_frame_ok(f, sizeof f, sizeof f, 0xA2, 7, 0x0102, 0x0999)); /* not for us */
    assert(!twr_frame_ok(f, sizeof f, sizeof f, 0xA3, 7, 0x0102, 0x0304)); /* wrong type */
    assert(!twr_frame_ok(f, sizeof f - 1, sizeof f, 0xA2, 7, 0x0102, 0x0304)); /* short */

    printf("dw1000 time/ranging math: all checks passed\n");
    return 0;
}
