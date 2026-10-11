/*
 * Host-side check of the dw1000 driver on ESP-IDF: ../dw1000_cases.h is the
 * fake chip and the shared cases; this file feeds the fake from
 * spi_device_polling_transmit(), and checks what only ESP-IDF has -- the
 * SPI device re-added at full speed once the PLL locks, the RSTn pin, and
 * the IRQ pin's ISR and semaphore.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "uwb.h"
#include "uwb_priv.h"
#include "dw1000.h"
#include "dw1000_twr.h"

#define RET(err)	uwb_esp_err(err)

#define PIN_CS		5
#define PIN_RST		6
#define PIN_IRQ		7

extern int stub_sems_live;

static dw1000_config_t base_cfg(void)
{
	return (dw1000_config_t){
		.host = SPI2_HOST,
		.cs_gpio = PIN_CS,
		.rst_gpio = -1,
		.irq_gpio = -1,
		.channel = 5,
		.rate = DW_RATE_6M8,
		.prf = DW_PRF_64M,
		.preamble_len = 128,
		.preamble_code = 9,
		.addr = 2,
	};
}

static gpio_isr_t isr;
static void *isr_arg;

static void raise_irq(void)
{
	if (isr)
		isr(isr_arg);
}

#include "../dw1000_cases.h"

/* ------------------------------------------------- the IDF side of it --- */

/* Every read of the clock is 50 us later: SPI traffic takes time. */
int64_t esp_timer_get_time(void)
{
	return stub_time_us += 50;
}

static int devices, attaches;
static int attach_hz[4], attach_mode[4], attach_cs[4];
static int attach_xfers[4];             /* transactions before each attach */
static bool attach_cplock[4];
static int max_hz_seen;

static struct spi_device_t {
	int hz;
} device;

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *config,
			     spi_device_handle_t *out)
{
	assert(host == SPI2_HOST && devices == 0);
	/* The first four since a test zeroed attaches. */
	if (attaches < 4) {
		attach_hz[attaches] = config->clock_speed_hz;
		attach_mode[attaches] = config->mode;
		attach_cs[attaches] = config->spics_io_num;
		attach_xfers[attaches] = xfers;
		attach_cplock[attaches] = cplock;
	}
	attaches++;
	devices++;
	device.hz = config->clock_speed_hz;
	*out = &device;
	return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t dev)
{
	assert(dev == &device && devices == 1);
	devices--;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t dev,
				      spi_transaction_t *t)
{
	assert(dev == &device && devices == 1 && t->length % 8 == 0);
	assert(!(t->flags & (SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA)));
	/* Over 3 MHz before the PLL locks loses bits (DS Table 18). */
	assert(dev->hz <= 3000000 || cplock);
	if (dev->hz > max_hz_seen)
		max_hz_seen = dev->hz;
	if (spi_fails())
		return ESP_ERR_TIMEOUT;
	chip_xfer(t->tx_buffer, t->rx_buffer, t->length / 8);
	return ESP_OK;
}

/* RSTn: driven low, then let go as an input; never driven high. */
static int rst_lows, rst_releases;
static gpio_mode_t rst_mode = GPIO_MODE_INPUT;

esp_err_t gpio_set_direction(gpio_num_t gpio, gpio_mode_t mode)
{
	assert(gpio == PIN_RST);
	rst_mode = mode;
	if (mode == GPIO_MODE_INPUT)
		rst_releases++;
	return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
	assert(gpio == PIN_RST && rst_mode == GPIO_MODE_OUTPUT && level == 0);
	rst_lows++;
	return ESP_OK;
}

static gpio_config_t irq_io;
static esp_err_t isr_service_ret;
static int isr_removes;

esp_err_t gpio_config(const gpio_config_t *config)
{
	irq_io = *config;
	return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
	(void)flags;
	return isr_service_ret;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t handler, void *arg)
{
	assert(gpio == PIN_IRQ && !isr);
	isr = handler;
	isr_arg = arg;
	return ESP_OK;
}

/* As IDF's: harmless for a pin with no handler, which a failed install
 * leaves -- esp-dw1000-uwb removed it then too. */
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio)
{
	assert(gpio == PIN_IRQ);
	isr = NULL;
	isr_removes++;
	return ESP_OK;
}

/* ----------------------------------------------------------- ESP cases --- */

/* 2 MHz in mode 0 until the PLL locks, then the device again at spi_hz. */
static void test_spi_speed(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;

	attaches = 0;
	max_hz_seen = 0;
	h = open_cfg(&c);
	assert(attaches == 2);
	assert(attach_hz[0] == 2000000 && attach_mode[0] == 0 &&
	       attach_cs[0] == PIN_CS && attach_xfers[0] == 0);
	assert(attach_hz[1] == 8000000 && attach_mode[1] == 0 &&
	       attach_cplock[1]);
	dw1000_send(h, "x", 1);
	assert(max_hz_seen == 8000000);
	assert(dw1000_del(h) == ESP_OK && devices == 0);

	attaches = 0;
	c.spi_hz = 20000000;
	h = open_cfg(&c);
	assert(attach_hz[1] == 20000000);
	dw1000_del(h);

	/* Past the DW1000's 20 MHz: refused before anything is added. */
	attaches = 0;
	c.spi_hz = 20000001;
	assert(dw1000_init(&c, &h) == ESP_ERR_INVALID_ARG && attaches == 0);
}

static void test_reset_pin(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;

	rst_lows = rst_releases = 0;
	h = open_ok();
	assert(rst_lows == 0);          /* -1: not wired, never touched */
	dw1000_del(h);

	c.rst_gpio = 0;                 /* GPIO0 never serves as RSTn */
	h = open_cfg(&c);
	assert(rst_lows == 0);
	dw1000_del(h);

	c.rst_gpio = PIN_RST;
	h = open_cfg(&c);
	assert(rst_lows == 1 && rst_releases == 1 && rst_mode == GPIO_MODE_INPUT);
	dw1000_del(h);
}

static void test_irq(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h;
	uint8_t buf[8];
	size_t len;

	c.irq_gpio = PIN_IRQ;
	isr_service_ret = ESP_ERR_INVALID_STATE;    /* another driver's */
	h = open_cfg(&c);
	assert(isr && isr_arg == h);
	assert(irq_io.pin_bit_mask == 1ULL << PIN_IRQ &&
	       irq_io.mode == GPIO_MODE_INPUT &&
	       irq_io.intr_type == GPIO_INTR_POSEDGE);
	/* The events that end a wait, routed to the pin. */
	assert(get_le(DW_SYS_MASK, 0, 4) ==
	       (SYS_STATUS_TXFRS | SYS_STATUS_RXFCG | SYS_STATUS_ALL_RX_ERR |
		SYS_STATUS_ALL_RX_TO));
	/* A mutex and the ISR's semaphore. */
	assert(stub_sems_live == 2);

	/* The ISR gives the semaphore; the wait ends on it. */
	air_queue("irq", 3, 99, false);
	assert(dw1000_recv(h, buf, sizeof buf, &len, 50) == ESP_OK && len == 3);
	assert(dw1000_recv(h, buf, sizeof buf, &len, 20) == ESP_ERR_TIMEOUT);

	assert(dw1000_del(h) == ESP_OK);
	assert(isr_removes == 1 && !isr && stub_sems_live == 0);

	/* A failed install stops init; nothing is left behind. */
	isr_service_ret = ESP_FAIL;
	chip_reset();
	h = NULL;
	assert(dw1000_init(&c, &h) == ESP_FAIL && !h);
	assert(stub_sems_live == 0 && devices == 0);
	isr_service_ret = ESP_OK;
}

/* esp_err_t names for the errnos, as esp-dw1000-uwb's callers saw them. */
static void test_esp_errors(void)
{
	dw1000_config_t c = base_cfg();
	dw1000_handle_t h = NULL;

	chip_reset();
	absent = true;
	assert(dw1000_init(&c, &h) == ESP_ERR_NOT_FOUND);
	chip_reset();
	pll_never = true;
	assert(dw1000_init(&c, &h) == ESP_FAIL);
	h = open_ok();
	assert(dw1000_recv(h, (uint8_t[4]){ 0 }, 4, NULL, 5) == ESP_ERR_TIMEOUT);
	late_next_tx = true;
	peer_on_tx = responder;
	assert(dw1000_twr_range(h, 2, NULL) == ESP_ERR_INVALID_STATE);
	dw1000_del(h);
	assert(stub_sems_live == 0 && devices == 0);
}

int main(void)
{
	run_dw1000_cases();
	assert(stub_sems_live == 0 && devices == 0);
	test_spi_speed();
	test_reset_pin();
	test_irq();
	test_esp_errors();
	printf("all dw1000 checks passed (ESP-IDF)\n");
	return 0;
}
