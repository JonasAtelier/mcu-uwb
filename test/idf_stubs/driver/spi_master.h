#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Host stub for the ESP-IDF spi_master driver: only what the bus port uses.
 * The tx_data/rx_data anonymous unions are shaped like the real header's.
 */

typedef enum {
	SPI1_HOST = 0,
	SPI2_HOST = 1,
	SPI3_HOST = 2,
} spi_host_device_t;

typedef struct spi_device_t *spi_device_handle_t;

typedef struct {
	uint8_t mode;
	int clock_speed_hz;
	int spics_io_num;
	uint32_t flags;
	int queue_size;
} spi_device_interface_config_t;

#define SPI_TRANS_USE_RXDATA	(1 << 2)
#define SPI_TRANS_USE_TXDATA	(1 << 3)

typedef struct spi_transaction_t {
	uint32_t flags;
	uint16_t cmd;
	uint64_t addr;
	size_t length;
	size_t rxlength;
	void *user;
	union {
		const void *tx_buffer;
		uint8_t tx_data[4];
	};
	union {
		void *rx_buffer;
		uint8_t rx_data[4];
	};
} spi_transaction_t;

esp_err_t spi_bus_add_device(spi_host_device_t host_id,
			     const spi_device_interface_config_t *dev_config,
			     spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_transmit(spi_device_handle_t handle,
			      spi_transaction_t *trans_desc);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
				      spi_transaction_t *trans_desc);

#endif
