#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Host stub for the slice of the ESP-IDF UART driver API the port uses. */

typedef int uart_port_t;

bool uart_is_driver_installed(uart_port_t uart_num);
esp_err_t uart_flush_input(uart_port_t uart_num);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length,
		    TickType_t ticks_to_wait);

#endif
