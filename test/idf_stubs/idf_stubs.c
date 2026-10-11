/* The stub definitions every test binary links. */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

char stub_log_last[256];
int64_t stub_time_us;

/*
 * ESP-IDF's default CONFIG_FREERTOS_HZ. Weak, so a test can define its own.
 */
__attribute__((weak)) unsigned int stub_tick_hz = 100;

void stub_log(char level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(stub_log_last, sizeof(stub_log_last), "%c %s: ", level,
		     tag);
	va_start(ap, fmt);
	vsnprintf(stub_log_last + n, sizeof(stub_log_last) - n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", stub_log_last);
}

/* Weak, so a test that wants time to move its own way defines it. */
__attribute__((weak)) int64_t esp_timer_get_time(void)
{
	return stub_time_us;
}

/*
 * Semaphores for a single task. A mutex taken twice can only be the code
 * locking itself out, which on the real, non-recursive mutex deadlocks
 * forever, so it asserts. A binary semaphore that would block returns
 * pdFALSE at once: there is no other task to give it. stub_sems_live
 * counts the ones created and not deleted.
 */
struct stub_sem {
	bool live, binary, held;
	int count;
};

static struct stub_sem stub_sems[8];
int stub_sems_live;

static SemaphoreHandle_t stub_sem_new(bool binary)
{
	for (size_t i = 0; i < sizeof(stub_sems) / sizeof(stub_sems[0]); i++) {
		if (!stub_sems[i].live) {
			stub_sems[i] = (struct stub_sem){ .live = true,
							  .binary = binary };
			stub_sems_live++;
			return (SemaphoreHandle_t)(void *)&stub_sems[i];
		}
	}
	abort();
}

__attribute__((weak)) SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
	return stub_sem_new(false);
}

__attribute__((weak)) SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
	return stub_sem_new(true);
}

__attribute__((weak)) BaseType_t xSemaphoreTake(SemaphoreHandle_t sem,
						TickType_t ticks)
{
	struct stub_sem *s = (struct stub_sem *)(void *)sem;

	(void)ticks;
	assert(s->live);
	if (s->binary) {
		if (!s->count)
			return pdFALSE;
		s->count = 0;
		return pdTRUE;
	}
	assert(!s->held);
	s->held = true;
	return pdTRUE;
}

__attribute__((weak)) BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
	struct stub_sem *s = (struct stub_sem *)(void *)sem;

	assert(s->live);
	if (s->binary) {
		s->count = 1;
		return pdTRUE;
	}
	assert(s->held);
	s->held = false;
	return pdTRUE;
}

__attribute__((weak)) BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem,
						       BaseType_t *woken)
{
	struct stub_sem *s = (struct stub_sem *)(void *)sem;

	assert(s->live && s->binary);
	s->count = 1;
	if (woken)
		*woken = pdTRUE;
	return pdTRUE;
}

__attribute__((weak)) void vSemaphoreDelete(SemaphoreHandle_t sem)
{
	struct stub_sem *s = (struct stub_sem *)(void *)sem;

	assert(s->live && !s->held);
	s->live = false;
	stub_sems_live--;
}

/*
 * A scheduler of one task: time passes in vTaskDelay(), and also by
 * stub_tick_step every time xTaskGetTickCount() looks, so that a deadline
 * loop still ends in a test whose own vTaskDelay() does nothing -- the
 * original drivers' tests define one. A test that wants exact time sets
 * stub_tick_step to 0. Weak, so a test that wants its own clock defines
 * them.
 */
TickType_t stub_ticks;
TickType_t stub_tick_step = 1;

__attribute__((weak)) void vTaskDelay(TickType_t ticks)
{
	stub_ticks += ticks;
}

__attribute__((weak)) TickType_t xTaskGetTickCount(void)
{
	TickType_t now = stub_ticks;

	stub_ticks += stub_tick_step;
	return now;
}

/* Weak, as the time it waits is a fake chip's business. */
__attribute__((weak)) void esp_rom_delay_us(uint32_t us)
{
	(void)us;
}

/*
 * src/uwb_port_esp.c is in every binary and calls the bus and GPIO ones.
 * A test with a fake chip defines the ones it needs, which win over these
 * weak ones; a test that reaches one it did not define has a bug, so stop it
 * there.
 */
__attribute__((weak)) esp_err_t
i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
			  const i2c_device_config_t *config,
			  i2c_master_dev_handle_t *out_handle)
{
	(void)bus;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
	(void)device;
	abort();
}

/* Something answers at every address, unless a test says otherwise. */
__attribute__((weak)) esp_err_t
i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
		 int xfer_timeout_ms)
{
	(void)bus;
	(void)address;
	(void)xfer_timeout_ms;
	return ESP_OK;
}

__attribute__((weak)) esp_err_t
i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data,
		    size_t length, int timeout_ms)
{
	(void)device;
	(void)data;
	(void)length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_transmit_receive(i2c_master_dev_handle_t device,
			    const uint8_t *tx, size_t tx_length, uint8_t *rx,
			    size_t rx_length, int timeout_ms)
{
	(void)device;
	(void)tx;
	(void)tx_length;
	(void)rx;
	(void)rx_length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_add_device(spi_host_device_t host,
		   const spi_device_interface_config_t *config,
		   spi_device_handle_t *out_handle)
{
	(void)host;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_remove_device(spi_device_handle_t device)
{
	(void)device;
	abort();
}

__attribute__((weak)) esp_err_t
spi_device_polling_transmit(spi_device_handle_t device,
			    spi_transaction_t *transfer)
{
	(void)device;
	(void)transfer;
	abort();
}

__attribute__((weak)) bool
uart_is_driver_installed(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) esp_err_t
uart_flush_input(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) int
uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
	(void)uart_num;
	(void)src;
	(void)size;
	abort();
}

__attribute__((weak)) esp_err_t
uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait)
{
	(void)uart_num;
	(void)ticks_to_wait;
	abort();
}

__attribute__((weak)) int
uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length,
		TickType_t ticks_to_wait)
{
	(void)uart_num;
	(void)buf;
	(void)length;
	(void)ticks_to_wait;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_config(const gpio_config_t *config)
{
	(void)config;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_set_level(gpio_num_t gpio, uint32_t level)
{
	(void)gpio;
	(void)level;
	abort();
}

__attribute__((weak)) int
gpio_get_level(gpio_num_t gpio)
{
	(void)gpio;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_set_direction(gpio_num_t gpio, gpio_mode_t mode)
{
	(void)gpio;
	(void)mode;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_install_isr_service(int intr_alloc_flags)
{
	(void)intr_alloc_flags;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t isr_handler, void *args)
{
	(void)gpio;
	(void)isr_handler;
	(void)args;
	abort();
}

__attribute__((weak)) esp_err_t
gpio_isr_handler_remove(gpio_num_t gpio)
{
	(void)gpio;
	abort();
}
