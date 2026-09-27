/*
 * SPDX-FileCopyrightText: 2026 EasyInput contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_hidd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sentinel returned through owner_conn_handle when no HID-input subscriber owns
 * the transport.  This matches NimBLE's BLE_HS_CONN_HANDLE_NONE value without
 * exposing NimBLE headers to users of the esp_hid public API.
 */
#define EASY_INPUT_HIDD_OWNER_NONE UINT16_C(0xFFFF)

/**
 * Identity of one concrete HID-owner lifetime.
 *
 * NimBLE connection handles are small numeric slots and may be reused after a
 * disconnect or host reset.  generation changes whenever ownership is lost or
 * acquired, so the pair remains fresh even when conn_handle is reused.
 */
typedef struct {
    uint16_t conn_handle;
    uint32_t generation;
} easy_input_hidd_owner_snapshot_t;

/**
 * Authoritative adapter lifecycle state, including periods with no HID owner.
 *
 * Unlike easy_input_hidd_owner_snapshot_get(), this snapshot succeeds for a
 * live HIDD device even when the host reset or disconnected. generation still
 * advances at owner boundaries. host_generation advances on every NimBLE host
 * reset and host_synced becomes true only after the matching sync callback, so
 * advertising recovery never depends on a best-effort START event.
 */
typedef struct {
    uint16_t conn_handle;
    uint32_t generation;
    uint32_t host_generation;
    bool connected;
    bool host_synced;
} easy_input_hidd_lifecycle_snapshot_t;

/**
 * Synchronous handler for one application-owned FEATURE report.
 *
 * The local NimBLE HIDD adapter normally forwards FEATURE writes through an
 * independent esp_event task. EasyInput configuration needs a stronger
 * lifecycle contract: admission must be recorded while the NimBLE write
 * callback is still in progress so Deep Sleep cannot overtake a queued
 * configuration chunk. conn_handle is the actual writer supplied by
 * ble_svc_hid_access(), not the current HID owner. A registered report is
 * consumed by this callback and is not posted to the ordinary HIDD event
 * loop.
 */
typedef void (*easy_input_hidd_feature_write_handler_t)(
    void *context,
    uint16_t conn_handle,
    const uint8_t *data,
    size_t length);

/**
 * Return the current BLE HID owner and its lifetime generation.
 *
 * On ESP_ERR_INVALID_STATE, conn_handle is EASY_INPUT_HIDD_OWNER_NONE.  The
 * generation may still describe the most recent ownership transition and must
 * only be used as an identity token when this function returns ESP_OK.
 */
esp_err_t easy_input_hidd_owner_snapshot_get(
    esp_hidd_dev_t *dev,
    easy_input_hidd_owner_snapshot_t *owner_snapshot);

esp_err_t easy_input_hidd_lifecycle_snapshot_get(
    esp_hidd_dev_t *dev,
    easy_input_hidd_lifecycle_snapshot_t *lifecycle_snapshot);

/**
 * Route one FEATURE report ID synchronously from the NimBLE host callback.
 *
 * Registration must happen before the host task starts. The callback runs
 * without the HIDD state locks held. Device deinitialization closes admission
 * and waits for any callback already in progress before returning.
 */
esp_err_t easy_input_hidd_feature_write_handler_set(
    esp_hidd_dev_t *dev,
    uint8_t report_id,
    easy_input_hidd_feature_write_handler_t handler,
    void *context);

/**
 * Terminal-only shutdown of the adapter's independent esp_event plane.
 *
 * This closes new HIDD event publication, waits for publishers and a handler
 * already executing, then deletes the event-loop task. It deliberately does
 * not terminate GAP links, stop GATT, or free the HIDD object; NimBLE owns
 * those protocol resources until nimble_port_stop()/nimble_port_deinit().
 * Deep Sleep resets the remaining RAM immediately afterward.
 *
 * Must be called from the application owner task, never from a HIDD event
 * callback. This operation is irreversible.
 */
esp_err_t easy_input_hidd_event_plane_shutdown(esp_hidd_dev_t *dev);

/**
 * Request termination only if the supplied owner lifetime is still current.
 *
 * Exact owner validation and ble_gap_terminate() submission share the same
 * owner gate used by notification submission and lifecycle changes, so a
 * recycled numeric connection handle cannot cause a newer host to be torn
 * down by an older recovery request.
 */
esp_err_t easy_input_hidd_owner_terminate(
    esp_hidd_dev_t *dev,
    const easy_input_hidd_owner_snapshot_t *expected_owner,
    uint8_t hci_reason);

/**
 * Submit an INPUT report only to the exact BLE HID-owner lifetime supplied.
 *
 * The owner identity check and NimBLE notification submission are serialized
 * with owner acquisition, disconnect, and host-reset generation changes.
 * This closes the connection-handle reuse race between a caller taking an
 * owner snapshot and later submitting the report.
 *
 * The report buffer is copied by NimBLE before this function returns.  The
 * ordinary esp_hidd_dev_input_set() API remains available for source
 * compatibility and targets whichever owner is current at submission time.
 *
 * @param dev HID device returned by esp_hidd_dev_init().
 * @param expected_owner Exact owner snapshot previously returned by
 *                       easy_input_hidd_owner_snapshot_get().
 * @param map_index HID report-map index, with the same semantics as
 *                  esp_hidd_dev_input_set().
 * @param report_id HID INPUT report identifier.
 * @param data Report payload, or NULL only when length is zero.
 * @param length Report payload length.
 *
 * @return ESP_OK after NimBLE accepts the notification; ESP_ERR_INVALID_STATE
 *         if the owner has disappeared or its handle/generation no longer
 *         matches; ESP_ERR_NO_MEM or ESP_ERR_NOT_FINISHED for recoverable
 *         transport pressure; another esp_err_t for invalid input or a
 *         backend error.
 */
esp_err_t easy_input_hidd_dev_input_set_for_owner(
    esp_hidd_dev_t *dev,
    const easy_input_hidd_owner_snapshot_t *expected_owner,
    size_t map_index,
    size_t report_id,
    uint8_t *data,
    size_t length);

/**
 * Return the BLE connection that currently owns HID input delivery.
 *
 * The owner is the first connection that enables notifications for a standard
 * keyboard or mouse INPUT report (including a restored bonded subscription).
 * Vendor-defined INPUT reports and auxiliary configuration connections never
 * establish or replace an owner.
 *
 * @param dev HID device returned by esp_hidd_dev_init().
 * @param owner_conn_handle Receives the NimBLE connection handle on success,
 *                          or EASY_INPUT_HIDD_OWNER_NONE on failure.
 *
 * @return ESP_OK when an owner exists; ESP_ERR_INVALID_STATE when no owner is
 *         active; ESP_ERR_INVALID_ARG for invalid pointers.
 */
esp_err_t easy_input_hidd_owner_conn_handle_get(
    esp_hidd_dev_t *dev,
    uint16_t *owner_conn_handle);

#ifdef __cplusplus
}
#endif
