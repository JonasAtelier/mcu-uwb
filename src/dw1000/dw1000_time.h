/*
 * DW1000 timestamp arithmetic and the two-way-ranging time-of-flight formula.
 *
 * Kept free of platform headers so it can be compiled and asserted on the
 * host; see test/test_dw1000_time.c. The arithmetic is the one any UWB
 * radio shares, in uwb_math.h; these are its DW1000 names, on the DW1000's
 * 40-bit counter, which esp-dw1000-uwb's own test uses. The 40-bit
 * wraparound handled here is the classic source of wild outlier ranges, so
 * it is worth testing directly.
 */
#pragma once

#include <stdint.h>

#include "uwb_math.h"

/* The device counter is 40 bits wide and wraps roughly every 17.2 seconds
 * (UM 7.2.8, p.73). */
#define DW_TS_BITS 40
#define DW_TS_MASK 0xFFFFFFFFFFULL

/* One counter tick, in seconds: 1 / (128 * 499.2 MHz) (UM p.73, p.99). */
#define DW_TIME_UNIT 15.650040064103e-12

/* Speed of light in air, m/s. */
#define DW_SPEED_OF_LIGHT UWB_SPEED_OF_LIGHT

/* Elapsed ticks from `start` to `end`, correct across a counter wrap. */
static inline uint64_t dw_ts_diff(uint64_t end, uint64_t start)
{
    return uwb_ts_diff(end, start, DW_TS_BITS);
}

/* Add ticks to a timestamp, wrapping into the 40-bit range. */
static inline uint64_t dw_ts_add(uint64_t ts, uint64_t delta)
{
    return uwb_ts_add(ts, delta, DW_TS_BITS);
}

/* Asymmetric double-sided two-way ranging; see uwb_ds_twr_tof(). Returns
 * time of flight in device ticks. */
static inline double dw_tof_ticks(uint64_t t1, uint64_t t2, uint64_t t3,
                                  uint64_t t4, uint64_t t5, uint64_t t6)
{
    return uwb_ds_twr_tof(t1, t2, t3, t4, t5, t6, DW_TS_BITS);
}

static inline double dw_ticks_to_metres(double ticks)
{
    return uwb_ticks_to_m(ticks, DW_TIME_UNIT);
}

/*
 * Round a target transmit time down to the 512-tick boundary the delayed-send
 * hardware actually uses: DX_TIME ignores the low 9 bits of the timestamp
 * (UM 7.2.12, p.77).
 */
static inline uint64_t dw_delayed_tx_time(uint64_t base, uint64_t delay_ticks)
{
    return dw_ts_add(base, delay_ticks) & ~0x1FFULL;
}
