/* Shared internals between dw1000.c and dw1000_twr.c. Not a public header. */
#pragma once

#include <stdbool.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include "uwb_priv.h"
#include "dw1000.h"

struct dw1000_t {
    struct uwb_dev      uwb;     /* the category view; priv points back here */
    struct uwb_bus      bus;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t   lock;    /* held per SPI transaction */
    SemaphoreHandle_t   irq_sem; /* given by the IRQ ISR; NULL when polling */
    gpio_num_t          irq_gpio;
#else
    volatile bool       irq_seen; /* set by dw1000_irq() */
    bool                use_irq;
#endif
    struct uwb_pin      rst;
    uint32_t            tx_fctrl_base;
    uint16_t            antenna_delay;
    uint16_t            addr; /* this node's ranging address */
    uint8_t             seq;  /* last ranging sequence number sent */
};

int dw_write(dw1000_handle_t d, uint8_t reg, uint16_t sub, const void *data, size_t len);
int dw_read(dw1000_handle_t d, uint8_t reg, uint16_t sub, void *data, size_t len);
int dw_write32(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint32_t v);
int dw_write16(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint16_t v);
int dw_write8(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint8_t v);
int dw_read32(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint32_t *v);
int dw_read40(dw1000_handle_t d, uint8_t reg, uint16_t sub, uint64_t *v);

int  dw_prepare_tx(dw1000_handle_t d, const void *data, size_t len);
int  dw_wait_tx_done(dw1000_handle_t d, uint32_t timeout_ms);
int  dw_trx_off(dw1000_handle_t d);
int  dw_recv(dw1000_handle_t d, void *buf, size_t cap, size_t *len, uint32_t timeout_ms);
int  dw_send(dw1000_handle_t d, const void *data, size_t len);
int  dw_wait_rx(dw1000_handle_t d, void *buf, size_t cap, size_t *len, uint32_t timeout_ms);
