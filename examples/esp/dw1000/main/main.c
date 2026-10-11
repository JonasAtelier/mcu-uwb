/*
 * Ranging demo. Flash two boards: one with ROLE_INITIATOR set to 1 (the tag),
 * one with it set to 0 (the anchor). Both print the measured distance.
 *
 * Channel, rate, PRF, preamble length and preamble code must match on both.
 */
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dw1000.h"
#include "dw1000_twr.h"
#include "uwb_log.h"

#define ROLE_INITIATOR 1

/* Ranging addresses. With more anchors, give each its own and range to each. */
#define TAG_ADDR    1
#define ANCHOR_ADDR 2

#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_SCLK 18
#define PIN_CS    4
#define PIN_RST  27
#define PIN_IRQ  34

static const char *TAG = "ranging";

void app_main(void)
{
    spi_bus_config_t bus = {
        .miso_io_num     = PIN_MISO,
        .mosi_io_num     = PIN_MOSI,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 256,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    dw1000_config_t cfg = {
        .host          = SPI2_HOST,
        .cs_gpio       = PIN_CS,
        .rst_gpio      = PIN_RST,
        .irq_gpio      = PIN_IRQ,
        .channel       = 5,
        .rate          = DW_RATE_6M8,
        .prf           = DW_PRF_64M,
        .preamble_len  = 128,
        .preamble_code = 9,
        .antenna_delay = 16436, /* calibrate this -- see the README */
        .addr          = ROLE_INITIATOR ? TAG_ADDR : ANCHOR_ADDR,
    };

    dw1000_handle_t dw;
    ESP_ERROR_CHECK(dw1000_init(&cfg, &dw));
    uwb_log_config(dw1000_uwb(dw));
    ESP_LOGI(TAG, "DW1000 up, running as %s", ROLE_INITIATOR ? "initiator" : "responder");

    while (1) {
        float d;
        esp_err_t err;

#if ROLE_INITIATOR
        err = dw1000_twr_range(dw, ANCHOR_ADDR, &d);
        vTaskDelay(pdMS_TO_TICKS(100));
#else
        err = dw1000_twr_respond(dw, &d, 1000);
#endif

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "distance %.3f m", d);
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "range failed: %s", esp_err_to_name(err));
        }
    }
}
