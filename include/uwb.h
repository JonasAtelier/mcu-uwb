/*
 * uwb -- a small Linux-IIO-style UWB transceiver subsystem for ESP-IDF and
 * the STM32 HAL.
 *
 * One struct uwb_dev per radio. It holds only what a UWB ranging
 * transceiver shares: send a frame, receive one, and the hardware
 * timestamps of both against the radio's own clock -- the raw material of
 * every ranging scheme -- plus the configuration init settled on. The
 * arithmetic that turns six timestamps into a distance is the same for any
 * such radio, and lives in src/uwb_math.h, free of platform headers so the
 * host tests check it.
 *
 * The ranging exchange itself stays with each chip (dw1000_twr.h): it rests
 * on how a chip schedules a delayed send and turns its receiver on after
 * one, and with one chip here there is no second shape to share. Each chip
 * keeps its own handle API too; the handle holds a struct uwb_dev, and
 * dw1000_uwb() hands it out. The handle's init fills it and its del ends
 * it: there is no probe or remove of its own.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef UWB_H
#define UWB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uwb_dev;

struct uwb_ops {
	/* send a frame and block until the radio reports it left */
	int (*send)(struct uwb_dev *uwb, const void *frame, size_t len);
	/* receive one, its payload size in *len, at most timeout_ms away */
	int (*recv)(struct uwb_dev *uwb, void *buf, size_t cap, size_t *len,
		    uint32_t timeout_ms);
	/* the last frame's send / arrival time, and the clock now, in ticks */
	int (*tx_timestamp)(struct uwb_dev *uwb, uint64_t *ticks);
	int (*rx_timestamp)(struct uwb_dev *uwb, uint64_t *ticks);
	int (*systime)(struct uwb_dev *uwb, uint64_t *ticks);
};

struct uwb_dev {
	const char *name;		/* "dw1000"; log prefix */
	const struct uwb_ops *ops;
	void *priv;			/* the chip's handle */

	/* What init configured; read only. */
	uint8_t channel;		/* IEEE 802.15.4 UWB channel */
	uint8_t prf_mhz;		/* mean pulse repetition frequency */
	uint16_t rate_kbps;		/* data rate: 110, 850 or 6800 */
	uint16_t preamble_len;		/* symbols */
	uint16_t addr;			/* this node's ranging address */

	/* The timestamp counter: its width, and one tick in seconds. */
	uint8_t ts_bits;
	double tick_s;
};

/* Send one frame and block until it has left. */
int uwb_send(struct uwb_dev *uwb, const void *frame, size_t len);

/*
 * Turn the receiver on and block for one frame, at most timeout_ms. *len
 * is its payload size, without the FCS; a frame longer than cap is cut to
 * cap. -ETIMEDOUT if nothing came, -EIO if something came in error.
 */
int uwb_recv(struct uwb_dev *uwb, void *buf, size_t cap, size_t *len,
	     uint32_t timeout_ms);

/*
 * Hardware timestamps, in ticks of tick_s seconds, ts_bits wide: the last
 * frame's send and arrival, antenna delay already taken off, and the
 * radio's clock now. *ticks is written only on success.
 */
int uwb_tx_timestamp(struct uwb_dev *uwb, uint64_t *ticks);
int uwb_rx_timestamp(struct uwb_dev *uwb, uint64_t *ticks);
int uwb_systime(struct uwb_dev *uwb, uint64_t *ticks);

/* Ticks from start to end on this radio's counter, across its wrap; 0 for
 * a NULL uwb. */
uint64_t uwb_ticks_between(const struct uwb_dev *uwb, uint64_t start,
			   uint64_t end);

/*
 * The clock the library's own waits run on, in microseconds. On ESP-IDF it
 * is esp_timer_get_time(). On the STM32 HAL the default is weak and only
 * HAL_GetTick() * 1000; define your own uwb_time_us() for real
 * microseconds.
 */
int64_t uwb_time_us(void);

#ifdef __cplusplus
}
#endif

#endif /* UWB_H */
