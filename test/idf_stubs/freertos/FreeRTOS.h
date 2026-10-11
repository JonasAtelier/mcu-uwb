#ifndef FREERTOS_H
#define FREERTOS_H

#include <assert.h>
#include <stdint.h>

/*
 * Host stub for FreeRTOS: ticks, for the bus port's UART timeouts and the
 * chips' polling. The tick rate is a variable so a test can move it; it
 * starts at ESP-IDF's default 100 Hz (see idf_stubs.c), where
 * pdMS_TO_TICKS() of anything under 10 ms is 0. Same macros as the real one;
 * pdMS_TO_TICKS truncates as IDF's does.
 */
extern unsigned int stub_tick_hz;

#define configTICK_RATE_HZ	stub_tick_hz
#define portTICK_PERIOD_MS	((TickType_t)1000 / configTICK_RATE_HZ)
#define portMAX_DELAY		0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) \
	((TickType_t)((TickType_t)(ms) * configTICK_RATE_HZ / 1000U))
#define pdTRUE			1
#define pdFALSE			0
#define portYIELD_FROM_ISR()	((void)0)

typedef uint32_t TickType_t;
typedef int BaseType_t;

#endif
