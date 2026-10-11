/*
 * Timestamp arithmetic and the two-way-ranging time-of-flight formula, for
 * any UWB radio that stamps frames on a wrapping counter. Kept free of
 * platform headers so it can be compiled and asserted on the host. Each
 * chip's _time.h keeps its own names for these, as aliases.
 */
#pragma once

#include <stdint.h>

/* Speed of light in air, m/s. */
#define UWB_SPEED_OF_LIGHT 299702547.0

/* All ones in the low `bits` bits: the counter's range. */
static inline uint64_t uwb_ts_mask(unsigned bits)
{
    return bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
}

/* Elapsed ticks from `start` to `end`, correct across a counter wrap. */
static inline uint64_t uwb_ts_diff(uint64_t end, uint64_t start, unsigned bits)
{
    return (end - start) & uwb_ts_mask(bits);
}

/* Add ticks to a timestamp, wrapping into the counter's range. */
static inline uint64_t uwb_ts_add(uint64_t ts, uint64_t delta, unsigned bits)
{
    return (ts + delta) & uwb_ts_mask(bits);
}

/*
 * Asymmetric double-sided two-way ranging.
 *
 *   initiator            responder
 *      t1  --- poll --->    t2
 *      t4  <-- resp ---     t3
 *      t5  --- final -->    t6
 *
 * Double-sided cancels the clock-frequency offset between the two devices,
 * which single-sided ranging cannot do; the asymmetric form additionally
 * removes the requirement that the two reply delays match.
 *
 * Returns time of flight in ticks. Every subtraction goes through
 * uwb_ts_diff so an exchange that straddles a counter wrap still works.
 */
static inline double uwb_ds_twr_tof(uint64_t t1, uint64_t t2, uint64_t t3,
                                    uint64_t t4, uint64_t t5, uint64_t t6,
                                    unsigned bits)
{
    double round1 = (double)uwb_ts_diff(t4, t1, bits); /* initiator: poll sent -> resp seen  */
    double reply1 = (double)uwb_ts_diff(t3, t2, bits); /* responder: poll seen -> resp sent  */
    double round2 = (double)uwb_ts_diff(t6, t3, bits); /* responder: resp sent -> final seen */
    double reply2 = (double)uwb_ts_diff(t5, t4, bits); /* initiator: resp seen -> final sent */

    return (round1 * round2 - reply1 * reply2) / (round1 + round2 + reply1 + reply2);
}

/* Ticks of tick_s seconds, as a distance through air. */
static inline double uwb_ticks_to_m(double ticks, double tick_s)
{
    return ticks * tick_s * UWB_SPEED_OF_LIGHT;
}
