# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## The handle and the category view

The DW1000 keeps the handle API esp-dw1000-uwb had: `dw1000_init()` fills a
handle, every chip call takes it, and `dw1000_del()` frees it. The handle
holds a `struct uwb_dev`, and `dw1000_uwb(h)` hands it out:

| Call | Does |
|---|---|
| `uwb_send(u, frame, len)` | send one frame, block until it has left |
| `uwb_recv(u, buf, cap, &len, timeout_ms)` | turn the receiver on, block for one frame; the payload without its FCS, cut to `cap` |
| `uwb_tx_timestamp(u, &ticks)` / `uwb_rx_timestamp(u, &ticks)` | the last frame's send and arrival time, antenna delay already taken off |
| `uwb_systime(u, &ticks)` | the radio's clock now |
| `uwb_ticks_between(u, start, end)` | ticks from start to end, across the counter's wrap |
| `uwb_log_config(u)` | one info line: channel, PRF, rate, preamble, address |

The dev also holds what init configured -- `channel`, `prf_mhz`,
`rate_kbps`, `preamble_len`, `addr` -- and the counter's `ts_bits` (40) and
`tick_s` (about 15.65 ps). Timestamps through `uwb_*` are written only on
success; the handle's own `dw1000_tx_timestamp()`, `dw1000_rx_timestamp()`
and `dw1000_systime()` return the value, or 0 if the read failed, as they
always did. The `struct uwb_dev *` is valid from a successful init to the
del; it is never probed or removed on its own.

The ranging exchange stays the chip's: it rests on how the DW1000 schedules
a delayed send and turns its receiver on after one.

| Call | Does |
|---|---|
| `dw1000_twr_range(h, dst, &m)` | one exchange as the initiator, with the responder whose `addr` is `dst` |
| `dw1000_twr_respond(h, &m, timeout_ms)` | one exchange as the responder; POLLs for another responder are ignored |
| `dw1000_set_antenna_delay(h, d)` | retune the calibration at run time |

Every node on a channel needs its own `addr`. How the four frames go, and
how to calibrate the antenna delay, is in
[the chip reference](chips.md#ranging).

## Buses and pins

- **ESP-IDF:** an SPI host the app has `spi_bus_initialize()`d; the driver
  adds its device on `cs_gpio`, slow until the PLL locks, then at `spi_hz`,
  and removes it at del. `rst_gpio` and `irq_gpio` are the driver's to set
  up; 0 or -1 for either means not wired.
- **HAL:** an `SPI_HandleTypeDef *` the app has `HAL_SPI_Init()`ed in mode 0
  with 8-bit frames, a chip select output, and RSTn as an open-drain output,
  or a NULL port. The driver drives chip select and RSTn; it never
  reconfigures a pin. The speed switch is the app's `spi_clock` callback;
  see [hardware](hardware.md#spi-speed). **NULL means the handle stays slow**,
  at 3 MHz or less, for good.

## Timing and threads

**Calls block.** A send waits up to 100 ms for the radio to report the frame
left; a receive up to `timeout_ms`. A wait reads `SYS_STATUS` until an event
it is waiting for is set:

- **IRQ wired** (ESP-IDF `irq_gpio`; HAL `use_irq` plus `dw1000_irq()` from
  the EXTI callback): between reads the task sleeps on the ISR's semaphore
  (ESP-IDF) or spins on the flag `dw1000_irq()` sets (HAL), until the
  deadline.
- **No IRQ:** back to back for the first 2 ms, which covers the ranging
  reply delay, then one read per yield -- a FreeRTOS tick on ESP-IDF, a
  `HAL_Delay(1)` on the HAL.

The deadline is wall-clock, `uwb_time_us()`, so SPI traffic counts.

**Locks.** On ESP-IDF each SPI transaction takes a per-handle mutex, as in
esp-dw1000-uwb, so another task may read timestamps meanwhile. On the HAL
there is no lock: one caller per device. On either, **one ranging exchange
at a time per radio**: two would receive each other's frames.

## Logging

On ESP-IDF everything goes under the log tag `uwb`, prefixed with the chip
name: `dw1000: PLL did not lock`. On the HAL every line goes through
`printf()` as `uwb: dw1000: ...`. Your own code can use the same `uwb_err()`,
`uwb_warn()`, `uwb_info()` and `uwb_dbg()` from `uwb_log.h`. `uwb_dbg()` is
compiled out on the HAL unless `CONFIG_UWB_DEBUG` is 1 (stm-forge:
`UWB_DEBUG = y`).

## Errors

`uwb_*` calls return 0 or a negative errno on both platforms. The chip calls
return `esp_err_t` on ESP-IDF, as esp-dw1000-uwb did, and errno on the HAL:

| Meaning | HAL | ESP-IDF |
|---|---|---|
| bad argument or config, NULL handle; HAL: SPI handle not mode 0 / 8-bit; ESP-IDF: `spi_hz` over 20 MHz | `-EINVAL` | `ESP_ERR_INVALID_ARG` |
| no memory for the handle or its semaphores | `-ENOMEM` | `ESP_ERR_NO_MEM` |
| no DW1000 answers: `DEV_ID` is not `0xDECA0130` | `-ENODEV` | `ESP_ERR_NOT_FOUND` |
| the PLL never locks; a frame received in error | `-EIO` | `ESP_FAIL` |
| nothing in time: a send, a receive, the peer of a ranging exchange | `-ETIMEDOUT` | `ESP_ERR_TIMEOUT` |
| a frame over 125 bytes | `-EMSGSIZE` | `ESP_ERR_INVALID_SIZE` |
| ranging: a frame from another exchange got in the way | `-EBADMSG` | `ESP_ERR_INVALID_RESPONSE` |
| ranging: this side missed its own reply slot, nothing was sent | `-ETIME` | `ESP_ERR_INVALID_STATE` |
| HAL: the SPI handle busy | `-EAGAIN` | — |
| HAL: `spi_clock` failed | its errno | — |

On ESP-IDF a bus error passes through errno on its way up, and the map is
lossy in that direction: an IDF code with no errno of its own comes back as
`ESP_FAIL`. `strerror(-ret)` names any errno.

## Changes from esp-dw1000-uwb

Both ESP-IDF headers are token-for-token the original's, plus
`dw1000_uwb()` and the `uwb.h` it brings in. Behaviour changes,
deliberately:

- **A failed SPI transfer is an error now** (an upstream bug fixed). The
  original's register read returned 0 when the transfer failed, and init
  ignored the result of every configuration write: a bus fault mid-init
  left a half-configured radio reporting `ESP_OK`, and a failed `DEV_ID`
  read showed up as "DEV_ID reads 0x00000000". Every transfer's result now
  comes back, init stops at the first failure with its code, and nothing
  is leaked.
- **Smart TX power is off** (`SYS_CFG.DIS_STXP` set). The `TX_POWER` values
  are the manual-mode ones of UM Table 19, which only mean what they say
  with smart TX power disabled; the original left it on. **This changes the
  transmit level on air.** See [the chip reference](chips.md#tx-power).
- **Channel 3's `FS_PLLTUNE` is 0x5E**, the User Manual's (Table 41), not
  the 0x56 of Decawave's reference code that the original had. Channels 1,
  2, 4, 5 and 7 are unchanged. Unconfirmed on a bench; see
  [the chip reference](chips.md#fs_plltune-on-channel-3).
- Log lines go under the tag `uwb` as `dw1000: ...`, not under `dw1000`.
- ESP-IDF 5.2 or newer, not 5.0: the shared bus layer uses the
  `i2c_master` API.
- Errors pass through errno inside the driver, so an SPI error with no errno
  of its own comes back as `ESP_FAIL` rather than the original IDF code. The
  driver's own codes are unchanged.

Ranging frames are unchanged: an esp-dw1000-uwb 0.2 board and an mcu-uwb
board range with each other.
