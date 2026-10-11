#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include <stdint.h>

/* Host stub: the clock samples are stamped with. Tests set stub_time_us. */
extern int64_t stub_time_us;

int64_t esp_timer_get_time(void);

#endif
