# Hardware wiring

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Pins

### DW1000 — `examples/*/dw1000`

Names are the data sheet's, `docs/DW1000_Data_Sheet.pdf` section 3. A
DWM1000 module carries the same names on its castellations, minus the
supply rails it regulates itself.

| Pin | ESP-IDF (example) | STM32 (example) | Note |
|---|---|---|---|
| VDD | 3.3 V | 3.3 V | one rail on the module; the bare chip has nine, all fed from it |
| GND | GND | GND | shared ground, always |
| SPICLK | GPIO 18 | SPI1 SCK | any pins the bus can use |
| SPIMOSI | GPIO 23 | SPI1 MOSI | |
| SPIMISO | GPIO 19 | SPI1 MISO | |
| SPICSn | GPIO 4 | PA4 | one per chip on the bus; a falling edge also wakes the chip. On the HAL a push-pull output idling high, driven by the library |
| RSTn | GPIO 27 | PB1 | the library drives it low for 1 ms, then lets it go. **Never drive it high** (DS 5.6.3.1, p.23). ESP-IDF: the driver switches the pin to output low, then back to input; `0` or `-1` if unwired, so GPIO0 cannot be used. HAL: an **open-drain** output the app set up, idling high; `NULL` port if unwired |
| IRQ | GPIO 34 | PB0, EXTI0 | optional, active high. ESP-IDF: set `irq_gpio` and waits sleep on its rising edge; `0` or `-1` polls. HAL: a rising-edge EXTI whose callback calls `dw1000_irq()`, with `use_irq` set |
| WAKEUP | GND | GND | the driver never sleeps the chip |
| EXTON | nothing | nothing | wakes an external regulator; leave open |
| GPIO5/SPIPOL, GPIO6/SPIPHA | nothing | nothing | **latched at power-up to pick the SPI mode.** Both have pull-downs, so open gives mode 0, which this library uses (DS Table 17, p.25) |
| GPIO0–GPIO3 | nothing | nothing | LED drivers; not used |
| FORCEON, TESTMODE, VREF | GND, GND, 11 kΩ to GND | same | bare chip only; a DWM1000 has these done already |

### SPI speed

The DW1000 takes SPI at up to 3 MHz until its PLL locks, 20 MHz after (DS
Table 18, p.26).

- **ESP-IDF:** the driver adds its device at 2 MHz for the reset, identity
  check and start-up, then removes it and adds it again at `spi_hz` (0:
  8 MHz; above 20 MHz init refuses).
- **HAL:** the handle and its prescaler are the app's. Give the config a
  `spi_clock(ctx, fast)` callback: the driver calls it with `false` before
  it touches the chip and with `true` once the PLL is up, and the app sets
  the prescaler (`HAL_SPI_Init()` again, or `CR1.BR`). **Without a callback
  the handle must stay at 3 MHz or less all the time** -- always correct,
  only slower.

On the HAL the handle must be in mode 0 (CPOL low, CPHA first edge) with
8-bit frames; init checks and refuses anything else with `-EINVAL` before
touching the bus.
