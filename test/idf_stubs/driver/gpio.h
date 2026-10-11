#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

#include "esp_err.h"

/* Host stub for the ESP-IDF gpio driver: only what the drivers use. */

typedef int gpio_num_t;

#define GPIO_NUM_NC	(-1)

typedef enum {
	GPIO_MODE_DISABLE = 0,
	GPIO_MODE_INPUT = 1,
	GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

typedef enum {
	GPIO_INTR_DISABLE = 0,
	GPIO_INTR_POSEDGE = 1,
} gpio_int_type_t;

typedef void (*gpio_isr_t)(void *arg);

typedef struct {
	uint64_t pin_bit_mask;
	gpio_mode_t mode;
	int pull_up_en;
	int pull_down_en;
	int intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level);
int gpio_get_level(gpio_num_t gpio);
esp_err_t gpio_set_direction(gpio_num_t gpio, gpio_mode_t mode);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, gpio_isr_t isr_handler,
			       void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio);

#endif
