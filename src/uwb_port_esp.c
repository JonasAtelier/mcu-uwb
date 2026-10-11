/*
 * uwb_priv.h on ESP-IDF. The CMakeLists lists this file; stm-forge
 * compiles every .c in src/, so outside ESP-IDF it is empty.
 */
#ifdef ESP_PLATFORM

#include <string.h>

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uwb.h"
#include "uwb_priv.h"

/* 400 kHz when a caller says 0: fast mode. */
#define DEFAULT_SCL_HZ		400000

/*
 * Ticks for at least ms milliseconds, for the bus timeouts. Two things make
 * a plain pdMS_TO_TICKS(ms) wait less than asked:
 *
 *   - pdMS_TO_TICKS() truncates. At IDF's default 100 Hz tick, 2 ms is 0
 *     ticks and 15 ms is 1, so round up instead.
 *   - A wait of n ticks counts tick interrupts, and the first one may be
 *     about to fire, so n ticks can be as little as n - 1 full periods.
 *     Add one.
 *
 * So 20 ms at 100 Hz is 3 ticks, 20-30 ms: long, never short.
 */
static TickType_t uwb_ticks(uint32_t ms)
{
	return (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000) +
	       1;
}

int64_t uwb_time_us(void)
{
	return esp_timer_get_time();
}

/* ------------------------------------------------- reset pin, delay -- */

/* 0 counts as unwired too, so a zeroed config never pulses GPIO0. */
bool uwb_pin_wired(const struct uwb_pin *pin)
{
	return pin->gpio > 0;
}

void uwb_pin_drive_low(const struct uwb_pin *pin)
{
	gpio_set_direction(pin->gpio, GPIO_MODE_OUTPUT);
	gpio_set_level(pin->gpio, 0);
}

/* An input: RSTn floats, and the DW1000 releases itself. */
void uwb_pin_release(const struct uwb_pin *pin)
{
	gpio_set_direction(pin->gpio, GPIO_MODE_INPUT);
}

void uwb_delay_us(uint32_t us)
{
	esp_rom_delay_us(us);
}

void uwb_yield(void)
{
	vTaskDelay(1);
}

/* --------------------------------------------------------------- I2C -- */

int uwb_i2c_probe(uwb_i2c_bus handle, uint8_t address,
		       uint32_t timeout_ms)
{
	return uwb_errno(i2c_master_probe(handle, address,
					       (int)timeout_ms));
}

static int uwb_i2c_transfer(struct uwb_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint8_t out[1 + UWB_BUS_MAX_LEN];

	if ((tx_len && rx_len) || tx_len > UWB_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (rx_len)
		return uwb_errno(i2c_master_transmit_receive(
			bus->i2c, out, 1, rx, rx_len, (int)bus->timeout_ms));
	if (tx_len)
		memcpy(out + 1, tx, tx_len);
	return uwb_errno(i2c_master_transmit(bus->i2c, out, 1 + tx_len,
						  (int)bus->timeout_ms));
}

static void uwb_i2c_detach(struct uwb_bus *bus)
{
	i2c_master_bus_rm_device(bus->i2c);
}

static const struct uwb_bus_ops uwb_i2c_ops = {
	.kind = UWB_BUS_I2C,
	.transfer = uwb_i2c_transfer,
	.detach = uwb_i2c_detach,
};

int uwb_i2c_attach(struct uwb_bus *bus, uwb_i2c_bus handle,
			uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	i2c_device_config_t devcfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = scl_hz ? scl_hz : DEFAULT_SCL_HZ,
	};

	int ret;

	ret = uwb_errno(i2c_master_bus_add_device(handle, &devcfg,
						       &bus->i2c));
	if (ret)
		return ret;
	/* Set only now: a bus with ops is one that has something to detach. */
	bus->ops = &uwb_i2c_ops;
	bus->timeout_ms = timeout_ms;
	return 0;
}

/* --------------------------------------------------------------- SPI -- */

/*
 * One full-duplex transaction, so CS stays low across cmd, tx and rx: clock
 * out cmd and tx followed by zeros, and keep what came back after them. The
 * buffers live on the stack, word-aligned: IDF bounces non-DMA-capable or
 * unaligned buffers through a copy of its own.
 */
static int uwb_spi_transfer(struct uwb_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint8_t out[1 + UWB_BUS_MAX_LEN] __attribute__((aligned(4))) = {
		0
	};
	uint8_t in[1 + UWB_BUS_MAX_LEN] __attribute__((aligned(4)));
	spi_transaction_t t = { 0 };
	int ret;

	if (tx_len + rx_len > UWB_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	t.length = (1 + tx_len + rx_len) * 8;
	t.tx_buffer = out;
	t.rx_buffer = in;
	ret = uwb_errno(spi_device_polling_transmit(bus->spi, &t));
	if (!ret && rx_len)
		memcpy(rx, in + 1 + tx_len, rx_len);
	return ret;
}

/*
 * One frame, full duplex: rx[i] is what came back while tx[i] went out.
 * Polled, not queued: for a few bytes the interrupt and task switch of a
 * queued transfer cost more than the transfer itself. A frame of up to four
 * bytes travels in the transaction itself (tx_data, rx_data); a longer one,
 * through word-aligned buffers on the stack.
 */
static int uwb_spi_exchange(struct uwb_bus *bus, const uint8_t *tx,
				 uint8_t *rx, size_t len)
{
	uint8_t out[1 + UWB_BUS_MAX_LEN] __attribute__((aligned(4)));
	uint8_t in[1 + UWB_BUS_MAX_LEN] __attribute__((aligned(4)));
	spi_transaction_t t = { 0 };
	bool inline_data = len <= sizeof(t.tx_data);
	int ret;

	if (!len || len > sizeof(out))
		return -EINVAL;

	t.length = len * 8;
	if (inline_data) {
		t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
		memcpy(t.tx_data, tx, len);
	} else {
		memcpy(out, tx, len);
		t.tx_buffer = out;
		t.rx_buffer = in;
	}
	ret = uwb_errno(spi_device_polling_transmit(bus->spi, &t));
	if (!ret)
		memcpy(rx, inline_data ? t.rx_data : in, len);
	return ret;
}

static void uwb_spi_detach(struct uwb_bus *bus)
{
	spi_bus_remove_device(bus->spi);
}

static const struct uwb_bus_ops uwb_spi_ops = {
	.kind = UWB_BUS_SPI,
	.transfer = uwb_spi_transfer,
	.exchange = uwb_spi_exchange,
	.detach = uwb_spi_detach,
};

int uwb_spi_attach(struct uwb_bus *bus, spi_host_device_t host,
			int cs_pin, uint32_t clock_hz, uint8_t mode)
{
	spi_device_interface_config_t devcfg = {
		.mode = mode,
		.clock_speed_hz = (int)clock_hz,
		.spics_io_num = cs_pin,
		.queue_size = 1,
	};

	int ret;

	ret = uwb_errno(spi_bus_add_device(host, &devcfg, &bus->spi));
	if (ret)
		return ret;
	bus->ops = &uwb_spi_ops;
	bus->timeout_ms = 0;	/* spi_master transfers take none */
	return 0;
}

/* -------------------------------------------------------------- UART -- */

/*
 * Drop whatever is waiting -- a late reply to an earlier command would read
 * as the start of this one -- then send, then wait for the whole reply.
 * uart_read_bytes() returns early only with everything; a short count means
 * the timeout ran out.
 */
static int uwb_uart_transfer(struct uwb_bus *bus, uint8_t cmd,
				  const uint8_t *tx, size_t tx_len,
				  uint8_t *rx, size_t rx_len)
{
	uint8_t out[1 + UWB_BUS_MAX_LEN];
	int n, ret;

	if (tx_len > UWB_BUS_MAX_LEN || rx_len > INT32_MAX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	ret = uwb_errno(uart_flush_input(bus->uart));
	if (ret)
		return ret;
	n = uart_write_bytes(bus->uart, out, 1 + tx_len);
	if (n != (int)(1 + tx_len))
		return -EIO;
	if (!rx_len)
		return 0;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)rx_len,
			    uwb_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < rx_len ? -ETIMEDOUT : 0;
}

/*
 * The UART driver keeps everything the line delivers in its RX ring buffer,
 * so the echo of a whole datagram waits there however fast it came: flush,
 * send, wait for the last bit to leave, then read back as many bytes as
 * went out.
 */
static int uwb_uart_exchange(struct uwb_bus *bus, const uint8_t *tx,
				  uint8_t *rx, size_t len)
{
	int n, ret;

	if (!len || len > 1 + UWB_BUS_MAX_LEN)
		return -EINVAL;

	ret = uwb_errno(uart_flush_input(bus->uart));
	if (ret)
		return ret;
	n = uart_write_bytes(bus->uart, tx, len);
	if (n != (int)len)
		return -EIO;
	ret = uwb_errno(uart_wait_tx_done(bus->uart,
					       uwb_ticks(bus->timeout_ms)));
	if (ret)
		return ret;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)len,
			    uwb_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < len ? -ETIMEDOUT : 0;
}

static int uwb_uart_receive(struct uwb_bus *bus, uint8_t *rx,
				 size_t rx_len)
{
	int n;

	if (rx_len > INT32_MAX)
		return -EINVAL;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)rx_len,
			    uwb_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < rx_len ? -ETIMEDOUT : 0;
}

/* The driver is the app's: nothing was added, nothing to remove. */
static void uwb_uart_detach(struct uwb_bus *bus)
{
	(void)bus;
}

static const struct uwb_bus_ops uwb_uart_ops = {
	.kind = UWB_BUS_UART,
	.transfer = uwb_uart_transfer,
	.exchange = uwb_uart_exchange,
	.receive = uwb_uart_receive,
	.detach = uwb_uart_detach,
};

int uwb_uart_attach(struct uwb_bus *bus, uart_port_t port,
			 uint32_t timeout_ms)
{
	if (!uart_is_driver_installed(port))
		return -EINVAL;
	bus->ops = &uwb_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = port;
	return 0;
}

#endif /* ESP_PLATFORM */
