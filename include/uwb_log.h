/*
 * Kernel dev_err()-style logging for UWB drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "uwb", prefixed
 * with the device's name, so one esp_log_level_set("uwb", ...) controls
 * every chip and a line still says which chip it came from. There is
 * deliberately no level machinery of our own; uwb_dbg() is also subject
 * to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_UWB_DEBUG is not used.
 *
 * STM32 HAL: every line goes out through printf() as "uwb: <name>: ...",
 * so a line still says which chip it came from. Where printf() ends up is
 * the app's business: with newlib, retarget _write() to a UART. There is
 * deliberately no level machinery; uwb_dbg() alone is compiled out
 * unless CONFIG_UWB_DEBUG is 1 (stm-forge: UWB_DEBUG = y).
 */
#ifndef UWB_LOG_H
#define UWB_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "uwb.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define UWB_LOG_TAG	"uwb"

#define uwb_err(uwb, fmt, ...) \
	ESP_LOGE(UWB_LOG_TAG, "%s: " fmt, (uwb)->name, ##__VA_ARGS__)
#define uwb_warn(uwb, fmt, ...) \
	ESP_LOGW(UWB_LOG_TAG, "%s: " fmt, (uwb)->name, ##__VA_ARGS__)
#define uwb_info(uwb, fmt, ...) \
	ESP_LOGI(UWB_LOG_TAG, "%s: " fmt, (uwb)->name, ##__VA_ARGS__)
#define uwb_dbg(uwb, fmt, ...) \
	ESP_LOGD(UWB_LOG_TAG, "%s: " fmt, (uwb)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define uwb_err(uwb, fmt, ...) \
	printf("uwb: %s: " fmt "\n", (uwb)->name, ##__VA_ARGS__)
#define uwb_warn(uwb, fmt, ...) \
	printf("uwb: %s: " fmt "\n", (uwb)->name, ##__VA_ARGS__)
#define uwb_info(uwb, fmt, ...) \
	printf("uwb: %s: " fmt "\n", (uwb)->name, ##__VA_ARGS__)

#if CONFIG_UWB_DEBUG
#define uwb_dbg(uwb, fmt, ...) \
	printf("uwb: %s: " fmt "\n", (uwb)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define uwb_dbg(uwb, fmt, ...) do { \
	if (0) \
		printf("uwb: %s: " fmt "\n", (uwb)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: channel, PRF, data rate, preamble length, address. */
void uwb_log_config(const struct uwb_dev *uwb);

#ifdef __cplusplus
}
#endif

#endif /* UWB_LOG_H */
