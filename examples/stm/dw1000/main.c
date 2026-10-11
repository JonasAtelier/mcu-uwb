/*
 * DW1000: two-way ranging over SPI, as examples/esp/dw1000 does. Flash two
 * boards: one with ROLE_INITIATOR 1 (the tag), one with 0 (the anchor).
 *
 * A snippet, not a project: the clock tree, the printf() retarget, the SPI
 * bus and the pins are the app's -- here hspi1 as CubeMX generates it
 * (MX_SPI1_Init()), mode 0 (CPOL low, CPHA 1 edge), 8-bit frames, software
 * NSS; CS a push-pull output idling high; RSTn an OPEN-DRAIN output idling
 * high (never push-pull: RSTn must never be driven high); IRQ an EXTI
 * rising-edge input. Needs HAL_SPI_MODULE_ENABLED and UWB_DW1000 = y;
 * printing the floats needs -u _printf_float.
 */
#include <errno.h>
#include <stdio.h>

#include CMSIS_device_header

#include "dw1000.h"
#include "dw1000_twr.h"
#include "uwb_log.h"

#define ROLE_INITIATOR 1

/* Ranging addresses. With more anchors, give each its own and range to each. */
#define TAG_ADDR	1
#define ANCHOR_ADDR	2

extern SPI_HandleTypeDef hspi1;		/* from the app's MX_SPI1_Init() */

static dw1000_handle_t dw;

/*
 * The DW1000 takes at most 3 MHz until its PLL locks, 20 MHz after (DS
 * Table 18). For SPI1 on an F405 at 168 MHz, APB2 is 84 MHz: /32 is
 * 2.6 MHz, /8 is 10.5 MHz. Pick the prescalers for your own bus clock.
 */
static int spi_clock(void *ctx, bool fast)
{
	SPI_HandleTypeDef *spi = ctx;

	spi->Init.BaudRatePrescaler = fast ? SPI_BAUDRATEPRESCALER_8 :
					     SPI_BAUDRATEPRESCALER_32;
	return HAL_SPI_Init(spi) == HAL_OK ? 0 : -EIO;
}

/* The IRQ pin's EXTI: only a flag; the waits stop polling SPI for it. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
	if (pin == GPIO_PIN_0 && dw)
		dw1000_irq(dw);
}

void ranging_task(void)
{
	const dw1000_config_t cfg = {
		.spi           = &hspi1,
		.cs_port       = GPIOA,
		.cs_pin        = GPIO_PIN_4,
		.rst_port      = GPIOB,		/* open-drain */
		.rst_pin       = GPIO_PIN_1,
		.use_irq       = true,		/* IRQ on PB0, EXTI0 */
		.spi_clock     = spi_clock,
		.spi_ctx       = &hspi1,
		.channel       = 5,
		.rate          = DW_RATE_6M8,
		.prf           = DW_PRF_64M,
		.preamble_len  = 128,
		.preamble_code = 9,
		.antenna_delay = 16436,		/* calibrate this -- see the README */
		.addr          = ROLE_INITIATOR ? TAG_ADDR : ANCHOR_ADDR,
	};

	/* -ENODEV: no DW1000 answers. -EIO: its PLL did not lock. */
	if (dw1000_init(&cfg, &dw))
		return;
	uwb_log_config(dw1000_uwb(dw));

	for (;;) {
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
	}
}
