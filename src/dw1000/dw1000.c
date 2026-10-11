/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "uwb_priv.h"

#if CONFIG_UWB_DW1000

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#include "uwb_log.h"
#include "dw1000.h"
#include "dw1000_priv.h"
#include "dw1000_regs.h"
#include "dw1000_time.h"

#define DW1000_DEV_ID 0xDECA0130UL      /* UM 7.2.2, p.65 */

/* The events a wait can end on; with an IRQ pin these drive it. */
#define IRQ_EVENTS \
    (SYS_STATUS_TXFRS | SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO)

/* Without an IRQ pin, how long a wait reads SYS_STATUS back to back before it
 * starts sleeping a tick between reads. It covers the ranging reply delay
 * (~1.5 ms, dw1000_twr.c), so a peer that answers promptly is seen at once. */
#define SPIN_US 2000

/* SPI before the PLL is up: the DW1000 takes at most 3 MHz then (DS Table
 * 18, p.26). */
#define SLOW_SPI_HZ     2000000
#define SPI_TIMEOUT_MS  10          /* HAL only; a transaction takes microseconds */

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

/*
 * One SPI transaction at a time. On ESP-IDF a FreeRTOS mutex, as
 * esp-dw1000-uwb had, so other tasks can read timestamps meanwhile; on the
 * bare-metal HAL there is no other task to hold off, so one caller per
 * device is the rule (docs/usage.md).
 */
#ifdef ESP_PLATFORM
#define LOCK(d)     xSemaphoreTake((d)->lock, portMAX_DELAY)
#define UNLOCK(d)   xSemaphoreGive((d)->lock)
#else
#define LOCK(d)     ((void)(d))
#define UNLOCK(d)   ((void)(d))
#endif

/* ---------------------------------------------------------------- SPI ---- */

/*
 * A DW1000 transaction opens with a 1 to 3 byte header (UM 2.2.1.2,
 * Figures 2, 4 and 6, p.12-13):
 *   byte 0: bit7 write, bit6 sub-address present, bits5:0 register file id
 *   byte 1: bit7 extended sub-address, bits6:0 low bits of the sub-address
 *   byte 2: bits14:7 of the sub-address
 * Payload bytes follow, least significant first, in the same chip select --
 * which is what the bus layer's transfer() is: cmd is header byte 0, tx
 * the rest of the header (and a write's payload), rx a read's payload.
 */
static size_t build_header(uint8_t *hdr, uint8_t reg, uint16_t sub, bool write)
{
    hdr[0] = (reg & 0x3F) | (write ? 0x80 : 0x00);
    if (sub == 0) {
        return 1;
    }
    hdr[0] |= 0x40;
    if (sub <= 0x7F) {
        hdr[1] = (uint8_t)sub;
        return 2;
    }
    hdr[1] = (uint8_t)(sub & 0x7F) | 0x80;
    hdr[2] = (uint8_t)(sub >> 7);
    return 3;
}

int dw_write(dw1000_handle_t d, uint8_t reg, uint16_t sub, const void *data, size_t len)
{
    uint8_t hdr[3];
    uint8_t tx[UWB_BUS_MAX_LEN];
    size_t hlen = build_header(hdr, reg, sub, true);
    int err;

    if (hlen - 1 + len > sizeof(tx))
        return -EMSGSIZE;
    memcpy(tx, hdr + 1, hlen - 1);
    memcpy(tx + hlen - 1, data, len);

    LOCK(d);
    err = d->bus.ops->transfer(&d->bus, hdr[0], tx, hlen - 1 + len, NULL, 0);
    UNLOCK(d);
    return err;
}

int dw_read(dw1000_handle_t d, uint8_t reg, uint16_t sub, void *data, size_t len)
{
    uint8_t hdr[3];
    size_t hlen = build_header(hdr, reg, sub, false);
    int err;

    LOCK(d);
    err = d->bus.ops->transfer(&d->bus, hdr[0], hdr + 1, hlen - 1, data, len);
    UNLOCK(d);
    return err;
}

int dw_write32(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return dw_write(d, reg, sub, b, 4);
}

int dw_write16(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return dw_write(d, reg, sub, b, 2);
}

int dw_write8(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint8_t v)
{
    return dw_write(d, reg, sub, &v, 1);
}

/* esp-dw1000-uwb returned 0 when the read failed; the failure now comes
 * back, and *v is left alone. */
int dw_read32(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint32_t *v)
{
    uint8_t b[4];

    TRY(dw_read(d, reg, sub, b, 4));
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
    return 0;
}

/* Reads one of the 5-byte timestamp registers into a 40-bit value. */
int dw_read40(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint64_t *v)
{
    uint8_t b[5];
    uint64_t x = 0;

    TRY(dw_read(d, reg, sub, b, 5));
    for (int i = 4; i >= 0; i--) {
        x = (x << 8) | b[i];
    }
    *v = x;
    return 0;
}

/* ------------------------------------------------------------- setup ---- */

/* The DW1000 must be clocked from the 19.2 MHz crystal, not the PLL, while
 * OTP is being read: SYSCLKS 01 (UM 7.2.50.1, p.180). */
static int clocks_to_xti(dw1000_handle_t d)
{
    uint8_t b[2];

    TRY(dw_read(d, DW_PMSC, DW_PMSC_CTRL0, b, 2));
    b[0] = (b[0] & 0xFC) | 0x01;
    return dw_write(d, DW_PMSC, DW_PMSC_CTRL0, b, 2);
}

/*
 * The leading-edge detection algorithm lives in ROM and must be copied into
 * the device on every cold start. Without it the RX timestamps are taken from
 * the coarse correlator peak instead of the true first path, which costs tens
 * of centimetres of accuracy.
 *
 * The sequence is UM Table 4 (p.24): PMSC_CTRL0 = 0x0301, LDELOAD
 * (OTP_CTRL bit 15), wait, then PMSC_CTRL0 = 0x0200, which also hands the
 * clocks back to auto.
 */
static int load_lde_microcode(dw1000_handle_t d)
{
    TRY(dw_write16(d, DW_PMSC, DW_PMSC_CTRL0, 0x0301));
    TRY(dw_write16(d, DW_OTP_IF, DW_OTP_CTRL, 0x8000)); /* LDELOAD */
    uwb_delay_us(1000); /* the UM's 150 us is a minimum; Zephyr waits 1 ms */
    return dw_write16(d, DW_PMSC, DW_PMSC_CTRL0, 0x0200);
}

/*
 * OTPRDEN | OTPREAD, then 0, then OTP_RDAT -- esp-dw1000-uwb's order. UM
 * Table 13 (p.60) writes 0x01 between and clears after the read; see
 * docs/chips.md.
 */
static int otp_read(dw1000_handle_t d, uint16_t addr, uint32_t *v)
{
    TRY(dw_write16(d, DW_OTP_IF, DW_OTP_ADDR, addr));
    TRY(dw_write8(d, DW_OTP_IF, DW_OTP_CTRL, 0x03)); /* OTPRDEN | OTPREAD */
    TRY(dw_write8(d, DW_OTP_IF, DW_OTP_CTRL, 0x00));
    return dw_read32(d, DW_OTP_IF, DW_OTP_RDAT, v);
}

static void hard_reset(dw1000_handle_t d)
{
    if (!uwb_pin_wired(&d->rst)) {
        return;
    }
    /* RSTn must be driven low then released to float -- never driven high
     * (DS 5.6.3.1, p.23). */
    uwb_pin_drive_low(&d->rst);
    uwb_delay_us(1000);
    uwb_pin_release(&d->rst);
    /* Wait out TDIG_ON, 2 ms nominal (DS Table 15, p.22), with margin. A
     * busy-wait: pdMS_TO_TICKS(5) is 0 ticks at the default 100 Hz tick rate. */
    uwb_delay_us(5000);
}

#ifdef ESP_PLATFORM
static void IRAM_ATTR dw_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(((dw1000_handle_t)arg)->irq_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* Routes IRQ_EVENTS to the IRQ pin -- active high, the SYS_CFG default that
 * dw_sys_cfg keeps -- and wakes the waiting task on its rising edge. */
static int init_irq(dw1000_handle_t d, gpio_num_t pin)
{
    d->irq_gpio = pin;
    d->irq_sem = xSemaphoreCreateBinary();
    if (!d->irq_sem) return -ENOMEM;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return uwb_errno(err);
    /* The ISR service is shared; another driver may have installed it. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return uwb_errno(err);
    err = gpio_isr_handler_add(pin, dw_isr, d);
    if (err != ESP_OK) return uwb_errno(err);
    return dw_write32(d, DW_SYS_MASK, 0, IRQ_EVENTS);
}
#endif

/* Preamble length -> the PAC size index used by the DRX_TUNE2 table
 * (UM Table 6, p.32). */
static int pac_index(uint16_t preamble_len)
{
    if (preamble_len <= 128) return 0;  /* PAC 8  */
    if (preamble_len <= 512) return 1;  /* PAC 16 */
    if (preamble_len <= 1024) return 2; /* PAC 32 */
    return 3;                           /* PAC 64 */
}

/* esp-dw1000-uwb ignored every write's result here; a failed one now stops
 * init with its errno. */
static int apply_config(dw1000_handle_t d, const dw1000_config_t *cfg)
{
    int ch = dw_chan_index(cfg->channel);
    int prf = (cfg->prf == DW_PRF_64M) ? 1 : 0;
    int rate = cfg->rate;
    int pac = pac_index(cfg->preamble_len);

    TRY(dw_write16(d, DW_AGC_CTRL, DW_AGC_TUNE1, dw_agc_tune1[prf]));
    TRY(dw_write32(d, DW_AGC_CTRL, DW_AGC_TUNE2, DW_AGC_TUNE2_VAL));
    TRY(dw_write16(d, DW_AGC_CTRL, DW_AGC_TUNE3, DW_AGC_TUNE3_VAL));

    TRY(dw_write16(d, DW_DRX_CONF, DW_DRX_TUNE0b, dw_drx_tune0b[rate]));
    TRY(dw_write16(d, DW_DRX_CONF, DW_DRX_TUNE1a, dw_drx_tune1a[prf]));
    /* DRX_TUNE1b tracks preamble length rather than data rate (UM Table 30). */
    TRY(dw_write16(d, DW_DRX_CONF, DW_DRX_TUNE1b,
                   cfg->preamble_len > 1024 ? 0x0064 : (cfg->preamble_len > 64 ? 0x0020 : 0x0010)));
    TRY(dw_write32(d, DW_DRX_CONF, DW_DRX_TUNE2, dw_drx_tune2[pac][prf]));
    /* UM Table 32, p.139. */
    TRY(dw_write16(d, DW_DRX_CONF, DW_DRX_TUNE4H, cfg->preamble_len == 64 ? 0x0010 : 0x0028));

    TRY(dw_write8(d, DW_RF_CONF, DW_RF_RXCTRLH, dw_rf_rxctrlh[ch]));
    TRY(dw_write32(d, DW_RF_CONF, DW_RF_TXCTRL, dw_rf_txctrl[ch]));
    TRY(dw_write8(d, DW_TX_CAL, DW_TC_PGDELAY, dw_tc_pgdelay[ch]));

    TRY(dw_write32(d, DW_FS_CTRL, DW_FS_PLLCFG, dw_fs_pllcfg[ch]));
    TRY(dw_write8(d, DW_FS_CTRL, DW_FS_PLLTUNE, dw_fs_plltune[ch]));

    TRY(dw_write8(d, DW_LDE_CTRL, DW_LDE_CFG1, DW_LDE_CFG1_VAL));
    TRY(dw_write16(d, DW_LDE_CTRL, DW_LDE_CFG2, dw_lde_cfg2[prf]));
    TRY(dw_write16(d, DW_LDE_CTRL, DW_LDE_REPC, dw_lde_repc_val(cfg->preamble_code, rate)));

    TRY(dw_write32(d, DW_TX_POWER, 0, dw_tx_power[ch][prf]));

    /* CHAN_CTRL (UM 7.2.32, p.108-109): TX and RX channel in the low byte,
     * then the preamble codes. RXPRF sits at bits 19:18 and the non-standard
     * SFD flag at bit 17. */
    uint32_t chan_ctrl = ((uint32_t)cfg->channel) | ((uint32_t)cfg->channel << 4);
    chan_ctrl |= ((uint32_t)(prf ? 2 : 1)) << 18;
    chan_ctrl |= ((uint32_t)cfg->preamble_code) << 22; /* TX_PCODE */
    chan_ctrl |= ((uint32_t)cfg->preamble_code) << 27; /* RX_PCODE */
    /* The standard IEEE 802.15.4 SFD is used at every rate. Decawave's
     * proprietary SFD (CHAN_CTRL bit 17) performs better at 110 kbps, but
     * selecting it also demands the non-standard DRX_TUNE0b values and a
     * USR_SFD length write. Setting the bit on its own -- as an earlier
     * version of this file did -- leaves the receiver mistuned. */
    TRY(dw_write32(d, DW_CHAN_CTRL, 0, chan_ctrl));

    /* SYS_CFG and TX_FCTRL are packed in dw1000_regs.h so the host test can
     * check the bit positions. */
    TRY(dw_write32(d, DW_SYS_CFG, 0, dw_sys_cfg(rate)));
    d->tx_fctrl_base = dw_tx_fctrl_base(rate, prf, cfg->preamble_len);

    return 0;
}

static int set_antenna_delay(dw1000_handle_t d, uint16_t delay)
{
    d->antenna_delay = delay;
    TRY(dw_write16(d, DW_TX_ANTD, 0, delay));
    return dw_write16(d, DW_LDE_CTRL, DW_LDE_RXANTD, delay);
}

/* ------------------------------------------------------------ traffic ---- */

/* Loads the transmit buffer and frame control, but does not start the radio. */
int dw_prepare_tx(dw1000_handle_t d, const void *data, size_t len)
{
    if (len + 2 > 127) return -EMSGSIZE; /* + 2 FCS bytes */
    TRY(dw_write(d, DW_TX_BUFFER, 0, data, len));
    return dw_write32(d, DW_TX_FCTRL, 0, d->tx_fctrl_base | (uint32_t)(len + 2));
}

/*
 * Waits until SYS_STATUS has a bit of `mask` set and gives that status, or 0
 * once `timeout_ms` has passed. With an IRQ the task sleeps between reads --
 * on the ISR's semaphore on ESP-IDF, on the flag dw1000_irq() sets on the
 * HAL; without one it spins for SPIN_US, then reads once per tick. The
 * deadline is wall time, so SPI traffic counts.
 */
static int wait_status(dw1000_handle_t d, uint32_t mask, uint32_t timeout_ms,
                       uint32_t *status)
{
    int64_t start = uwb_time_us();
    int64_t end = start + (int64_t)timeout_ms * 1000;

    for (;;) {
        uint32_t s;

        TRY(dw_read32(d, DW_SYS_STATUS, 0, &s));
        if (s & mask) {
            *status = s;
            return 0;
        }
        int64_t now = uwb_time_us();
        if (now >= end) {
            *status = 0;
            return 0;
        }
#ifdef ESP_PLATFORM
        if (d->irq_sem) {
            xSemaphoreTake(d->irq_sem, pdMS_TO_TICKS((end - now) / 1000) + 1);
            continue;
        }
#else
        if (d->use_irq) {
            while (!d->irq_seen && uwb_time_us() < end) {
            }
            d->irq_seen = false;
            continue;
        }
#endif
        if (now - start >= SPIN_US) {
            uwb_yield();
        }
    }
}

/* Turns the radio off and clears every event it may have raised, as Zephyr's
 * dwt_disable_txrx does. The IRQ pin is edge-triggered, so an event bit left
 * set would hold the line high and hide the next one. */
int dw_trx_off(dw1000_handle_t d)
{
    TRY(dw_write32(d, DW_SYS_CTRL, 0, SYS_CTRL_TRXOFF));
    return dw_write32(d, DW_SYS_STATUS, 0, IRQ_EVENTS | SYS_STATUS_ALL_RX_GOOD);
}

int dw_wait_tx_done(dw1000_handle_t d, uint32_t timeout_ms)
{
    uint32_t status;

    TRY(wait_status(d, SYS_STATUS_TXFRS, timeout_ms, &status));
    if (!status) {
        /* Cancel the send, or a late TXFRS would end the next wait at once
         * with a stale TX timestamp. */
        dw_trx_off(d);
        return -ETIMEDOUT;
    }
    return dw_write32(d, DW_SYS_STATUS, 0, SYS_STATUS_TXFRS);
}

int dw_send(dw1000_handle_t d, const void *data, size_t len)
{
    TRY(dw_prepare_tx(d, data, len));
    TRY(dw_write32(d, DW_SYS_CTRL, 0, SYS_CTRL_TXSTRT));
    return dw_wait_tx_done(d, 100);
}

int dw_wait_rx(dw1000_handle_t d, void *buf, size_t cap, size_t *len, uint32_t timeout_ms)
{
    uint32_t status;

    TRY(wait_status(d, SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO,
                    timeout_ms, &status));

    if (status & SYS_STATUS_RXFCG) {
        uint32_t finfo;

        TRY(dw_read32(d, DW_RX_FINFO, 0, &finfo));
        size_t n = (finfo & 0x7F); /* RXFLEN, bits 6:0 (UM 7.2.18, p.92) */
        n = (n >= 2) ? n - 2 : 0; /* drop the FCS */
        if (n > cap) n = cap;
        TRY(dw_read(d, DW_RX_BUFFER, 0, buf, n));
        /* An LDE error can come with a good frame; clear it too, or it
         * stays set: the next wait would end on it as a failure, and with an
         * IRQ pin it would hold the edge-triggered line high. */
        TRY(dw_write32(d, DW_SYS_STATUS, 0, SYS_STATUS_ALL_RX_GOOD | SYS_STATUS_ALL_RX_ERR));
        if (len) *len = n;
        return 0;
    }

    /* Errored receptions, and none in time, leave the radio idle; the caller
     * decides whether to try again. */
    TRY(dw_trx_off(d));
    /* Receiver-only soft reset: clear and set SOFTRESET bit 28 (UM p.182),
     * which Zephyr applies after every RX error and timeout. */
    TRY(dw_write8(d, DW_PMSC, DW_PMSC_SOFTRESET, 0xE0)); /* reset RX */
    TRY(dw_write8(d, DW_PMSC, DW_PMSC_SOFTRESET, 0xF0)); /* release */
    return status ? -EIO : -ETIMEDOUT;
}

int dw_recv(dw1000_handle_t d, void *buf, size_t cap, size_t *len, uint32_t timeout_ms)
{
    TRY(dw_write32(d, DW_SYS_CTRL, 0, SYS_CTRL_RXENAB));
    return dw_wait_rx(d, buf, cap, len, timeout_ms);
}

/* ------------------------------------------------- the uwb_dev ops -- */

static dw1000_handle_t of(struct uwb_dev *uwb)
{
    return uwb->priv;
}

static int op_send(struct uwb_dev *uwb, const void *frame, size_t len)
{
    return dw_send(of(uwb), frame, len);
}

static int op_recv(struct uwb_dev *uwb, void *buf, size_t cap, size_t *len,
                   uint32_t timeout_ms)
{
    return dw_recv(of(uwb), buf, cap, len, timeout_ms);
}

static int op_tx_timestamp(struct uwb_dev *uwb, uint64_t *ticks)
{
    return dw_read40(of(uwb), DW_TX_TIME, 0, ticks);
}

static int op_rx_timestamp(struct uwb_dev *uwb, uint64_t *ticks)
{
    return dw_read40(of(uwb), DW_RX_TIME, 0, ticks);
}

static int op_systime(struct uwb_dev *uwb, uint64_t *ticks)
{
    return dw_read40(of(uwb), DW_SYS_TIME, 0, ticks);
}

static const struct uwb_ops dw1000_ops = {
    .send = op_send,
    .recv = op_recv,
    .tx_timestamp = op_tx_timestamp,
    .rx_timestamp = op_rx_timestamp,
    .systime = op_systime,
};

/* -------------------------------------------------------------- init ----- */

/* Also tears down a half-built handle, so every init failure path can end
 * here. */
static void release(dw1000_handle_t d)
{
#ifdef ESP_PLATFORM
    if (d->irq_sem) {
        gpio_isr_handler_remove(d->irq_gpio);
        vSemaphoreDelete(d->irq_sem);
    }
#endif
    if (d->bus.ops)
        uwb_bus_detach(&d->bus);
#ifdef ESP_PLATFORM
    if (d->lock) vSemaphoreDelete(d->lock);
#endif
    free(d);
}

static int init(const dw1000_config_t *cfg, dw1000_handle_t *out)
{
    dw1000_handle_t d;
    uint32_t id, v;
    int err;

    if (!cfg || !out) return -EINVAL;
    if (dw_chan_index(cfg->channel) < 0) return -EINVAL;
    if (cfg->preamble_code < 1 || cfg->preamble_code > 24) return -EINVAL;
#ifdef ESP_PLATFORM
    uint32_t spi_hz = dw_fast_spi_hz(cfg->spi_hz);
    if (!spi_hz) return -EINVAL;
#else
    /* SPI mode 0 (DS Table 17, p.25), bytes. The handle is the app's, so
     * refuse a wrong one before the bus is touched. */
    if (!cfg->spi || !cfg->cs_port ||
        cfg->spi->Init.CLKPolarity != SPI_POLARITY_LOW ||
        cfg->spi->Init.CLKPhase != SPI_PHASE_1EDGE ||
        cfg->spi->Init.DataSize != SPI_DATASIZE_8BIT)
        return -EINVAL;
#endif

    d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->uwb.name = "dw1000";
    d->addr = cfg->addr;
#ifdef ESP_PLATFORM
    d->rst.gpio = cfg->rst_gpio;
    d->lock = xSemaphoreCreateMutex();
    if (!d->lock) {
        release(d);
        return -ENOMEM;
    }

    /* The DW1000 only accepts SPI above 3 MHz once the PLL is up, so start
     * slow, do the reset and identity check, then speed up. */
    err = uwb_spi_attach(&d->bus, cfg->host, cfg->cs_gpio, SLOW_SPI_HZ, 0);
#else
    d->rst.port = cfg->rst_port;
    d->rst.pin = cfg->rst_pin;
    d->use_irq = cfg->use_irq;
    /* Slow first, the app's way; with no callback the handle stays slow. */
    err = cfg->spi_clock ? cfg->spi_clock(cfg->spi_ctx, false) : 0;
    if (!err)
        err = uwb_spi_attach(&d->bus, cfg->spi, cfg->cs_port, cfg->cs_pin,
                             SPI_TIMEOUT_MS);
#endif
    if (err) {
        release(d);
        return err;
    }

    hard_reset(d);

    err = dw_read32(d, DW_DEV_ID, 0, &id);
    if (err)
        goto fail;
    if (id != DW1000_DEV_ID) {
        uwb_err(&d->uwb, "DEV_ID reads 0x%08lX, expected 0x%08lX", (unsigned long)id,
                (unsigned long)DW1000_DEV_ID);
        err = -ENODEV;
        goto fail;
    }

    /* Soft reset first, as Zephyr's dwt_initialise_dev does: SYSCLKS 01,
     * SOFTRESET to 0, then back to all ones (UM p.182). Without RSTn wired, a
     * warm MCU restart finds the chip as the previous firmware left it: a
     * sticky CPLOCK, receive timeouts, event masks. */
    err = clocks_to_xti(d);
    if (!err)
        err = dw_write16(d, DW_PMSC, DW_PMSC_CTRL1, 0x0000); /* PKTSEQ off */
    if (!err)
        err = dw_write8(d, DW_PMSC, DW_PMSC_SOFTRESET, 0x00); /* reset all */
    if (err)
        goto fail;
    uwb_delay_us(1000);
    err = dw_write8(d, DW_PMSC, DW_PMSC_SOFTRESET, 0xF0); /* release */

    /* Zephyr's dwt_initialise_dev order: OTP on the crystal clock, then the
     * LDE load, then check the PLL. PLLLDT makes that CPLOCK check reliable
     * (UM p.85). */
    if (!err)
        err = clocks_to_xti(d);
    if (!err)
        err = dw_write8(d, DW_EXT_SYNC, DW_EC_CTRL, EC_CTRL_PLLLDT);

    /* Kick the LDO tuning the factory stored, if any (UM 2.5.5.11, p.24). */
    if (!err)
        err = otp_read(d, OTP_LDOTUNE, &v);
    if (!err && (v & 0xFF))
        err = dw_write8(d, DW_OTP_IF, DW_OTP_SF, OTP_SF_LDO_KICK);

    /* Apply the factory crystal trim so both ends of a link agree on time.
     * FS_XTALT bits 7:5 must stay 011 (UM 7.2.44.5, p.150). */
    if (!err)
        err = otp_read(d, OTP_XTAL_TRIM, &v);
    if (!err) {
        uint32_t trim = v & 0x1F;
        err = dw_write8(d, DW_FS_CTRL, DW_FS_XTALT, (uint8_t)((trim ? trim : 0x10) | 0x60));
    }
    if (!err)
        err = load_lde_microcode(d);
    if (err)
        goto fail;

    /* PLL lock takes time after the clock switch, so a single read could
     * false-fail: poll every 10 us, for 1 ms or more, before giving up. */
    int tries = 100;
    for (;;) {
        err = dw_read32(d, DW_SYS_STATUS, 0, &v);
        if (err)
            goto fail;
        if ((v & SYS_STATUS_CPLOCK) || !--tries)
            break;
        uwb_delay_us(10);
    }
    if (!tries) {
        uwb_err(&d->uwb, "PLL did not lock");
        err = -EIO;
        goto fail;
    }

    err = apply_config(d, cfg);
    if (err)
        goto fail;

#ifdef ESP_PLATFORM
    uwb_bus_detach(&d->bus);
    d->bus.ops = NULL;
    err = uwb_spi_attach(&d->bus, cfg->host, cfg->cs_gpio, spi_hz, 0);
#else
    err = cfg->spi_clock ? cfg->spi_clock(cfg->spi_ctx, true) : 0;
#endif
    if (err)
        goto fail;

    err = set_antenna_delay(d, cfg->antenna_delay ? cfg->antenna_delay : 16436);
    /* Without RSTn the radio may still be busy from before; idle it and drop
     * any event left over, so the IRQ line starts low. */
    if (!err)
        err = dw_trx_off(d);
#ifdef ESP_PLATFORM
    if (!err && cfg->irq_gpio > 0)
        err = init_irq(d, cfg->irq_gpio);
#else
    if (!err && d->use_irq)
        err = dw_write32(d, DW_SYS_MASK, 0, IRQ_EVENTS);
#endif
    if (err)
        goto fail;

    d->uwb.ops = &dw1000_ops;
    d->uwb.priv = d;
    d->uwb.channel = cfg->channel;
    d->uwb.prf_mhz = cfg->prf == DW_PRF_64M ? 64 : 16;
    d->uwb.rate_kbps = cfg->rate == DW_RATE_110K ? 110 :
                       cfg->rate == DW_RATE_850K ? 850 : 6800;
    d->uwb.preamble_len = cfg->preamble_len;
    d->uwb.addr = cfg->addr;
    d->uwb.ts_bits = DW_TS_BITS;
    d->uwb.tick_s = DW_TIME_UNIT;
    *out = d;
    return 0;

fail:
    release(d);
    return err;
}

/* ------------------------------------------------------ the handle API -- */

uwb_handle_ret dw1000_init(const dw1000_config_t *cfg, dw1000_handle_t *out)
{
    return UWB_HANDLE_RET(init(cfg, out));
}

uwb_handle_ret dw1000_del(dw1000_handle_t d)
{
    if (!d) return UWB_HANDLE_RET(-EINVAL);
    release(d);
    return UWB_HANDLE_RET(0);
}

uwb_handle_ret dw1000_set_antenna_delay(dw1000_handle_t d, uint16_t delay)
{
    if (!d) return UWB_HANDLE_RET(-EINVAL);
    return UWB_HANDLE_RET(set_antenna_delay(d, delay));
}

uwb_handle_ret dw1000_send(dw1000_handle_t d, const void *data, size_t len)
{
    if (!d || !data) return UWB_HANDLE_RET(-EINVAL);
    return UWB_HANDLE_RET(dw_send(d, data, len));
}

uwb_handle_ret dw1000_recv(dw1000_handle_t d, void *buf, size_t cap, size_t *len,
                           uint32_t timeout_ms)
{
    if (!d || !buf) return UWB_HANDLE_RET(-EINVAL);
    return UWB_HANDLE_RET(dw_recv(d, buf, cap, len, timeout_ms));
}

/* 0 for a NULL handle or a failed read, as esp-dw1000-uwb gave on failure;
 * uwb_tx_timestamp() and friends say which. */
static uint64_t stamp(dw1000_handle_t d, uint8_t reg)
{
    uint64_t v = 0;

    if (d && dw_read40(d, reg, 0, &v))
        v = 0;
    return v;
}

uint64_t dw1000_tx_timestamp(dw1000_handle_t d) { return stamp(d, DW_TX_TIME); }
uint64_t dw1000_rx_timestamp(dw1000_handle_t d) { return stamp(d, DW_RX_TIME); }
uint64_t dw1000_systime(dw1000_handle_t d)      { return stamp(d, DW_SYS_TIME); }

struct uwb_dev *dw1000_uwb(dw1000_handle_t d)
{
    return d ? &d->uwb : NULL;
}

#ifndef ESP_PLATFORM
void dw1000_irq(dw1000_handle_t d)
{
    if (d)
        d->irq_seen = true;
}
#endif

#endif /* CONFIG_UWB_DW1000 */
