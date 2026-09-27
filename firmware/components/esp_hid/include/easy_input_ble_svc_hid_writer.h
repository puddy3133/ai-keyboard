/*
 * SPDX-FileCopyrightText: 2026 EasyInput contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connection-aware companion to NimBLE's report-write callback.
 *
 * ESP-IDF v5.5.5's public ble_svc_hid_report_write_cb_t omits the writer's
 * connection handle even though ble_svc_hid_access() owns it. That is unsafe
 * for chunked FEATURE reports when an HID host and an auxiliary GATT client
 * coexist. The pinned local service adapter publishes the exact handle here.
 */
typedef void (*easy_input_ble_svc_hid_report_write_cb_t)(
    uint16_t conn_handle,
    uint16_t attr_handle,
    uint8_t report_type,
    uint8_t report_id,
    const uint8_t *data,
    uint16_t len);

void easy_input_ble_svc_hid_register_report_write_cb(
    easy_input_ble_svc_hid_report_write_cb_t cb);

#ifdef __cplusplus
}
#endif
