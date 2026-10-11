#ifndef I2C_MASTER_H
#define I2C_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Host stub for the ESP-IDF 5.2+ i2c_master API. */

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

typedef enum {
	I2C_ADDR_BIT_LEN_7 = 0,
} i2c_addr_bit_len_t;

typedef struct {
	i2c_addr_bit_len_t dev_addr_length;
	uint16_t device_address;
	uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
/*
 * As in IDF: ESP_ERR_NOT_FOUND on NACK. Transfers that are NACKed return
 * ESP_ERR_INVALID_STATE instead, and a fake should model that.
 */
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
			      const uint8_t *data, size_t length,
			      int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
				      const uint8_t *tx, size_t tx_length,
				      uint8_t *rx, size_t rx_length,
				      int timeout_ms);

#endif
