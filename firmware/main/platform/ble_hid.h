#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_hidd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "keyboard/agent_status.h"
#include "keyboard/ble_input_scheduler.h"
#include "keyboard/ble_advertising_state.h"
#include "keyboard/ble_fixed_text_stream.h"
#include "keyboard/ble_connection_profile.h"
#include "keyboard/ble_management_gate.h"
#include "keyboard/ble_owner_recovery.h"
#include "keyboard/config_receiver.h"
#include "keyboard/config_status.h"
#include "keyboard/gatt_status_snapshot.h"
#include "keyboard/hid_report_queue.h"
#include "keyboard/keymap.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace easy_input {

class BleHidTransport {
 public:
  using StatusReadCallback = void (*)(void* context);
  using WorkReadyCallback = void (*)(void* context);

  esp_err_t begin();

  bool connected() const;
  // Reconciles the application cache with the adapter's exact HID-owner
  // lifetime. Call from the main task before routing physical input.
  void refresh_connection_identity();
  std::uint32_t connection_epoch() const;
  ai_keyboard::BleOwnerToken connection_identity() const;
  bool take_pending_config(std::string* out,
                           ai_keyboard::BleOwnerToken* origin_owner);
  bool take_pending_agent_status(ai_keyboard::AgentStatusCommand* out);

  void poll_input_delivery(std::uint32_t now_ms);
  // Owner-task-only view of application input work that has not been
  // submitted yet: queued keyboard/wheel reports or a pending fixed-text
  // stream. This deliberately excludes the post-submit BLE TX grace, which is
  // a whole-device deep-sleep fence rather than runnable input work.
  bool input_reports_pending() const;
  // Includes both queued reports and the post-submit BLE TX grace. Use this
  // aggregate for whole-device power/sleep admission and disconnected-input
  // cleanup, where an in-flight submission must remain observable.
  bool input_delivery_pending() const;
  bool management_work_pending() const;
  // Returns the nearest absolute esp_timer deadline owned by BLE. False means
  // BLE is callback-driven and has no armed timer at this moment.
  bool next_work_deadline_us(std::uint64_t* deadline_us) const;
  void set_work_ready_callback(WorkReadyCallback callback, void* context);

  // Prepare is reversible: it atomically closes management admission only
  // when no callback is in flight. Commit is terminal and owner-task-only.
  bool try_begin_deep_sleep_quiesce();
  void cancel_deep_sleep_quiesce();
  esp_err_t shutdown_for_deep_sleep();
  void open_config_window(const char* reason);
  bool config_window_active();
  void update_battery_level(std::uint8_t percent);
  void set_status_read_callback(StatusReadCallback callback, void* context);
  void publish_status_json(const std::string& status_json);
  bool send_firmware_event(const char* source, const ai_keyboard::FirmwareEvent& event);
  bool send_firmware_event_for_owner(
      const char* source,
      const ai_keyboard::FirmwareEvent& event,
      ai_keyboard::BleOwnerToken expected_owner);
  // 配置/音频控制回执(0x11 输入报文),与 USB 侧格式一致。
  void send_config_ack(std::uint8_t phase_code,
                       bool ok,
                       std::uint16_t bytes,
                       std::uint16_t crc16,
                       bool saved);
  bool send_config_ack_for_owner(std::uint8_t phase_code,
                                 bool ok,
                                 std::uint16_t bytes,
                                 std::uint16_t crc16,
                                 bool saved,
                                 ai_keyboard::BleOwnerToken expected_owner);
  bool send_keyboard_report(std::uint8_t modifier, std::uint8_t keycode);
  bool send_keyboard_report(std::uint8_t modifier,
                            const std::array<std::uint8_t, 6>& keycodes,
                            bool apple_fn = false);
  bool send_keyboard_report(std::uint8_t modifier,
                            const std::array<std::uint8_t, 6>& keycodes,
                            bool apple_fn,
                            ai_keyboard::HidReportClass report_class);
  bool send_keyboard_report_for_owner(
      std::uint8_t modifier,
      const std::array<std::uint8_t, 6>& keycodes,
      bool apple_fn,
      ai_keyboard::HidReportClass report_class,
      ai_keyboard::BleOwnerToken expected_owner);
  bool send_mouse_wheel(std::int8_t vertical, std::int8_t horizontal);
  bool send_mouse_wheel_for_owner(
      std::int8_t vertical,
      std::int8_t horizontal,
      ai_keyboard::BleOwnerToken expected_owner);

  void handle_hidd_event(std::int32_t event_id, void* event_data);
  void handle_hidd_config_feature(std::uint16_t conn_handle,
                                  const std::uint8_t* data,
                                  std::size_t len);
  int handle_gap_event(ble_gap_event* event);
  int handle_config_access(std::uint16_t conn_handle,
                           std::uint16_t attr_handle,
                           ble_gatt_access_ctxt* ctxt);

 private:
  esp_err_t init_low_level();
  esp_err_t prepare_identity_address();
  esp_err_t ensure_identity_set();
  esp_err_t register_config_service();
  esp_err_t start_advertising(ai_keyboard::BleAdvertisingMode mode);
  bool start_directed_reconnect_advertising();
  ai_keyboard::BleAdvertisingMode desired_advertising_mode();
  void receive_config_report(const std::uint8_t* data,
                             std::size_t len,
                             ai_keyboard::BleOwnerToken origin_owner,
                             std::uint16_t source_conn_handle);
  bool receive_agent_status_report(const std::uint8_t* data, std::size_t len);
  bool try_enter_management_callback();
  void leave_management_callback();
  bool management_callback_in_flight() const;
  bool management_quiesce_interrupted() const;
  bool management_owner_work_pending() const;
  void notify_work_ready() const;
  bool called_from_owner_task() const;
  bool sync_hid_owner(const char* reason);
  void note_control_connection(std::uint16_t conn_handle);
  int config_write_authorization_error(std::uint16_t conn_handle) const;
  void reset_connection_state();
  void request_connection_reconcile();
  void request_stable_connection_parameters(const char* reason);
  void reconcile_stable_connection_parameters(const char* reason);
  void cache_connection_status(std::uint16_t conn_handle,
                               std::int32_t update_status);
  bool stable_connection_parameters_match_actual_locked() const;
  void schedule_connection_update_retry_locked(std::int64_t now_us);
  static ai_keyboard::BleInputTxResult transmit_scheduled_report_callback(
      void* context,
      const ai_keyboard::BleScheduledReport& report);
  ai_keyboard::BleInputTxResult transmit_scheduled_report(
      const ai_keyboard::BleScheduledReport& report);
  void recover_fatal_input_delivery(const char* reason);
  void service_owner_recovery(std::uint64_t now_us);
  bool read_hidd_lifecycle(bool* connected,
                           ai_keyboard::BleOwnerToken* owner,
                           std::uint32_t* host_generation = nullptr,
                           bool* host_synced = nullptr) const;
  void complete_owner_recovery(const char* reason);
  void request_advertising_reconcile(bool force_restart = false);
  void service_advertising_reconcile();
  void apply_deferred_input_reset(const char* reason);
  void clear_pending_input_reports(const char* reason);
  bool queued_input_delivery_pending() const;
  bool ble_tx_grace_pending(std::uint64_t now_us) const;
  void arm_ble_tx_grace(std::uint16_t conn_handle,
                        std::uint32_t expected_owner_generation = 0);
  void arm_conservative_ble_tx_grace();
  bool config_transfer_pending(std::uint64_t now_us) const;
  void begin_config_endpoint_lifetime(std::uint16_t conn_handle,
                                      std::uint32_t host_generation);
  void end_config_endpoint_lifetime(std::uint16_t conn_handle);
  void observe_config_host_generation(std::uint32_t host_generation);
  std::uint32_t config_endpoint_epoch_locked(
      std::uint16_t conn_handle) const;
  std::uint32_t next_config_endpoint_epoch_locked();
  void reset_config_transfer_locked();
  void reset_all_config_endpoint_lifetimes_locked();
  void reset_config_transfer();
  bool connected_config_window_active();
  std::int32_t connected_config_window_remaining_ms() const;
  void queue_completed_config(std::string json,
                              ai_keyboard::BleOwnerToken origin_owner);
  void pump_fixed_text_stream(std::uint32_t now_ms);
  bool send_hotkey_report(const std::string& hotkey, bool pressed);
  bool tap_hotkey(const std::string& hotkey);
  bool send_input_report(std::uint8_t report_id,
                         const std::uint8_t* data,
                         std::size_t len,
                         const char* context,
                         ai_keyboard::HidReportClass report_class,
                         ai_keyboard::BleOwnerToken expected_owner = {});
  bool send_app_command_report(std::uint8_t command_kind,
                               std::uint8_t chunk_index,
                               std::uint8_t total_chunks,
                               const std::uint8_t* data,
                               std::size_t len,
                               ai_keyboard::BleOwnerToken expected_owner = {},
                               ai_keyboard::HidReportClass report_class =
                                   ai_keyboard::HidReportClass::AppCommand);
  bool send_fixed_text_command(
      const std::string& text,
      ai_keyboard::BleOwnerToken expected_owner = {});
  bool send_hotkey_app_command(
      const std::string& hotkey,
      bool pressed,
      ai_keyboard::BleOwnerToken expected_owner = {});
  std::string status_json_for_publish(const std::string& status_json) const;
  bool copy_status_json_for_read(std::uint16_t conn_handle,
                                 std::uint16_t offset,
                                 char* out,
                                 std::size_t out_capacity,
                                 std::size_t* out_len);
  void forget_status_read_snapshot(std::uint16_t conn_handle);
  void clear_status_read_snapshots();

  esp_hidd_dev_t* hid_dev_ = nullptr;
  struct ConfigEndpointLifetime {
    std::uint16_t conn_handle = 0xFFFF;
    std::uint32_t epoch = 0;
  };
  ai_keyboard::EndpointBoundConfigReceiver config_receiver_;
  SemaphoreHandle_t config_receiver_mutex_ = nullptr;
  // Protected by config_receiver_mutex_. A numeric NimBLE connection handle
  // is reusable, so CONFIG assembly is keyed by an epoch allocated for each
  // successful GAP CONNECT lifetime. The counter deliberately survives host
  // generation resets; zero remains the invalid/sentinel epoch.
  std::array<ConfigEndpointLifetime, CONFIG_BT_NIMBLE_MAX_CONNECTIONS>
      config_endpoint_lifetimes_{};
  std::uint32_t config_endpoint_epoch_counter_ = 0;
  std::uint32_t config_endpoint_host_generation_ = 0;
  std::atomic<bool> config_transfer_in_progress_{false};
  std::atomic<std::int64_t> config_transfer_deadline_us_{0};
  std::atomic<std::uint32_t> config_transfer_endpoint_epoch_{0};

  mutable portMUX_TYPE pending_config_mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::string pending_config_json_;
  ai_keyboard::BleOwnerToken pending_config_owner_{};
  bool pending_config_ready_ = false;

  mutable portMUX_TYPE pending_agent_status_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::AgentStatusCommand pending_agent_status_{};
  bool pending_agent_status_ready_ = false;

  mutable portMUX_TYPE status_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::GattStatusSnapshotCache<CONFIG_BT_NIMBLE_MAX_CONNECTIONS,
                                       ai_keyboard::kConfigStatusGattSafeLen>
      status_read_cache_;
  StatusReadCallback status_read_callback_ = nullptr;
  void* status_read_context_ = nullptr;

  mutable portMUX_TYPE management_mux_ = portMUX_INITIALIZER_UNLOCKED;
  ai_keyboard::BleManagementGate management_gate_;
  WorkReadyCallback work_ready_callback_ = nullptr;
  void* work_ready_context_ = nullptr;
  TaskHandle_t owner_task_ = nullptr;

  bool initialized_ = false;
  bool identity_address_ready_ = false;
  bool gatt_schema_change_pending_ = false;
  // GAP/HIDD callbacks publish advertising intent only. The main task owns
  // every advertising stop/fields/response/start sequence, so procedures
  // cannot race across callback tasks.
  std::atomic<bool> directed_reconnect_attempted_{false};
  std::atomic<bool> directed_reconnect_active_{false};
  std::atomic<bool> slow_advertising_{false};

  mutable portMUX_TYPE connection_power_mux_ = portMUX_INITIALIZER_UNLOCKED;
  // Protected by connection_power_mux_; the GAP callback may close the
  // window while the main task is preparing a configuration advertisement.
  bool connected_config_advertising_enabled_ = false;
  std::int64_t connected_config_advertising_deadline_us_ = 0;
  bool stable_connection_parameters_requested_ = false;
  bool connection_update_in_flight_ = false;
  std::int64_t connection_update_retry_after_us_ = 0;
  std::uint8_t connection_update_retry_attempt_ = 0;
  std::uint16_t active_conn_handle_ = 0xFFFF;
  std::uint32_t active_owner_generation_ = 0;
  std::uint16_t control_conn_handle_ = 0xFFFF;
  bool actual_connection_params_valid_ = false;
  std::uint16_t actual_conn_interval_ = 0;
  std::uint16_t actual_conn_latency_ = 0;
  std::uint16_t actual_conn_supervision_timeout_ = 0;
  std::int32_t last_conn_update_status_ = 0;
  // NimBLE reports notification/GATT submission synchronously, not actual
  // over-the-air completion. Accepted wireless work therefore holds the Awake
  // state through a connection-interval-derived grace deadline. This is not a
  // controller-empty or peer-ACK fence. The single deadline is deliberately
  // conservative across HID and control connections.
  std::int64_t ble_tx_grace_deadline_us_ = 0;

  ai_keyboard::HidReportQueue pending_input_reports_;
  ai_keyboard::MouseWheelQueue pending_wheel_reports_;
  ai_keyboard::BleFixedTextStream fixed_text_stream_;
  ai_keyboard::BleInputScheduler input_scheduler_;
  // Queue ownership stays on the main task. NimBLE callbacks only request a
  // deferred reset, avoiding host-task/main-task lock inversion around
  // esp_hidd_dev_input_set().
  std::atomic<bool> input_report_reset_requested_{false};
  // A permanent adapter error invalidates the current owner immediately. Keep
  // producers suppressed until GAP confirms teardown, so no fresh reports can
  // refill the queue behind a permanently unsendable head.
  std::atomic<bool> owner_recovery_pending_{false};
  // Protected by connection_power_mux_. GAP callbacks only advance this pure
  // state machine; all terminate/reset side effects run on the main task.
  ai_keyboard::BleOwnerRecoveryState owner_recovery_{};
  std::atomic<bool> connection_reconcile_requested_{false};
  std::atomic<bool> advertising_reconcile_requested_{false};
  std::atomic<bool> advertising_force_restart_requested_{false};
  // Main-task-only level reconciler. Best-effort HIDD/GAP events are hints;
  // the adapter host generation and ble_gap_adv_active() are authoritative.
  ai_keyboard::BleAdvertisingState advertising_state_{};
  std::int64_t advertising_retry_after_us_ = 0;
  std::uint8_t advertising_retry_attempt_ = 0;
  std::atomic<std::uint32_t> pending_input_report_count_{0};
  std::atomic<std::uint32_t> pending_wheel_report_count_{0};
  std::atomic<std::uint32_t> queued_input_report_count_{0};
  std::atomic<std::uint32_t> transmitted_input_report_count_{0};
  std::atomic<std::uint32_t> dropped_input_report_count_{0};
  std::atomic<std::uint32_t> queued_wheel_report_count_{0};
  std::atomic<std::uint32_t> coalesced_wheel_report_count_{0};
  std::atomic<std::uint32_t> transmitted_wheel_report_count_{0};
  std::atomic<std::uint32_t> dropped_wheel_report_count_{0};
  // Published by the main-task scheduler so status reads from the NimBLE task
  // never touch scheduler-owned, non-atomic state.
  std::atomic<std::uint32_t> retryable_input_report_count_{0};
  std::atomic<std::uint32_t> hid_queue_high_watermark_{0};

  std::uint8_t battery_level_ = 0xFF;
  std::array<std::uint8_t, 6> identity_address_le_{};
};

}  // namespace easy_input
