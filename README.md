# mcu-uwb

Measure distance between two Decawave DW1000 radios, including DWM1000 modules, using two-way ranging. The SPI driver also sends and receives individual radio frames.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

## Quick start

Use two boards, each with a DW1000 module. Set the SPI pins and flash one with `ROLE_INITIATOR` set to 1, then set it to 0, rebuild and flash the other. Both need matching radio settings.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-uwb.git uwb
cd uwb/examples/esp/dw1000
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/dw1000/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

Both serial monitors print the distance in metres. Calibrate antenna delay against a measured separation for accurate ranges.

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Start from an example for your chip. The STM32 examples fit into a project with board setup already in place. [ESP-IDF examples](examples/esp/) · [STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

[MIT](LICENSE)

<p align="center">
  <img src="assets/module.jpg"
       alt="An ESP32 board carrying a DWM1000 ultra-wideband module"
       width="520">
  <br><sub>ESP32 UWB board carrying a DWM1000 &mdash; photo <a href="https://www.makerfabs.com/esp32-uwb-ultra-wideband.html">Makerfabs</a></sub>
</p>
