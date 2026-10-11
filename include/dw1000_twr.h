/*
 * Double-sided two-way ranging on top of the dw1000 driver.
 *
 * Run dw1000_twr_respond() in a loop on the anchor and dw1000_twr_range() on
 * the tag. Four frames are exchanged; the responder computes the distance and
 * reports it back, so both ends end up with the same number.
 *
 * One exchange at a time per handle. Each SPI access is locked, so other
 * tasks may read timestamps or the clock meanwhile, but two exchanges on one
 * radio would receive each other's frames.
 */
#pragma once

#include "dw1000.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
/* Run one ranging exchange as the initiator, against the responder whose
 * config.addr is `dst`. On success *distance_m holds the measured distance in
 * metres. ESP_ERR_TIMEOUT if the responder never answers,
 * ESP_ERR_INVALID_RESPONSE if a frame from another exchange got in the way. */
esp_err_t dw1000_twr_range(dw1000_handle_t dev, uint16_t dst, float *distance_m);

/* Serve one ranging exchange as the responder. POLLs addressed to another
 * responder are ignored. *distance_m is optional. ESP_ERR_TIMEOUT if no
 * initiator called within timeout_ms.
 *
 * Either side returns ESP_ERR_INVALID_STATE if it missed its own reply slot:
 * the task was held off too long, or the preamble is too long to fit the
 * reply delay. Nothing was sent. The first is worth a retry; the second is
 * not, since a preamble of 1536 symbols or more never fits the ~1.5 ms. */
esp_err_t dw1000_twr_respond(dw1000_handle_t dev, float *distance_m, uint32_t timeout_ms);
#else
/* As on ESP-IDF: -ETIMEDOUT if the peer never answers, -EBADMSG if a frame
 * from another exchange got in the way, -ETIME if this side missed its own
 * reply slot (nothing was sent). */
int dw1000_twr_range(dw1000_handle_t dev, uint16_t dst, float *distance_m);
int dw1000_twr_respond(dw1000_handle_t dev, float *distance_m, uint32_t timeout_ms);
#endif

#ifdef __cplusplus
}
#endif
