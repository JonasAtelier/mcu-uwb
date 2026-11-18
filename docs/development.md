# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  uwb.h                     struct uwb_dev, uwb_send/recv(), timestamps, uwb_ticks_between()
  uwb_log.h                 uwb_err/warn/info/dbg, uwb_log_config()
  dw1000.h dw1000_twr.h     the chip's handle API and dw1000_uwb(); its ranging
src/                        private to the library
  uwb.c                     the core: bound checks, timestamps written only on success
  uwb_log.c                 uwb_log_config()
  uwb_math.h                wrap-safe tick arithmetic, the DS-TWR formula, ticks to metres
  uwb_priv.h                bus ops, attach and probe, reset pin, delay, yield, errno maps
  uwb_port_esp.c            all of that on ESP-IDF; empty elsewhere
  uwb_port_stm.c            all of that on the STM32 HAL; empty under ESP-IDF
  dw1000/                   dw1000.c  dw1000_twr.c  dw1000_regs.h  dw1000_time.h
                            dw1000_frame.h  dw1000_priv.h
test/                       host tests, `make -C test test`
  test_dw1000_time.c        the original's maths and frame-header test
  core_cases.h              core cases, shared by esp/ and stm/
  dw1000_cases.h            a DW1000 at SPI-transaction level and its cases, shared
  esp/                      suites on idf_stubs/, through uwb_port_esp.c
  stm/                      suites on hal_stubs/, through uwb_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/dw1000/               ESP-IDF two-board ranging project
  stm/dw1000/               STM32 snippet of the same
docs/                       the five guides; the DS and UM PDFs here are gitignored
assets/                     README photo
CMakeLists.txt              ESP-IDF component; elsewhere a static library for add_subdirectory(); stm-forge globs src/**/*.c instead
                            the static library's options are Kconfig's bool entries, found by the type line directly under each `config`
Kconfig                     UWB and UWB_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: init, del, the ops, the chip's calls |
| `<chip>_regs.h` | register map: addresses, bit fields, values, each with its manual citation |
| `<chip>_proto.h` | serial protocol, for parts without registers |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host |
| `<chip>_priv.h` | driver state, only if needed |

The DW1000 keeps the original's file names: its pure maths are
`dw1000_time.h`, aliases of `uwb_math.h` under the original's names, so the
original's test runs unchanged in substance, and the ranging frame header is
`dw1000_frame.h`. The chip driver keeps the original's four-space style; the
library's own files use tabs.

## Handles first, ranging per chip

As in mcu-stepper and mcu-loadcell: the chip's calls already had public
names taking a handle, and the ESP-IDF headers had to stay as they were, so
the handle stays the device. `struct dw1000_t` embeds a `struct uwb_dev`,
init fills it and points `priv` back at the handle, and `dw1000_uwb()` hands
it out.

What a UWB ranging radio shares is small: send, receive, and the
timestamps. The ranging exchange stays with the chip: it rests on how the
chip schedules a delayed send and turns its receiver on after one, and with
one chip there is no second shape to generalise from. The arithmetic it
uses is generic and lives in `src/uwb_math.h`.

## Platform ports

The driver in `src/` is the same C on both platforms; what differs is in
`src/uwb_port_esp.c` and `src/uwb_port_stm.c`:

| Helper | ESP-IDF | STM32 HAL |
|---|---|---|
| bus ops, attach, `uwb_i2c_probe()` | `i2c_master`, `spi_master`, UART driver | HAL I2C, SPI, UART |
| `uwb_pin_wired/drive_low/release()` (RSTn) | `gpio > 0`; output low; back to input | port not NULL; `HAL_GPIO_WritePin()` RESET; SET, which floats an open-drain pin |
| `uwb_delay_us()` | `esp_rom_delay_us()` | weak, on SysTick, never early |
| `uwb_yield()` | `vTaskDelay(1)` | `HAL_Delay(1)` |
| `uwb_time_us()` | `esp_timer_get_time()` | weak, `HAL_GetTick()` × 1000 |

What only one platform has stays in the driver behind `#ifdef
ESP_PLATFORM`: on ESP-IDF the per-handle mutex, the IRQ pin's ISR and
semaphore, and re-adding the SPI device at full speed; on the HAL the
handle checks, the `spi_clock` callback and `dw1000_irq()`'s flag.

The handle calls return `esp_err_t` on ESP-IDF and errno on the HAL: the
driver works in errno and `UWB_HANDLE_RET()` converts once where each public
call returns (`uwb_esp_err()`).

Each chip file includes `uwb_priv.h` before its `#if CONFIG_UWB_<CHIP>`: on
ESP-IDF that is what brings in `sdkconfig.h`, and a chip file that tested
the option first would compile to nothing.

## Bus abstraction

The bus layer is mcu-stepper's: `struct uwb_bus_ops` with `transfer()`,
`exchange()` (SPI frames, UART echoes), `receive()` (UART) and `detach()`,
picked at run time by which attach the driver calls. Every category library
offers I2C, SPI and UART, used or not; the DW1000 is on SPI, and
`test/{esp,stm}/test_port_*` check all three. A DW1000 transaction is one
`transfer()`: header byte 0 as `cmd`, the rest of the header and a write's
payload as `tx`, a read's payload as `rx`, all under one chip select.

## Adding a chip

1. `include/foo.h`: the chip's handle API, with the platform's handle types
   behind `#ifdef ESP_PLATFORM`, and `struct uwb_dev *foo_uwb(foo_handle_t)`.
2. `src/foo/foo.c`, in a folder of its own. Include `uwb_priv.h` first, then
   wrap the driver in `#if CONFIG_UWB_FOO` (never `#ifdef`). Embed a
   `struct uwb_dev` in the handle; give it all five ops.
3. In init: set `uwb.name` first, refuse a bad config, allocate, attach,
   check identity, configure, and only on success set `uwb.ops`,
   `uwb.priv` and the configuration fields. Check every bus transfer. Work
   in errno; return `UWB_HANDLE_RET(err)`.
4. Add `CONFIG_UWB_FOO` to the no-chip checks in `CMakeLists.txt` (its
   ESP-IDF part; the `add_subdirectory()` part reads `Kconfig`) and
   `src/uwb_port_stm.c`, a HAL module `#error` to `src/uwb_priv.h` if it
   needs one, its sources to `CMakeLists.txt`, a `config UWB_FOO` to
   `Kconfig`, `# UWB_FOO is not set` to both forges' defconfigs, and its
   tests to `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. Nothing is checked against
real silicon.

- **The original's own test** runs with plumbing changes only:
  `test/test_dw1000_time.c` is its `test_time.c` with the include paths
  changed (`../src/dw1000/`) and the two `SYS_CFG` expectations carrying
  `DIS_STXP` (0x00041200, 0x00441200). The original had no driver test.
- **This library's own cases** are written once, in `test/*_cases.h`,
  against fakes that know nothing of the driver, and both platforms run them
  from their own calls. The fake DW1000 decodes each transaction's header and
  keeps the register files; it models write-1-to-clear `SYS_STATUS`, CPLOCK
  only after the LDE load, OTP reads, immediate and delayed sends with their
  timestamps, WAIT4RESP and RXENAB, and a scripted peer for each side of a
  ranging exchange.
- **The SPI-error regression:** `test_init_failures` fails every transaction
  of a good init in turn, both from there on and that one alone, and
  expects init to return the failure and free everything. With the
  original's `dw_read32()` or one unchecked configuration write pasted
  back in, it fails; so do the `SYS_CFG` bit 18 check with `DIS_STXP`
  dropped and the channel 3 check with `FS_PLLTUNE` 0x56.

| Binary | Checks |
|---|---|
| `test_dw1000_time` | the original's: 40-bit wrap, asymmetric reply delays, the 512-tick delayed-send rounding, a 10 m link end to end, register packing, the frame header's drop rule |
| `esp/test_core`, `stm/test_core` | `uwb_send/recv` passing through; timestamps written only on success; the wrap-safe tick difference at 40 and 32 bits; unbound devices and ops tables with a hole refused without a call; log lines; on the HAL the default clock and `uwb_dbg()` |
| `esp/test_dw1000`, `stm/test_dw1000` | init: soft reset, LDE load, PLLLDT, LDO kick and crystal trim from OTP; **`SYS_CFG` with bit 18 set**; channel 5 / PRF 64 tuning words, `CHAN_CTRL`, antenna delay; the category view; **channel 3's `FS_PLLTUNE` 0x5E**; an absent chip, a wrong `DEV_ID`, a PLL that never locks, bad configs refused before allocating; **every init transaction failed in turn, its errno back and nothing leaked**; send and receive, the FCS dropped, a cut frame, a timeout and a frame in error each resetting the receiver; failed timestamp reads; ranging both ways against a scripted peer, a stale RESP, a missed FINAL slot (`-ETIME`, nothing sent, the warning cleared), no answer; NULLs. ESP-IDF: 2 MHz mode 0 until the PLL locks, then `spi_hz`, never over 3 MHz unlocked, 20 MHz refused; RSTn low then input, unwired at 0 and -1; the IRQ pin's config, `SYS_MASK`, the ISR waking a wait, the shared ISR service tolerated, a failed install leaking nothing; the `esp_err_t` names. HAL: `spi_clock(false)` before the first transaction and `(true)` only after lock, NULL working slow, either call's failure returned with nothing leaked; mode, phase, frame size and NULLs refused before the bus; RSTn low ≥ 1 ms then let go; `use_irq` routing `SYS_MASK` and `dw1000_irq()` ending a wait |
| `esp/test_port_esp`, `stm/test_port_stm` | the bus ops as in mcu-stepper (I2C, SPI, UART with exchange and receive, the HAL's no-overrun echo) and the presence probe; RSTn never driven high; ESP-IDF: `esp_rom_delay_us()`, a one-tick yield, `esp_timer`; HAL: the SysTick delay never early for 1–64 µs, from every phase across a reload, with coarse reads, on the core clock / 8 -- exactly at the boundary from every phase within a count -- on a slower core, and the busy loop with SysTick stopped; `HAL_Delay(1)` as the yield; the default clock |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic` with UBSan
(`-fsanitize=undefined -fno-sanitize-recover=all`), and `-Wl,--wrap` (GNU
ld) to count allocations and capture `printf()`.
