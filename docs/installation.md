# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-uwb`; the forge package key, C API and configuration
options are `uwb`. The clone command below names the ESP-IDF component
directory `uwb`. It replaces `esp-dw1000-uwb` (forge key `dw1000`): a
project that `REQUIRES dw1000` now `REQUIRES uwb`.

## With forge

stm-forge must be at `ee921c3` or later ("build: libraries may keep sources
in src/ subfolders"); older ones only compile `src/*.c`, so the chip driver
in `src/dw1000/` is never built and the link fails with undefined
references to the chip functions. esp-forge is unaffected: the component's
CMakeLists lists its sources.

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
uwb_GIT=https://github.com/JonasAtelier/mcu-uwb@stable
```

and a target turns the library and its chip on in `configs/<target>_defconfig`:

```make
PKG_uwb             = y
UWB_DW1000          = y
# UWB_DEBUG is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_UWB_<CHIP>=y` in
`sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_UWB_<CHIP>=1`.
`UWB_DEBUG = y` turns on `uwb_dbg()` lines on the HAL; on ESP-IDF the log
level decides.

## ESP-IDF

Clone into your project's `components/` directory. A local component is
named after its directory, and that name is what you `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-uwb components/uwb
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES uwb driver)
```

Enable the chip under `idf.py menuconfig` → **Component config → UWB
transceiver subsystem**, or put `CONFIG_UWB_DW1000=y` in your
`sdkconfig.defaults` as `examples/esp/dw1000/` does. With the subsystem on
and no chip picked, the build warns. ESP-IDF 5.2 or newer is required: the
bus layer every category library shares is built on the `i2c_master` API.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, chip folders included, add `include/` and `src/` to the include path
(`src/` only needs to reach this library's own sources), and define what the
forge would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"`,
`"stm32f7xx.h"`, `"stm32g0xx.h"`), `USE_HAL_DRIVER`, and
`CONFIG_UWB_DW1000=1`.

**No chip driver is built by default.** The core is always built and each
driver is opt-in: a chip whose option is absent or 0 compiles to nothing.
With the library on and no chip picked, the ESP-IDF CMakeLists warns and
`src/uwb_port_stm.c` emits a `#warning`.

**The HAL modules are yours to enable:**

| Option | Needs in hal_conf | HAL source |
|---|---|---|
| `UWB_DW1000` | `HAL_SPI_MODULE_ENABLED` | `stm32<family>xx_hal_spi.c` |

Without it the build stops with an `#error` in `src/uwb_priv.h` that says
which define is missing. The bus layer's I2C and UART code sit under their
own module switches and build to nothing without them.

**Microseconds come from SysTick.** The DW1000 needs a few short waits --
the reset pulse, the LDE microcode load, the PLL lock poll -- and the HAL
has no microsecond delay. `HAL_Init()` starts SysTick for the HAL's 1 ms
tick, and the library's weak `uwb_delay_us()` counts it, never returning
early. If your app runs SysTick off, or drives the HAL timebase from a TIM,
define your own `void uwb_delay_us(uint32_t us)` (DWT->CYCCNT on an F4 or
F7, or a free-running timer).

**The clock is milliseconds.** The library's waits run on `uwb_time_us()`,
weak, `HAL_GetTick()` × 1000 by default. Define your own for real
microseconds; the waits only need it never to run backwards.

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART. stm-forge links newlib-nano
(`--specs=nano.specs`), whose `printf()` drops `%f` unless the app links with
`-u _printf_float`: add `target_link_options(firmware PRIVATE -u _printf_float)`
to your app's `CMakeLists.txt`, or the distance prints blank.
