/*
 * Host-side check of the core on ESP-IDF: the cases are ../core_cases.h;
 * this file supplies the log prefix.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "uwb.h"
#include "uwb_log.h"

/* The ESP-IDF stub log puts the level letter first. */
#define LOG_PREFIX(level)	level " "

#include "../core_cases.h"

/* The clock is esp_timer's, as it is. */
static void test_clock(void)
{
	stub_time_us = 123456789;
	assert(uwb_time_us() == 123456789);
}

int main(void)
{
	run_core_cases();
	test_clock();
	printf("all core checks passed\n");
	return 0;
}
