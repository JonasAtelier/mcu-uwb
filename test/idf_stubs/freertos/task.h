#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#endif
