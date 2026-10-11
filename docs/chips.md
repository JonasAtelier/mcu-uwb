# Chip reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## DW1000

Decawave's (now Qorvo's) IEEE 802.15.4 UWB transceiver, usually met as the
DWM1000 module. SPI, a 40-bit clock of about 15.65 ps per tick, and a
hardware timestamp on every frame sent and received: the raw material of
time-of-flight ranging. Documents, both in `docs/` (gitignored): the DW1000
Data Sheet ("DS") and the DW1000 User Manual v2.05 ("UM"), which holds the
register map.

| Function | Does |
|---|---|
| `dw1000_init(&cfg, &h)` | reset, check `DEV_ID == 0xDECA0130`, load the LDE microcode, configure, speed SPI up |
| `dw1000_send` / `dw1000_recv` | one raw frame out or in |
| `dw1000_tx_timestamp` / `dw1000_rx_timestamp` / `dw1000_systime` | 40-bit ticks, antenna delay already taken off |
| `dw1000_set_antenna_delay(h, d)` | retune the calibration at run time |
| `dw1000_twr_range(h, dst, &m)` / `dw1000_twr_respond(h, &m, ms)` | one ranging exchange, either side |
| `dw1000_uwb(h)` | the handle as a `struct uwb_dev` |
| `dw1000_irq(h)` | HAL only: the IRQ pin's EXTI callback calls it |
| `dw1000_del(h)` | free |

### Ranging

```
initiator                     responder
   t1  ------ POLL  ------>      t2
   t4  <----- RESP  ------       t3
   t5  ------ FINAL ------>      t6     (FINAL carries t1, t4, t5)
       <----- REPORT -----              (REPORT carries the distance)

every frame: type, seq, src addr, dst addr  (6 bytes, src/dw1000/dw1000_frame.h)

tof = (round1*round2 - reply1*reply2) / (round1 + round2 + reply1 + reply2)
```

Double-sided ranging cancels the clock-frequency offset between the two
crystals, which single-sided cannot; the asymmetric form also drops any
requirement that the two reply delays match. The responder holds all six
timestamps, so it does the arithmetic and reports the result back.

The header lets tags and anchors share a channel: a responder answers only
POLLs addressed to it, and both sides drop a frame whose sender or sequence
number does not belong to the exchange in progress (`-EBADMSG`).

`RESP` and `FINAL` go out with the delayed-transmit hardware, so the reply
delays are exact rather than however long software took. Each is scheduled
1.5 ms after the clock is read at send time, not after the frame it answers
arrived, so a frame noticed a tick late still gets its slot; the formula
needs exact reply delays, not equal or fixed ones. `DX_TIME` ignores its low
9 bits (UM 7.2.12, p.77), so a slot is rounded down to 512 ticks, and the
frame's timestamp is that time plus the antenna delay. A slot already past
is reported by the chip at once (HPDWARN or TXPUTE, UM p.90 and p.92): the
exchange ends with `-ETIME` and nothing is sent. The 1.5 ms must also hold
the preamble, about 1 µs a symbol: up to 1024 symbols fit at any rate,
1536, 2048 and 4096 never do.

The 40-bit counter wraps every ~17.2 s; every subtraction goes through the
wrap-safe difference (`src/uwb_math.h`), and that is tested.

### Antenna delay calibration

The knob that decides whether a distance is right. No register default can
set it: it absorbs the propagation delay of the antenna and its feed. The
symptom of a wrong value is every reading off by the *same* constant, at
every distance.

1. Two boards a known distance apart, 5–10 m, clear line of sight.
2. Range repeatedly and average.
3. One tick is about **4.7 mm**, so
   `new_delay = old_delay + (reported_m - actual_m) / 0.0047`.
4. Re-measure at a second distance, to confirm the offset went rather than
   turning into a slope error.

`dw1000_set_antenna_delay()` sweeps it without reflashing. 16436 is a
common starting point, not an answer.

### Start-up

`dw1000_init()`, in order, as Zephyr's `dwt_initialise_dev` does it:

1. Attach SPI slow: 2 MHz on ESP-IDF; on the HAL `spi_clock(ctx, false)`
   first, if there is one. The DW1000 takes at most 3 MHz before its PLL
   locks (DS Table 18, p.26).
2. Pulse RSTn low for 1 ms, if wired, then let it go -- never drive it high
   (DS 5.6.3.1, p.23) -- and wait out TDIG_ON, 2 ms nominal (DS Table 15,
   p.22), with margin: 5 ms.
3. Read `DEV_ID`, 0xDECA0130 (UM 7.2.2, p.65); anything else is `-ENODEV`.
4. Soft reset: clocks to the crystal, `SOFTRESET` to 0 and back (UM p.182).
   Without RSTn wired, a warm MCU restart otherwise finds the chip as the
   last firmware left it.
5. PLL lock detect on (`EC_CTRL.PLLLDT`, UM 7.2.37.1, p.118; p.85 says why).
6. From OTP (UM Table 10, p.58): kick the factory LDO tuning if there is
   one (`OTP_SF.LDO_KICK`), and apply the crystal trim (`FS_XTALT`, bits
   7:5 kept at 011, UM 7.2.44.5, p.150).
7. Load the LDE microcode (UM Table 4, p.24). Without it receive timestamps
   come from the correlator peak instead of the first path, tens of
   centimetres off.
8. Wait for `CPLOCK`, polling every 10 µs, 100 tries; none is `-EIO`.
9. The tuning tables for the channel, PRF, rate and preamble, `CHAN_CTRL`,
   `SYS_CFG` and `TX_POWER` (below).
10. SPI fast: on ESP-IDF the device again at `spi_hz`; on the HAL
    `spi_clock(ctx, true)`. **With no callback the handle stays as the app
    set it**, which must then be 3 MHz or less.
11. The antenna delay, both sides; the radio idled and every event cleared,
    so an edge-triggered IRQ line starts low; the IRQ events routed to the
    pin if it is wired.

Every SPI transfer's result is checked; the first failure ends init with its
code and frees what init had set up.

### SPI

A transaction opens with a 1 to 3 byte header (UM 2.2.1.2, Figures 2, 4 and
6, p.12–13) -- write bit, sub-address flag, the 6-bit register file id, then
up to 15 bits of sub-address -- and the payload follows, least significant
byte first, under one chip select. That is the bus layer's `transfer()`:
`cmd` is header byte 0, `tx` the rest of the header and a write's payload,
`rx` a read's payload. A transaction is at most 3 header bytes and a
127-byte frame; the bus layer holds 160 bytes, as esp-dw1000-uwb did
(`UWB_BUS_MAX_LEN`, 159 after the command byte).

### TX power

`TX_POWER` holds UM Table 19 (p.107), the reference values for **manual**
transmit power control, which "apply when smart TX power is disabled (i.e.
DIS_STXP=1)". esp-dw1000-uwb wrote Table 19 but left `SYS_CFG.DIS_STXP`
(bit 18, UM p.71) at its reset value, 0: smart TX power on, under which the
chip reads the register as Table 18's per-frame-length levels. The driver
now sets `DIS_STXP`, so the register means what Table 19 says. **This
changes the transmit level on air** at 6.8 Mbps, the only rate smart TX
power acts at. Table 19's values are a starting point for calibration,
designed for -41.3 dBm/MHz at the antenna through 1.5 dB of loss (p.107);
your board's loss is your own.

### FS_PLLTUNE on channel 3

The UM gives `FS_PLLTUNE` 0x5E for channel 3 (Table 41, p.149).
esp-dw1000-uwb had 0x56, the value in Decawave's reference code, which its
README records as cross-checked against Zephyr's driver. This library
follows the manual. **Unconfirmed on a bench:** if channel 3 fails to lock or
ranges badly while channel 5 works, this is the first value to try the
other way. Channels 1, 2, 4, 5 and 7 agree between the manual and the original.

### OTP read order

The driver reads OTP in esp-dw1000-uwb's order: `OTP_ADDR`, then
`OTP_CTRL` = 0x03 (OTPRDEN | OTPREAD), then `OTP_CTRL` = 0x00, then the
4-byte `OTP_RDAT`. UM Table 13 (p.60), "an example", writes 0x03, then 0x01,
reads `OTP_RDAT` with a data length of 1, and only then writes 0x00. The
driver's order is kept, as a reviewed decision rather than a checked one:
neither order has been run here. Table 13 also gives `OTP_RDAT`'s
address as "0x2A.04"; it is 0x2D:0A (UM 7.2.46.5, p.164), in the OTP
interface register file with the rest of the sequence.

### Registers

Every value in `src/dw1000/dw1000_regs.h` names its UM section or table and
page: register ids from UM Table 15 (p.62–63), each sub-address and bit from
its 7.2.x entry, and the tuning tables from UM Tables 6, 16, 19, 22–24,
28–32, 34, 35, 37, 40, 41, 47 and 48. esp-dw1000-uwb wrote them without the
manual and cross-checked them against Zephyr's driver; checked again here
against the manual, every value agrees except the two above, `FS_PLLTUNE`
on channel 3 and the `TX_POWER` mode.

Left alone, as the original had them:

- **The standard IEEE 802.15.4 SFD at every rate.** Decawave's proprietary
  SFD (`CHAN_CTRL` bit 17) is better at 110 kbps, but needs the
  non-standard `DRX_TUNE0b` values and a `USR_SFD` length write with it.
- **Single RX buffer** (`SYS_CFG.DIS_DRXB`, the reset default) and no
  receive wait timeout: the software wait bounds every receive.

## Provenance

Ported from esp-dw1000-uwb 0.2.0 (6fbbfc8). The driver code is the
original's, moved onto this library's bus layer and errno, with the changes
listed in [API usage](usage.md#changes-from-esp-dw1000-uwb). The timestamp
and ranging maths (`dw1000_time.h`) are aliases of `src/uwb_math.h`, so the
original's maths test runs with its include paths changed and two
expectations updated, both `SYS_CFG` values now carrying `DIS_STXP`.
