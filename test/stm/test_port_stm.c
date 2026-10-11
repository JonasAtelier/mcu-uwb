/*
 * Host-side check of src/uwb_port_stm.c against test/hal_stubs: the bus
 * ops -- SPI, which the DW1000 uses; I2C, presence probe included, and UART,
 * which no UWB chip uses yet -- and the RSTn pin, the SysTick microsecond
 * delay, the yield and the clock.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "uwb.h"
#include "uwb_priv.h"

static UART_HandleTypeDef uart_handle;
/*
 * In order. UART: O overrun clear, F flush, W send, R receive. SPI: L and H
 * chip select low and high, T transmit, X transmit-receive. RSTn: E low,
 * D let go.
 */
static char calls[64];
static HAL_StatusTypeDef next_status;
static int timeout_after_send;	/* the receive after a send times out */
static int stale_bytes;		/* bytes waiting in the data register */
static int ore_cleared;
static uint8_t sent[40];
static size_t sent_len;
static uint8_t reply[32];
static uint16_t last_size;

/*
 * A single wire, down to the data register: every byte sent comes straight
 * back into the one-byte RDR. A byte that arrives while RDR is still full
 * is lost and sets ORE, as on the F4/F7 USART. A receive takes from RDR, or
 * times out.
 */
static int one_wire;
static int rdr_full;
static uint8_t rdr;
static int ore;			/* overrun: a byte was lost */
static int max_in_flight;	/* most bytes sent before one was read */
static int in_flight;

static void reset(void)
{
	calls[0] = '\0';
	next_status = HAL_OK;
	timeout_after_send = stale_bytes = ore_cleared = 0;
	sent_len = 0;
	one_wire = rdr_full = ore = max_in_flight = in_flight = 0;
}

static void called(char c)
{
	size_t n = strlen(calls);

	assert(n + 1 < sizeof(calls));
	calls[n] = c;
	calls[n + 1] = '\0';
}

int stub_uart_get_flag(UART_HandleTypeDef *h, uint32_t flag)
{
	assert(h == &uart_handle && flag == UART_FLAG_RXNE);
	return stale_bytes > 0;
}

void stub_uart_clear_ore(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('O');
	ore_cleared++;
}

void stub_uart_flush(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('F');
	stale_bytes--;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *data,
				    uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle && sent_len + size <= sizeof(sent));
	called('W');
	memcpy(sent + sent_len, data, size);
	sent_len += size;
	if (one_wire) {
		for (uint16_t i = 0; i < size; i++) {
			if (rdr_full)
				ore = 1;	/* the echo is lost */
			else
				rdr = data[i];
			rdr_full = 1;
			if (++in_flight > max_in_flight)
				max_in_flight = in_flight;
		}
		return HAL_OK;
	}
	if (!timeout_after_send)
		return next_status;
	next_status = HAL_TIMEOUT;	/* for the receive that follows */
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle);
	called('R');
	last_size = size;
	if (one_wire) {
		for (uint16_t i = 0; i < size; i++) {
			if (!rdr_full)
				return HAL_TIMEOUT;
			data[i] = rdr;
			rdr_full = 0;
			in_flight = 0;
		}
		return HAL_OK;
	}
	if (next_status == HAL_OK)
		memcpy(data, reply, size);
	return next_status;
}

/* The SPI device behind a software chip select, and the RSTn pin. */
static SPI_HandleTypeDef spi_handle;
static GPIO_TypeDef cs_port, rst_port;
static uint8_t spi_out[40];	/* what the last transaction clocked out */
static uint16_t spi_size;
static uint8_t spi_in[40];	/* what the device clocks back */

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	if (port == &rst_port) {
		assert(pin == GPIO_PIN_15);
		called(state == GPIO_PIN_RESET ? 'E' : 'D');
		return;
	}
	assert(port == &cs_port && pin == GPIO_PIN_4);
	called(state == GPIO_PIN_RESET ? 'L' : 'H');
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, const uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &spi_handle && size <= sizeof(spi_out));
	called('T');
	memcpy(spi_out, data, size);
	spi_size = size;
	return next_status;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
					  const uint8_t *tx, uint8_t *rx,
					  uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &spi_handle && size <= sizeof(spi_out));
	called('X');
	memcpy(spi_out, tx, size);
	spi_size = size;
	if (next_status == HAL_OK)
		memcpy(rx, spi_in, size);
	return next_status;
}

/* The I2C device at 0x2A: Mem_Read, Mem_Write or a bare Master_Transmit. */
static I2C_HandleTypeDef i2c_handle;
static char i2c_last;		/* r, w or b */
static uint16_t i2c_addr, i2c_mem;

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && mem_size == I2C_MEMADD_SIZE_8BIT);
	i2c_last = 'r';
	i2c_addr = addr;
	i2c_mem = mem;
	memcpy(data, reply, size);
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && mem_size == I2C_MEMADD_SIZE_8BIT);
	i2c_last = 'w';
	i2c_addr = addr;
	i2c_mem = mem;
	memcpy(sent, data, size);
	sent_len = size;
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t addr,
					  uint8_t *data, uint16_t size,
					  uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && size == 1);
	i2c_last = 'b';
	i2c_addr = addr;
	i2c_mem = data[0];
	return next_status;
}

/* HAL_I2C_IsDeviceReady(): what it answers, and how it was asked. */
static HAL_StatusTypeDef ready_status;
static uint16_t ready_addr;
static uint32_t ready_trials;

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle);
	ready_addr = addr;
	ready_trials = trials;
	return ready_status;
}

static void test_i2c(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	struct uwb_bus bus;
	uint8_t rx[2] = { 0 };

	reset();
	assert(uwb_i2c_attach(&bus, &i2c_handle, 0x2A, 0, 10) == 0);
	assert(bus.ops->kind == UWB_BUS_I2C);

	/* The 7-bit address goes to the HAL shifted. */
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 2) == 0);
	assert(i2c_last == 'r' && i2c_addr == 0x54 && i2c_mem == 0x0F);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	assert(bus.ops->transfer(&bus, 0x20, args, 2, NULL, 0) == 0);
	assert(i2c_last == 'w' && i2c_mem == 0x20 && sent_len == 2 &&
	       sent[1] == 0x22);
	assert(bus.ops->transfer(&bus, 0x06, NULL, 0, NULL, 0) == 0);
	assert(i2c_last == 'b' && i2c_mem == 0x06);

	/* tx and rx together is not an I2C transaction; failures mapped. */
	assert(bus.ops->transfer(&bus, 0x20, args, 2, rx, 1) == -EINVAL);
	next_status = HAL_ERROR;
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 1) == -EIO);
	next_status = HAL_OK;

	/* Neither exchange() nor receive() on I2C. */
	assert(uwb_bus_exchange(&bus, args, rx, 1) == -EOPNOTSUPP);
	assert(uwb_bus_receive(&bus, rx, 1) == -EOPNOTSUPP);
	uwb_bus_detach(&bus);
}

static void test_spi(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[UWB_BUS_MAX_LEN] = { 0 };
	struct uwb_bus bus;
	uint8_t rx[3] = { 0 };

	/* Deselected at attach, so the first transfer starts on a fall. */
	reset();
	assert(uwb_spi_attach(&bus, &spi_handle, &cs_port, GPIO_PIN_4,
			      100) == 0);
	assert(bus.ops->kind == UWB_BUS_SPI);
	assert(strcmp(calls, "H") == 0);

	/* A read: CS low, cmd then zeros full duplex, CS high. */
	reset();
	memcpy(spi_in, "\xFF\x12\x34\x56", 4);
	memset(spi_out, 0xAA, sizeof(spi_out));
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 3) == 0);
	assert(strcmp(calls, "LXH") == 0 && spi_size == 4);
	assert(spi_out[0] == 0x8D && spi_out[1] == 0 && spi_out[2] == 0 &&
	       spi_out[3] == 0);
	assert(rx[0] == 0x12 && rx[1] == 0x34 && rx[2] == 0x56);

	/* A write: transmit only. */
	reset();
	assert(bus.ops->transfer(&bus, 0x0D, args, 2, NULL, 0) == 0);
	assert(strcmp(calls, "LTH") == 0 && spi_size == 3);
	assert(spi_out[0] == 0x0D && spi_out[1] == 0x11 && spi_out[2] == 0x22);

	/* CS goes high again on a failure; rx is left alone. */
	reset();
	rx[0] = 0x99;
	next_status = HAL_TIMEOUT;
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 1) == -ETIMEDOUT);
	assert(strcmp(calls, "LXH") == 0 && rx[0] == 0x99);

	/* Too long, tx and rx together: refused before CS moves. */
	reset();
	assert(bus.ops->transfer(&bus, 0x0D, many, sizeof(many), rx, 1) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	/* exchange(): one frame under one chip select, full duplex -- the
	 * five bytes, status first. */
	{
		static const uint8_t frame[5] = { 0xA1, 1, 2, 3, 4 };
		uint8_t back[5] = { 0 };
		uint8_t big[2 + UWB_BUS_MAX_LEN] = { 0 };

		reset();
		memcpy(spi_in, "\x09\xDE\xAD\xBE\xEF", 5);
		assert(uwb_bus_exchange(&bus, frame, back, 5) == 0);
		assert(strcmp(calls, "LXH") == 0 && spi_size == 5);
		assert(memcmp(spi_out, frame, 5) == 0);
		assert(memcmp(back, "\x09\xDE\xAD\xBE\xEF", 5) == 0);

		/* Refused before CS moves; CS high again after a failure. */
		reset();
		assert(uwb_bus_exchange(&bus, frame, back, 0) == -EINVAL);
		assert(uwb_bus_exchange(&bus, big, big, sizeof(big)) ==
		       -EINVAL);
		assert(calls[0] == '\0');
		next_status = HAL_BUSY;
		back[0] = 0x55;
		assert(uwb_bus_exchange(&bus, frame, back, 5) == -EAGAIN);
		assert(strcmp(calls, "LXH") == 0 && back[0] == 0x55);
	}

	/* No receive() on SPI. */
	assert(uwb_bus_receive(&bus, rx, 1) == -EOPNOTSUPP);
	uwb_bus_detach(&bus);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	static const struct {
		HAL_StatusTypeDef status;
		int expected;
	} map[] = {
		{ HAL_OK,	0 },
		{ HAL_ERROR,	-EIO },
		{ HAL_BUSY,	-EAGAIN },
		{ HAL_TIMEOUT,	-ETIMEDOUT },
	};
	uint8_t rx[2] = { 0 }, many[UWB_BUS_MAX_LEN + 1] = { 0 };
	struct uwb_bus bus;

	assert(uwb_uart_attach(&bus, &uart_handle, 100) == 0);
	assert(bus.ops->kind == UWB_BUS_UART);

	/* Stale input dropped first: overrun cleared, two bytes read out. */
	reset();
	stale_bytes = 2;
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(calls, "OFFWR") == 0);
	assert(stale_bytes == 0 && ore_cleared == 1);
	assert(sent_len == 3 && sent[0] == 0x13 && sent[1] == 0x11 &&
	       sent[2] == 0x22);
	assert(last_size == 2 && rx[0] == 0x12 && rx[1] == 0x34);

	/* No reply wanted: no receive. A failed send: no receive either. */
	reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "OW") == 0);
	reset();
	next_status = HAL_BUSY;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EAGAIN);
	assert(strcmp(calls, "OW") == 0);

	/* A reply that stops short: the send passes, the receive times out. */
	reset();
	timeout_after_send = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);
	assert(strcmp(calls, "OWR") == 0);

	/* HAL status to errno, through a send and a receive. */
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		reset();
		next_status = map[i].status;
		assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 1) ==
		       map[i].expected);
	}

	/* Too much to send is refused before the line is touched. */
	reset();
	assert(bus.ops->transfer(&bus, 0x13, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	/* receive(): the bytes, nothing sent, nothing dropped first. */
	reset();
	stale_bytes = 1;
	memcpy(reply, "\x56\x78", 2);
	assert(uwb_bus_receive(&bus, rx, 2) == 0);
	assert(strcmp(calls, "R") == 0 && last_size == 2);
	assert(rx[0] == 0x56 && rx[1] == 0x78 && stale_bytes == 1);
	reset();
	next_status = HAL_TIMEOUT;
	assert(uwb_bus_receive(&bus, rx, 2) == -ETIMEDOUT);

	uwb_bus_detach(&bus);
}

/*
 * A single wire, where the MCU hears every byte it sends. An 8-byte
 * write datagram through exchange() goes a byte at a time, each echo read
 * before the next byte leaves, so nothing ever overruns and every echo
 * comes back in order. The same datagram as one blocking transmit, as
 * transfer() sends it, overruns the data register at its second byte --
 * which is why the chip needs exchange().
 */
static void test_uart_echo(void)
{
	static const uint8_t dg[8] = { 0x05, 0x00, 0x83, 0x00, 0x00, 0x02,
				       0x00, 0x6B };
	uint8_t echo[8] = { 0 }, big[2 + UWB_BUS_MAX_LEN] = { 0 };
	struct uwb_bus bus;

	assert(uwb_uart_attach(&bus, &uart_handle, 20) == 0);

	reset();
	one_wire = 1;
	assert(uwb_bus_exchange(&bus, dg, echo, sizeof(dg)) == 0);
	assert(!ore && max_in_flight == 1 && !rdr_full);
	assert(memcmp(echo, dg, sizeof(dg)) == 0);
	assert(sent_len == 8 && memcmp(sent, dg, 8) == 0);
	/* Stale input dropped first, then send and take, byte by byte. */
	assert(strcmp(calls, "OWRWRWRWRWRWRWRWR") == 0);

	/* The control: a whole datagram in one transmit overruns. */
	reset();
	one_wire = 1;
	assert(bus.ops->transfer(&bus, dg[0], dg + 1, 7, NULL, 0) == 0);
	assert(ore && max_in_flight == 8);

	/* A byte whose echo never comes is a timeout; failures mapped. */
	reset();
	timeout_after_send = 1;
	assert(uwb_bus_exchange(&bus, dg, echo, sizeof(dg)) ==
	       -ETIMEDOUT);
	assert(strcmp(calls, "OWR") == 0);
	reset();
	next_status = HAL_ERROR;
	assert(uwb_bus_exchange(&bus, dg, echo, sizeof(dg)) == -EIO);
	assert(strcmp(calls, "OW") == 0);

	/* Nothing, or too much, is refused before the line is touched. */
	reset();
	assert(uwb_bus_exchange(&bus, dg, echo, 0) == -EINVAL);
	assert(uwb_bus_exchange(&bus, big, big, sizeof(big)) == -EINVAL);
	assert(calls[0] == '\0');
	uwb_bus_detach(&bus);
}

static void test_probe(void)
{
	/* No ACK is HAL_ERROR there, -ENXIO here; the address goes shifted. */
	ready_status = HAL_ERROR;
	assert(uwb_i2c_probe(&i2c_handle, 0x2A, 100) == -ENXIO);
	assert(ready_addr == 0x54 && ready_trials == 1);
	ready_status = HAL_BUSY;
	assert(uwb_i2c_probe(&i2c_handle, 0x2A, 100) == -EAGAIN);
	ready_status = HAL_TIMEOUT;
	assert(uwb_i2c_probe(&i2c_handle, 0x2A, 100) == -ETIMEDOUT);
	ready_status = HAL_OK;
	assert(uwb_i2c_probe(&i2c_handle, 0x2A, 100) == 0);
}

/*
 * RSTn: an open-drain output the app set up. Low pulls it down; a 1 lets
 * it float, so the DW1000 releases itself -- it is never driven high
 * (DS 5.6.3.1). A NULL port is not wired.
 */
static void test_reset_pin(void)
{
	struct uwb_pin rst = { .port = &rst_port, .pin = GPIO_PIN_15 };
	struct uwb_pin none = { .port = NULL };

	reset();
	assert(uwb_pin_wired(&rst) && !uwb_pin_wired(&none));
	assert(calls[0] == '\0');
	uwb_pin_drive_low(&rst);
	uwb_pin_release(&rst);
	assert(strcmp(calls, "ED") == 0);
}

/* Core cycles one delay took, in the stub's SysTick time. */
static uint64_t delay_cycles(uint32_t us)
{
	uint64_t before = stub_cycles;

	uwb_delay_us(us);
	return stub_cycles - before;
}

/*
 * Never early. At 16 MHz, us microseconds are 16 * us core cycles; the
 * delay may run over by the cost of its own reads, never under -- across
 * SysTick's reload too, from any phase of the counter.
 */
static void test_delay(void)
{
	SystemCoreClock = 16000000;
	stub_systick_step = 1;

	for (uint32_t us = 1; us <= 64; us++)
		assert(delay_cycles(us) >= 16u * us);

	/* Right across the reload, from every phase of a short period. */
	SysTick->LOAD = 99;
	for (uint32_t val = 0; val < 100; val++) {
		SysTick->VAL = val;
		assert(delay_cycles(1) >= 16);
		SysTick->VAL = val;
		assert(delay_cycles(64) >= 16u * 64);
	}

	/* Coarse reads, many cycles between two looks: still not early. */
	stub_systick_step = 7;
	for (uint32_t us = 1; us <= 64; us += 9)
		assert(delay_cycles(us) >= 16u * us);
	stub_systick_step = 1;

	/* SysTick on the core clock / 8 counts eight times slower. */
	SysTick->CTRL &= ~SysTick_CTRL_CLKSOURCE_Msk;
	assert(delay_cycles(64) >= 16u * 64);
	assert(delay_cycles(64) < 2u * 16u * 64);

	/*
	 * The boundary, exactly. At core / 8 one count is 8 cycles, longer
	 * than the delay's own reads, so where the first and last reads fall
	 * within a count decides it: from every phase of a count, every
	 * counter value across a short reload, and reads 1 to 8 cycles apart,
	 * the wait is never under us microseconds -- and a count's worth over
	 * the ask is all it may add beyond its reads.
	 */
	SysTick->LOAD = 9;
	for (uint32_t step = 1; step <= 8; step++) {
		stub_systick_step = step;
		for (uint32_t phase = 0; phase < 8; phase++) {
			for (uint32_t val = 0; val <= 9; val++) {
				for (uint32_t us = 1; us <= 4; us++) {
					uint64_t c;

					stub_systick_phase = phase;
					SysTick->VAL = val;
					c = delay_cycles(us);
					assert(c >= 16u * us);
					assert(c <= 16u * us + 8u + 4u * step +
						    8u);
				}
			}
		}
	}
	stub_systick_step = 1;
	SysTick->LOAD = 16000 - 1;
	SysTick->CTRL |= SysTick_CTRL_CLKSOURCE_Msk;

	/* A slower core: fewer cycles for the same time, never fewer than
	 * that time. */
	SystemCoreClock = 4000000;
	assert(delay_cycles(10) >= 40);
	SystemCoreClock = 16000000;

	/* SysTick stopped: the busy loop instead, which does not look at it. */
	SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
	assert(delay_cycles(64) == 1);		/* the one CTRL read */
	SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
	SysTick->LOAD = 16000 - 1;
}

/* No scheduler: the yield is a millisecond's HAL_Delay(), which adds a
 * tick, so never short. */
static void test_yield(void)
{
	stub_tick = 0;
	uwb_yield();
	assert(stub_tick == 2);
}

/* The weak clock: HAL_GetTick() milliseconds, as microseconds. */
static void test_time(void)
{
	stub_tick = 1234;
	assert(uwb_time_us() == 1234000);
}

int main(void)
{
	test_i2c();
	test_spi();
	test_uart();
	test_uart_echo();
	test_probe();
	test_reset_pin();
	test_delay();
	test_yield();
	test_time();
	printf("test_port_stm: all passed\n");
	return 0;
}
