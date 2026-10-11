/*
 * Host-side check of the core on the STM32 HAL: the cases are
 * ../core_cases.h; this file supplies the log prefix, and checks the clock
 * -- the weak default, HAL_GetTick() milliseconds as microseconds.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "uwb.h"
#include "uwb_log.h"

/* The HAL's lines are bare printf(), no level letter. */
#define LOG_PREFIX(level)	""

#include "../core_cases.h"

/* 32-bit milliseconds do not wrap in the 64-bit microseconds. */
static void test_clock_range(void)
{
	stub_tick = UINT32_MAX;
	assert(uwb_time_us() == (int64_t)UINT32_MAX * 1000);
}

/* With CONFIG_UWB_DEBUG on, as test/Makefile builds it, dbg prints. */
static void test_dbg(void)
{
	struct uwb_dev u = { .name = "fake" };

	uwb_dbg(&u, "d %d", 1);
	assert(!strcmp(stub_log_last, "uwb: fake: d 1"));
}

int main(void)
{
	run_core_cases();
	test_clock_range();
	test_dbg();
	printf("all core checks passed\n");
	return 0;
}
