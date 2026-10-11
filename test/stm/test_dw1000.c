/*
 * Host-side check of the dw1000 driver on the STM32 HAL: ../dw1000_cases.h
 * is the fake chip and the shared cases; this file feeds the fake from
 * HAL_SPI_Transmit() and HAL_SPI_TransmitReceive() under a software chip
 * select, and checks what only the HAL side has -- the handle checks, the
 * app's spi_clock() callback, the open-drain RSTn pin, and dw1000_irq().
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "uwb.h"
#include "dw1000.h"
#include "dw1000_twr.h"

#define RET(err)	(err)

static SPI_HandleTypeDef spi = {
	.Init = {
		.CLKPolarity = SPI_POLARITY_LOW,
		.CLKPhase = SPI_PHASE_1EDGE,
		.DataSize = SPI_DATASIZE_8BIT,
	},
};
static GPIO_TypeDef cs_port, rst_port;

/* The app's side of the speed switch: what the handle runs at now. */
static bool spi_fast;
static int clock_calls, clock_fail_at = -1;   /* fail that call with -EIO */
static int clock_xfers[4];              /* transactions before each call */
static bool clock_fast[4], clock_cplock[4];
static int ctx_tag;

static int spi_clock(void *ctx, bool fast);

static dw1000_config_t base_cfg(void)
{
	return (dw1000_config_t){
		.spi = &spi,
		.cs_port = &cs_port,
		.cs_pin = GPIO_PIN_4,
		.spi_clock = spi_clock,
		.spi_ctx = &ctx_tag,
		.channel = 5,
		.rate = DW_RATE_6M8,
		.prf = DW_PRF_64M,
		.preamble_len = 128,
		.preamble_code = 9,
		.addr = 2,
	};
}

/* The handle the app's EXTI callback would pass; NULL: no IRQ wired. */
static dw1000_handle_t irq_handle;
static int irqs;

static void raise_irq(void)
{
	irqs++;
	if (irq_handle)
		dw1000_irq(irq_handle);
}

#include "../dw1000_cases.h"

static int spi_clock(void *ctx, bool fast)
{
	assert(ctx == &ctx_tag);
	/* The first four since a test zeroed clock_calls. */
	if (clock_calls < 4) {
		clock_xfers[clock_calls] = xfers;
		clock_fast[clock_calls] = fast;
		clock_cplock[clock_calls] = cplock;
	}
	if (clock_calls++ == clock_fail_at)
		return -EIO;
	spi_fast = fast;
	return 0;
}

/* ------------------------------------------------- the HAL side of it --- */

/* Every read of the clock is 50 us later: SPI traffic takes time. */
static int64_t now_us;

int64_t uwb_time_us(void)
{
	return now_us += 50;
}

/* The port's SysTick delay has its own test; here, count what was asked. */
static uint64_t delayed_us;

void uwb_delay_us(uint32_t us)
{
	delayed_us += us;
	now_us += us;
}

static bool cs_low;
static int rst_lows, rst_releases;
static uint64_t rst_low_since, rst_low_for;

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	if (port == &cs_port) {
		assert(pin == GPIO_PIN_4);
		cs_low = state == GPIO_PIN_RESET;
		return;
	}
	assert(port == &rst_port && pin == GPIO_PIN_1);
	if (state == GPIO_PIN_RESET) {
		rst_lows++;
		rst_low_since = delayed_us;
	} else {
		/* Open-drain: SET lets the line float, the chip releases itself. */
		rst_releases++;
		rst_low_for = delayed_us - rst_low_since;
	}
}

static HAL_StatusTypeDef spi_xfer(SPI_HandleTypeDef *h, const uint8_t *tx,
				  uint8_t *rx, uint16_t n)
{
	assert(h == &spi && cs_low);
	/* Over 3 MHz before the PLL locks loses bits (DS Table 18). */
	assert(!spi_fast || cplock);
	if (spi_fails())
		return HAL_TIMEOUT;
	chip_xfer(tx, rx, n);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, const uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	uint8_t in[256];

	assert(size <= sizeof in && timeout > 0);
	return spi_xfer(h, data, in, size);
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
					  const uint8_t *tx, uint8_t *rx,
					  uint16_t size, uint32_t timeout)
{
	assert(timeout > 0);
	return spi_xfer(h, tx, rx, size);
}

/* ----------------------------------------------------------- STM cases --- */

/* Slow before the first transaction, fast once the PLL has locked. */
static void test_spi_clock(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;

	clock_calls = 0;
	h = open_cfg(&c);
	assert(clock_calls == 2);
	assert(!clock_fast[0] && clock_xfers[0] == 0);
	assert(clock_fast[1] && clock_cplock[1] && spi_fast);
	assert(!cs_low);
	dw1000_del(h);

	/* NULL: the app keeps the handle slow, and init still works. */
	spi_fast = false;
	c.spi_clock = NULL;
	h = open_cfg(&c);
	assert(!spi_fast);
	dw1000_del(h);

	/* The app's errno comes back, from either call; nothing leaks. */
	c = base_cfg();
	for (int k = 0; k < 2; k++) {
		chip_reset();
		allocs = frees = 0;
		clock_calls = 0;
		clock_fail_at = k;
		spi_fast = false;
		h = NULL;
		assert(dw1000_init(&c, &h) == -EIO && !h && allocs == frees);
		assert(clock_calls == k + 1);
		if (k == 0)
			assert(xfers == 0);     /* refused before the bus */
	}
	clock_fail_at = -1;
	spi_fast = false;
}

/* The app's handle must be mode 0 with bytes; anything else is refused
 * before the bus, or the callback, is touched. */
static void test_handle_checks(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h = NULL;

	chip_reset();
	clock_calls = 0;
	allocs = 0;
	spi.Init.CLKPolarity = SPI_POLARITY_HIGH;
	assert(dw1000_init(&c, &h) == -EINVAL);
	spi.Init.CLKPolarity = SPI_POLARITY_LOW;
	spi.Init.CLKPhase = SPI_PHASE_2EDGE;
	assert(dw1000_init(&c, &h) == -EINVAL);
	spi.Init.CLKPhase = SPI_PHASE_1EDGE;
	spi.Init.DataSize = SPI_DATASIZE_16BIT;
	assert(dw1000_init(&c, &h) == -EINVAL);
	spi.Init.DataSize = SPI_DATASIZE_8BIT;
	c.spi = NULL;
	assert(dw1000_init(&c, &h) == -EINVAL);
	c = base_cfg();
	c.cs_port = NULL;
	assert(dw1000_init(&c, &h) == -EINVAL);
	assert(!h && xfers == 0 && clock_calls == 0 && allocs == 0);
}

static void test_reset_pin(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;

	rst_lows = rst_releases = 0;
	h = open_ok();
	assert(rst_lows == 0);          /* NULL port: not wired */
	dw1000_del(h);

	c.rst_port = &rst_port;
	c.rst_pin = GPIO_PIN_1;
	h = open_cfg(&c);
	/* Low, then let go; held low long enough (DS 5.6.3.1). */
	assert(rst_lows == 1 && rst_releases == 1 && rst_low_for >= 1000);
	dw1000_del(h);
}

static void test_irq(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;
	uint8_t buf[8];
	size_t len;

	c.use_irq = true;
	h = open_cfg(&c);
	assert(get_le(DW_SYS_MASK, 0, 4) ==
	       (SYS_STATUS_TXFRS | SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_ERR |
		SYS_STATUS_ALL_RX_TO));

	irq_handle = h;
	irqs = 0;
	air_queue("irq", 3, 99, false);
	assert(dw1000_recv(h, buf, sizeof buf, &len, 50) == 0 && len == 3);
	assert(irqs == 1);
	/* No event: the wait runs to its deadline on the flag alone. */
	assert(dw1000_recv(h, buf, sizeof buf, &len, 20) == -ETIMEDOUT);
	irq_handle = NULL;
	dw1000_irq(NULL);               /* harmless */
	dw1000_del(h);

	/* Without use_irq, SYS_MASK is left alone. */
	h = open_ok();
	assert(get_le(DW_SYS_MASK, 0, 4) == 0);
	dw1000_del(h);
}

int main(void)
{
	run_dw1000_cases();
	test_spi_clock();
	test_handle_checks();
	test_reset_pin();
	test_irq();
	printf("all dw1000 checks passed (STM32 HAL)\n");
	return 0;
}
