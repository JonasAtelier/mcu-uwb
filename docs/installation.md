# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-uwb`; the forge package key, C API and configuration
options are `uwb`. The clone command below names the ESP-IDF component
directory `uwb`. It replaces `esp-dw1000-uwb` (forge key `dw1000`): a
project that `REQUIRES dw1000` now `REQUIRES uwb`.

Three step-by-step guides come first: [ESP-IDF](#esp-idf-step-by-step),
[STM32 with CMake](#with-cmake-a-cubemx-cmake-project) and
[STM32 without CMake](#without-cmake-stm32cubeide-a-makefile). None
needs a forge; if you build with esp-forge or stm-forge, see
[With forge](#with-forge). The steps use the DW1000, the one chip so far.
Ranging takes two boards: flash one as the initiator (the tag) and one as
the responder (the anchor). The reference follows the guides.

**Arduino is not supported yet.** TODO: make the library usable from
Arduino (Arduino IDE and PlatformIO).

## ESP-IDF, step by step

**You need** ESP-IDF 5.2 or newer, with `idf.py` working in your terminal, and
an ESP-IDF project: a folder with a `CMakeLists.txt` and a `main/` folder.
`idf.py create-project my-project` makes one.

ESP-IDF calls a library a **component**: a folder with its own
`CMakeLists.txt` that the build finds and compiles. This library is the
component `uwb`.

1. **Get the library.** Pick one way.

   - **Component manager**, the simplest: the first build downloads the
     library into `managed_components/uwb`. Create `main/idf_component.yml`:

     ```yaml
     dependencies:
       uwb:
         git: https://github.com/JonasAtelier/mcu-uwb.git
         version: stable
     ```

   - **Clone**, if you want to edit the library or build offline. In the
     project folder:

     ```sh
     git clone https://github.com/JonasAtelier/mcu-uwb components/uwb
     ```

     Keep the folder name `uwb`: ESP-IDF names a component after its folder.

   ```
   my-project/
   ├── CMakeLists.txt
   ├── sdkconfig.defaults       step 2
   ├── main/
   │   ├── CMakeLists.txt       step 3
   │   ├── idf_component.yml    step 1, component manager
   │   └── my-project.c         step 4
   └── components/
       └── uwb/                 step 1, clone
   ```

2. **Turn your chip on.** No chip driver is built until you do. Create
   `sdkconfig.defaults` in the project folder, a text file of settings you
   choose, with this line:

   ```ini
   CONFIG_UWB_DW1000=y
   ```

   ESP-IDF copies these lines into `sdkconfig`, the project's full settings
   file, only when it creates that file: **if `sdkconfig` already exists,
   delete it** so your line is read. `idf.py menuconfig` → **Component
   config → UWB transceiver subsystem** does the same from a menu.

3. **Let your code see the library.** A new project's `main/CMakeLists.txt`
   has no `REQUIRES` and already sees every component. If yours has a
   `REQUIRES` list, add `uwb` and `driver` to it:

   ```cmake
   idf_component_register(SRCS "main.c" REQUIRES uwb driver)
   ```

4. **Write the smallest test.** Copy
   [examples/esp/dw1000/main/main.c](../examples/esp/dw1000/main/main.c)
   over your main source file and set `PIN_MISO`, `PIN_MOSI`, `PIN_SCLK`,
   `PIN_CS`, `PIN_RST` and `PIN_IRQ` to your wiring. Build it once with
   `ROLE_INITIATOR` 1 for the tag and once with 0 for the anchor.
   [API usage](usage.md) explains the calls.

5. **Build.**

   ```sh
   idf.py set-target esp32    # your chip: esp32s3, esp32c3, ...
   idf.py build
   ```

   A good build ends with `Project build complete.` Flash it as any ESP-IDF
   app and open the serial monitor: on the tag, the driver's first line ends
   in `uwb: dw1000: channel 5, PRF 64 MHz, 6800 kbps, preamble 128, addr 0x0001`,
   and with the anchor running, `distance` lines follow.

**If it fails**

| The build prints | Why | Fix |
|---|---|---|
| `Failed to resolve component 'uwb' required by component 'main': unknown name.` | The clone is not in a folder named `uwb` | Rename it to `components/uwb` |
| `fatal error: dw1000.h: No such file or directory` | `main/CMakeLists.txt` has a `REQUIRES` list without `uwb` | Step 3 |
| `uwb: CONFIG_UWB is on but no chip driver is selected` (a CMake warning) | No chip turned on | Step 2 |
| ``undefined reference to `dw1000_init'`` | That chip is not turned on, often because `sdkconfig` existed before you edited `sdkconfig.defaults` | Delete `sdkconfig`, build again |

## STM32, step by step

**You need** a project for an STM32F4, F7 or G0 that uses ST's **HAL**, the
vendor library behind `HAL_Init()`, `HAL_SPI_Init()` and friends, with the
SPI your chip is wired to already set up. STM32CubeMX makes such a project;
with **Toolchain / IDE: CMake** it makes a CMake one, the main case below.

Two terms. A **HAL module** is the part of the HAL for one peripheral: I2C,
SPI, UART. It is compiled only when it is switched on in **hal_conf**, the
file `Core/Inc/stm32f4xx_hal_conf.h` (`stm32f7xx_hal_conf.h` on an F7,
`stm32g0xx_hal_conf.h` on a G0). CubeMX switches a module on when you enable
that peripheral.

### With CMake (a CubeMX CMake project)

1. **Get the library.** In the project folder, next to the top-level
   `CMakeLists.txt`:

   ```sh
   git clone https://github.com/JonasAtelier/mcu-uwb
   ```

   ```
   my-board/
   ├── CMakeLists.txt                  step 2
   ├── cmake/stm32cubemx/              made by CubeMX
   ├── Core/
   │   ├── Inc/stm32f4xx_hal_conf.h    hal_conf, step 3
   │   └── Src/main.c                  step 4
   ├── Drivers/                        the HAL, copied in by CubeMX
   └── mcu-uwb/                        step 1
   ```

2. **Add it to the build.** In the top `CMakeLists.txt`, after the line
   `add_subdirectory(cmake/stm32cubemx)`, add:

   ```cmake
   set(CONFIG_UWB_DW1000 ON)           # the chip
   set(MCU_HAL_TARGET stm32cubemx)
   set(MCU_DEVICE_HEADER stm32f4xx.h)  # stm32f7xx.h on an F7, stm32g0xx.h on a G0
   add_subdirectory(mcu-uwb)
   ```

   and add `uwb` to the link line CubeMX already wrote, so it reads:

   ```cmake
   target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx uwb)
   ```

   Do not add `PRIVATE`: CubeMX's line has no such keyword, and CMake refuses
   to mix the two forms on one target.

   `stm32cubemx` is a CMake *target*, a named bundle of settings, that CubeMX
   makes to carry the HAL's include folders and defines; `MCU_HAL_TARGET`
   lends those to the library, which works with any CubeMX version.
   `MCU_DEVICE_HEADER` is your chip family's main header.

3. **Check the HAL module.** The DW1000 talks SPI, so hal_conf must have
   `#define HAL_SPI_MODULE_ENABLED`; enabling SPI1 (or any SPI) in CubeMX
   does that. Set it up as a full-duplex master, mode 0 (CPOL low, CPHA
   1 edge), 8-bit frames, software NSS, with a prescaler that keeps the
   clock at 3 MHz or less. Two more pins, as GPIO outputs: PA4, the chip
   select, push-pull and high at start; PB1, RSTn, **open drain** and high
   at start (RSTn must never be driven high).

4. **Write the smallest test.** It is
   [examples/stm/dw1000/main.c](../examples/stm/dw1000/main.c) fitted into
   CubeMX's `Core/Src/main.c`, with the IRQ pin and the SPI clock switch
   left out: the driver polls the chip, and the SPI stays at its slow
   clock, which is always correct. Paste each part between the
   `USER CODE BEGIN` and `END` comments named, so a regenerate keeps it.

   After `/* USER CODE BEGIN Includes */`:

   ```c
   #include <errno.h>
   #include <stdio.h>

   #include "dw1000.h"
   #include "dw1000_twr.h"
   #include "uwb_log.h"

   /* 1 on the tag, 0 on the anchor. */
   #define ROLE_INITIATOR 1
   #define TAG_ADDR       1
   #define ANCHOR_ADDR    2
   ```

   After `/* USER CODE BEGIN 2 */`, which comes after `MX_SPI1_Init()`:

   ```c
     /* As in examples/stm/dw1000/main.c: CS on PA4, RSTn on PB1. */
     const dw1000_config_t cfg = {
       .spi           = &hspi1,
       .cs_port       = GPIOA,
       .cs_pin        = GPIO_PIN_4,
       .rst_port      = GPIOB,   /* open drain */
       .rst_pin       = GPIO_PIN_1,
       .channel       = 5,
       .rate          = DW_RATE_6M8,
       .prf           = DW_PRF_64M,
       .preamble_len  = 128,
       .preamble_code = 9,
       .antenna_delay = 16436,   /* calibrate this for your board */
       .addr          = ROLE_INITIATOR ? TAG_ADDR : ANCHOR_ADDR,
     };
     dw1000_handle_t dw;

     /* -ENODEV: no DW1000 answers. -EIO: its PLL did not lock. */
     if (dw1000_init(&cfg, &dw))
       Error_Handler();
     uwb_log_config(dw1000_uwb(dw));
   ```

   After `/* USER CODE BEGIN 3 */`, inside the `while (1)` loop:

   ```c
       float d;
       int err;

   #if ROLE_INITIATOR
       err = dw1000_twr_range(dw, ANCHOR_ADDR, &d);
       HAL_Delay(100);
   #else
       err = dw1000_twr_respond(dw, &d, 1000);
   #endif
       if (!err)
         printf("distance %.3f m\n", d);
       else if (err != -ETIMEDOUT)
         printf("range failed: %d\n", err);
   ```

   The lines go out through `printf()`: send that to a UART by writing
   newlib's `_write()`, and add this to `CMakeLists.txt` so numbers are
   printed:

   ```cmake
   target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
   ```

   [API usage](usage.md) explains the calls.

5. **Build** as before, for example `cmake --build` with your build folder.
   A good build shows a `Linking C executable` line and no error. On the
   UART of the tag, the driver's first line reads
   `uwb: dw1000: channel 5, PRF 64 MHz, 6800 kbps, preamble 128, addr 0x0001`;
   with the anchor running, `distance` lines follow.

**If it fails**

| You see | Why | Fix |
|---|---|---|
| `uwb: no chip driver is selected; set(CONFIG_UWB_<CHIP> ON) before add_subdirectory()` (a CMake warning), then ``undefined reference to `dw1000_init'`` | No chip turned on, or turned on after `add_subdirectory(mcu-uwb)` | Step 2 |
| `#error "the DW1000 driver needs the HAL SPI module: ...` | The HAL module is off | Enable the peripheral in CubeMX and regenerate |
| `#error "uwb: CMSIS_device_header is not defined; ...` | `MCU_DEVICE_HEADER` is missing | Step 2 |
| `fatal error: stm32f4xx.h: No such file or directory` | `MCU_HAL_TARGET` is missing | Step 2 |
| `Target "stm32cubmx" not found.` (a CMake error) | `MCU_HAL_TARGET` is misspelt | Step 2 |
| `The plain signature for target_link_libraries has already been used with the target` | `PRIVATE` added to the link line | Step 2 |
| No error, but the distance prints as a blank | The C library (newlib-nano) leaves out `%f` | The `-u _printf_float` line of step 4 |

### Without CMake (STM32CubeIDE, a Makefile)

1. Copy `include/` and `src/` of this repository into your project, for
   example to `Middlewares/mcu-uwb/`, keeping the chip folder inside `src/`.
2. Add every `.c` file under `src/` to the build, chip folder included.
3. Add two include paths, `Middlewares/mcu-uwb/include` and
   `Middlewares/mcu-uwb/src`, and two defines:
   `CMSIS_device_header="stm32f4xx.h"`, quotes included, and
   `CONFIG_UWB_DW1000=1`. CubeMX already defines `USE_HAL_DRIVER` and your
   device, such as `STM32F405xx`.
4. Steps 3 to 5 of the CMake guide apply as they are; `-u _printf_float`
   goes in the linker flags.

Where these settings usually live: in STM32CubeIDE, **Project → Properties →
C/C++ Build → Settings → Tool Settings → MCU GCC Compiler** (Include paths,
Preprocessor); in a CubeMX Makefile, `C_SOURCES`, `C_INCLUDES` and `C_DEFS`.
The build errors are the ones in the table above; the fix is the matching
define or include path instead of a CMake line. One more:
`#include expects "FILENAME" or <FILENAME>` means `CMSIS_device_header`
lost its quotes.

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

Setup: [ESP-IDF, step by step](#esp-idf-step-by-step). For reference:

- In `main/idf_component.yml`, `version` is a git ref; `stable` is the
  released one.
- A clone in `components/` is a local component, named after its folder;
  that name is what other components `REQUIRES`. The examples in
  `examples/esp/` need no name: their `main` lists no `REQUIRES`, and ESP-IDF
  gives `main` every component, so they build from any checkout name.
- With the subsystem on and no chip picked, the build warns.
- ESP-IDF 5.2 or newer is required: the bus layer every category library
  shares is built on the `i2c_master` API.

## STM32 HAL

Setup: [With CMake](#with-cmake-a-cubemx-cmake-project) or
[Without CMake](#without-cmake-stm32cubeide-a-makefile). The
`add_subdirectory()` route builds a static library `uwb`. For reference:

- Each `CONFIG_UWB_<CHIP>` (and `CONFIG_UWB_DEBUG`) that is true reaches the
  compiler as `CONFIG_UWB_<CHIP>=1`, for the library and your app.
- `MCU_HAL_TARGET` is a target that carries your `USE_HAL_DRIVER`, device
  define (`STM32F405xx`, ...) and the include dirs of the HAL, CMSIS and your
  `stm32<family>xx_hal_conf.h`. The library copies only those include dirs
  and defines, never the target's sources, so this works with any CubeMX
  version. `MCU_DEVICE_HEADER` becomes `CMSIS_device_header`, which CubeMX
  does not define. Every `mcu-*` library reads these two, so set them once.
  Leave them unset if your project already provides both.
- The CPU flags (`-mcpu`, `-mfloat-abi`, ...) must reach the library too;
  CubeMX's toolchain file sets them for every target.
- Without CMake, `src/` only needs to be on the include path of this
  library's own sources; the defines are what stm-forge passes.

**No chip driver is built by default.** The core is always built and each
driver is opt-in: a chip whose option is absent or 0 compiles to nothing.
With the library on and no chip picked, the CMakeLists warns (ESP-IDF and
`add_subdirectory()`) and `src/uwb_port_stm.c` emits a `#warning`.

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
to your app's `CMakeLists.txt` (in a CubeMX project the target is
`${CMAKE_PROJECT_NAME}`), or the distance prints blank.
