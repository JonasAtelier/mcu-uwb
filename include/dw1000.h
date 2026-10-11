/*
 * dw1000 -- the Decawave DW1000 UWB transceiver, as a UWB chip driver, for
 * ESP-IDF and the STM32 HAL.
 *
 * Low-level half: reset, configure, send and receive frames with hardware
 * timestamps. For distance measurement see dw1000_twr.h.
 *
 * Every register value behind this driver is quoted from DecaWave's DW1000
 * User Manual v2.05 and the data sheet, both in docs/; see
 * src/dw1000/dw1000_regs.h.
 *
 * The SPI bus is the app's: on ESP-IDF a host it has spi_bus_initialize()d,
 * to which the driver adds its device -- slow until the PLL locks, then at
 * spi_hz; on the STM32 HAL a handle it has HAL_SPI_Init()ed in mode 0, whose
 * speed the app changes when spi_clock asks. dw1000_uwb() hands out the
 * struct uwb_dev inside the handle.
 *
 * On ESP-IDF every call returns an esp_err_t, as esp-dw1000-uwb did; on the
 * STM32 HAL, 0 or a negative errno.
 */
#pragma once

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include CMSIS_device_header
#endif

#include "uwb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dw1000_t *dw1000_handle_t;

typedef enum { DW_RATE_110K, DW_RATE_850K, DW_RATE_6M8 } dw1000_rate_t;
typedef enum { DW_PRF_16M, DW_PRF_64M } dw1000_prf_t;

typedef struct {
#ifdef ESP_PLATFORM
    spi_host_device_t host;          /* an SPI bus you have already initialised */
    gpio_num_t        cs_gpio;
    gpio_num_t        rst_gpio;      /* RSTn pin. 0 or -1: not wired. GPIO0
                                        cannot serve as RSTn */
    gpio_num_t        irq_gpio;      /* DW1000 IRQ pin, so waits sleep instead of
                                        polling. 0 or -1: not wired, poll. GPIO0
                                        cannot serve as IRQ */
    uint32_t          spi_hz;        /* SPI clock once the PLL is up. 0 selects 8 MHz;
                                        above 20 MHz, the DW1000's limit, init refuses */
#else
    SPI_HandleTypeDef *spi;          /* HAL_SPI_Init()ed: mode 0, 8-bit frames */
    GPIO_TypeDef      *cs_port;      /* chip select: an output, idle high */
    uint16_t          cs_pin;
    GPIO_TypeDef      *rst_port;     /* RSTn: an OPEN-DRAIN output, or NULL if
                                        not wired. Never push-pull: RSTn must
                                        never be driven high */
    uint16_t          rst_pin;
    bool              use_irq;       /* the app's EXTI on the IRQ pin calls
                                        dw1000_irq(), so waits stop polling SPI */

    /* The DW1000 takes at most 3 MHz until its PLL locks, 20 MHz after. The
     * driver calls spi_clock(spi_ctx, false) before it touches the chip and
     * spi_clock(spi_ctx, true) once the PLL is up; the app sets the handle's
     * prescaler (HAL_SPI_Init() again, or CR1.BR) and returns 0 or a
     * negative errno. NULL: the app keeps the handle at 3 MHz or less all
     * the time -- always correct, only slower. */
    int             (*spi_clock)(void *spi_ctx, bool fast);
    void              *spi_ctx;
#endif

    uint8_t           channel;       /* 1, 2, 3, 4, 5 or 7 */
    dw1000_rate_t     rate;
    dw1000_prf_t      prf;
    uint16_t          preamble_len;  /* 64, 128, 256, 512, 1024, 1536, 2048 or 4096 */
    uint8_t           preamble_code; /* 1..24; must match on both ends */

    /* Antenna delay in device ticks, applied to both TX and RX. This is the
     * single knob that sets ranging accuracy: it absorbs the propagation
     * delay of the antenna and its feed, which no register default can know.
     * 0 selects 16436, a commonly used starting point. Calibrate it against a
     * known distance -- see the README. */
    uint16_t          antenna_delay;

    /* This node's address for dw1000_twr_*. Any value, 0 included; give each
     * tag and anchor sharing a channel its own. */
    uint16_t          addr;
} dw1000_config_t;

#ifdef ESP_PLATFORM
/* Reset the device, verify DEV_ID reads 0xDECA0130, load LDE microcode and
 * apply the configuration. Fails with ESP_ERR_NOT_FOUND if no DW1000 answers,
 * ESP_FAIL if its PLL does not lock. */
esp_err_t dw1000_init(const dw1000_config_t *cfg, dw1000_handle_t *out);
esp_err_t dw1000_del(dw1000_handle_t dev);

/* Send a frame and block until the radio reports it left. */
esp_err_t dw1000_send(dw1000_handle_t dev, const void *data, size_t len);

/* Enable the receiver and block for a frame. `len` receives the payload size,
 * excluding the two FCS bytes. ESP_ERR_TIMEOUT if nothing arrives in time. */
esp_err_t dw1000_recv(dw1000_handle_t dev, void *buf, size_t cap, size_t *len,
                      uint32_t timeout_ms);

/* Hardware timestamp of the last frame sent / received, in device ticks
 * (about 15.65 ps each, 40 bits wide). Both are antenna-delay corrected. */
uint64_t dw1000_tx_timestamp(dw1000_handle_t dev);
uint64_t dw1000_rx_timestamp(dw1000_handle_t dev);

/* Current value of the 40-bit system clock. */
uint64_t dw1000_systime(dw1000_handle_t dev);

/* Change the antenna delay after init, so a calibration routine can sweep it. */
esp_err_t dw1000_set_antenna_delay(dw1000_handle_t dev, uint16_t delay);
#else
/* Reset the device, verify DEV_ID reads 0xDECA0130, load LDE microcode and
 * apply the configuration. -EINVAL for a bad config or an SPI handle not in
 * mode 0 with 8-bit frames, before the bus is touched; -ENODEV if no DW1000
 * answers; -EIO if its PLL does not lock; a failed SPI transfer's errno, or
 * spi_clock's, as it comes. */
int dw1000_init(const dw1000_config_t *cfg, dw1000_handle_t *out);
int dw1000_del(dw1000_handle_t dev);

/* Send a frame and block until the radio reports it left. -EMSGSIZE past
 * 125 bytes. */
int dw1000_send(dw1000_handle_t dev, const void *data, size_t len);

/* Enable the receiver and block for a frame; -ETIMEDOUT if nothing arrives
 * in time, -EIO if one arrives in error. */
int dw1000_recv(dw1000_handle_t dev, void *buf, size_t cap, size_t *len,
                uint32_t timeout_ms);

/* Timestamps and the clock, in device ticks; 0 if the read failed. */
uint64_t dw1000_tx_timestamp(dw1000_handle_t dev);
uint64_t dw1000_rx_timestamp(dw1000_handle_t dev);
uint64_t dw1000_systime(dw1000_handle_t dev);

int dw1000_set_antenna_delay(dw1000_handle_t dev, uint16_t delay);

/* Call from the EXTI callback of the IRQ pin, with use_irq set. Only sets a
 * flag; safe from an interrupt. */
void dw1000_irq(dw1000_handle_t dev);
#endif

/* The handle as a struct uwb_dev; NULL for a NULL handle. Valid until
 * dw1000_del(). */
struct uwb_dev *dw1000_uwb(dw1000_handle_t dev);

#ifdef __cplusplus
}
#endif
