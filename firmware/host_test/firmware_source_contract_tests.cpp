#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "speaker_assets/speaker_assets_runtime.h"

#ifndef EASY_INPUT_REPO_ROOT
#error "EASY_INPUT_REPO_ROOT must identify the firmware repository"
#endif

namespace {

using easy_input::speaker_assets::SpeakerAssetsRouteToken;
using easy_input::speaker_assets::SpeakerAssetsRuntimeActionExecutor;
using easy_input::speaker_assets::SpeakerAssetsRuntimeCore;
using easy_input::speaker_assets::SpeakerAssetsRuntimeEnqueueResult;
using easy_input::speaker_assets::SpeakerAssetsRuntimeMailboxRecord;
using easy_input::speaker_assets::SpeakerAssetsRuntimeStepResult;

template <typename T, typename = void>
struct has_direct_usb_core_enqueue : std::false_type {};

template <typename T>
struct has_direct_usb_core_enqueue<
    T,
    std::void_t<decltype(std::declval<T&>().enqueue_usb_frame(
        std::declval<const SpeakerAssetsRouteToken&>(),
        std::declval<const std::uint8_t*>(),
        std::declval<std::size_t>(),
        std::declval<std::uint32_t>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_direct_wifi_core_enqueue : std::false_type {};

template <typename T>
struct has_direct_wifi_core_enqueue<
    T,
    std::void_t<decltype(std::declval<T&>().enqueue_wifi_frame(
        std::declval<const SpeakerAssetsRouteToken&>(),
        std::declval<const std::uint8_t*>(),
        std::declval<std::size_t>(),
        std::declval<std::uint32_t>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_result_only_step_comparison : std::false_type {};

template <typename T>
struct has_result_only_step_comparison<
    T,
    std::void_t<decltype(
        std::declval<const T&>() ==
        SpeakerAssetsRuntimeStepResult::Idle)>> : std::true_type {};

template <typename T, typename = void>
struct has_public_step_result_field : std::false_type {};

template <typename T>
struct has_public_step_result_field<
    T,
    std::void_t<decltype(std::declval<const T&>().result)>>
    : std::true_type {};

template <typename T, typename = void>
struct has_public_step_release_field : std::false_type {};

template <typename T>
struct has_public_step_release_field<
    T,
    std::void_t<decltype(std::declval<const T&>().release_now)>>
    : std::true_type {};

static_assert(
    !has_direct_usb_core_enqueue<SpeakerAssetsRuntimeCore>::value,
    "product RuntimeCore must reject direct zero-lease USB ingress");
static_assert(
    !has_direct_wifi_core_enqueue<SpeakerAssetsRuntimeCore>::value,
    "product RuntimeCore must reject direct zero-lease Wi-Fi ingress");
static_assert(
    !has_result_only_step_comparison<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome must not silently decay to result-only usage");
static_assert(
    !has_public_step_result_field<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome result must be inspected with its release lease");
static_assert(
    !has_public_step_release_field<
        SpeakerAssetsRuntimeCore::StepOutcome>::value,
    "product StepOutcome release lease must be inspected with its result");
static_assert(std::is_same_v<
              decltype(std::declval<
                           const SpeakerAssetsRuntimeCore::StepOutcome&>()
                           .inspect(
                               std::declval<
                                   SpeakerAssetsRuntimeStepResult*>(),
                               std::declval<
                                   easy_input::speaker_assets::
                                       SpeakerAssetsLogicalRequestLease*>())),
              bool>);
static_assert(std::is_same_v<
              decltype(std::declval<SpeakerAssetsRuntimeCore&>()
                           .import_mailbox_record(
                               std::declval<
                                   const SpeakerAssetsRuntimeMailboxRecord&>())),
              SpeakerAssetsRuntimeEnqueueResult>);
static_assert(std::is_same_v<
              decltype(std::declval<SpeakerAssetsRuntimeCore&>().step(
                  std::declval<std::uint32_t>(),
                  std::declval<bool>(),
                  std::declval<
                      SpeakerAssetsRuntimeActionExecutor*>())),
              SpeakerAssetsRuntimeCore::StepOutcome>);

std::string read_source(const std::string& relative_path) {
  std::ifstream input(std::string(EASY_INPUT_REPO_ROOT) + "/" + relative_path);
  assert(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string section(const std::string& source,
                    const std::string& begin_marker,
                    const std::string& end_marker) {
  const auto begin = source.find(begin_marker);
  assert(begin != std::string::npos);
  const auto end = source.find(end_marker, begin + begin_marker.size());
  assert(end != std::string::npos);
  return source.substr(begin, end - begin);
}

std::size_t count_occurrences(const std::string& source,
                              const std::string& needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = source.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string without_ascii_whitespace(const std::string& source) {
  std::string compact;
  compact.reserve(source.size());
  for (const char character : source) {
    if (!std::isspace(static_cast<unsigned char>(character))) {
      compact.push_back(character);
    }
  }
  return compact;
}

void battery_update_releases_hidd_lock_before_entering_nimble() {
  const auto source = read_source("components/esp_hid/src/nimble_hidd.c");
  const auto body = section(source,
                            "static int nimble_hidd_dev_battery_set",
                            "/* if mode is NULL");
  const auto nimble_call =
      body.find("const int rc = ble_svc_bas_battery_level_set(level);");
  assert(nimble_call != std::string::npos);

  const auto last_lock = body.rfind("\n    lock_hidd();", nimble_call);
  const auto last_unlock = body.rfind("\n    unlock_hidd();", nimble_call);
  const auto owner_lock = body.rfind("\n    lock_owner_gate();", nimble_call);
  const auto normal_hidd_unlock =
      body.find("\n    unlock_hidd();\n\n    /*", last_lock);
  const auto owner_unlock_after_call =
      body.find("\n        unlock_owner_gate();", nimble_call);
  assert(last_lock != std::string::npos);
  assert(last_unlock != std::string::npos);
  assert(last_unlock > last_lock);
  assert(owner_lock != std::string::npos);
  assert(normal_hidd_unlock != std::string::npos);
  const auto normal_owner_unlock =
      body.find("unlock_owner_gate();", normal_hidd_unlock);
  assert(normal_owner_unlock != std::string::npos);
  assert(normal_owner_unlock > nimble_call);
  assert(owner_unlock_after_call != std::string::npos);
}

void gatt_status_read_callback_is_cache_only_and_refreshes_once() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto access = section(source,
                              "int BleHidTransport::handle_config_access",
                              "void BleHidTransport::note_control_connection");
  const auto read_end = access.find(
      "if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)");
  assert(read_end != std::string::npos);
  const auto read_path = access.substr(0, read_end);

  assert(read_path.find("copy_status_json_for_read") != std::string::npos);
  assert(read_path.find("ctxt->offset == 0") != std::string::npos);
  assert(read_path.find("conn_handle != BLE_HS_CONN_HANDLE_NONE") !=
         std::string::npos);
  assert(read_path.find("current_status_json") == std::string::npos);
  assert(read_path.find("connected()") == std::string::npos);
  assert(read_path.find("connection_identity()") == std::string::npos);
  assert(read_path.find("note_control_connection") == std::string::npos);
  assert(read_path.find("update_battery_level") == std::string::npos);
  assert(read_path.find("ESP_LOGI") == std::string::npos);
}

void stable_connection_reset_has_one_balanced_critical_section() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto reset = section(
      source,
      "void BleHidTransport::reset_connection_state",
      "bool BleHidTransport::connected_config_window_active");
  const auto enter = reset.find("portENTER_CRITICAL(&connection_power_mux_)");
  const auto exit = reset.find("portEXIT_CRITICAL(&connection_power_mux_)");
  assert(enter != std::string::npos);
  assert(exit != std::string::npos);
  assert(reset.find("portENTER_CRITICAL(&connection_power_mux_)", enter + 1) ==
         std::string::npos);
  assert(reset.find("portEXIT_CRITICAL(&connection_power_mux_)", exit + 1) ==
         std::string::npos);
  assert(exit > enter);
  assert(reset.find("stable_connection_parameters_requested_ = false") !=
         std::string::npos);
  assert(source.find("ConnectionPowerProfile") == std::string::npos);
  assert(source.find("connection_profile_name") == std::string::npos);
}

void stable_connection_update_failures_share_bounded_backoff() {
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto request = section(
      source,
      "void BleHidTransport::request_stable_connection_parameters",
      "void BleHidTransport::reconcile_stable_connection_parameters");
  assert(request.find("schedule_connection_update_retry_locked(now_us)") !=
         std::string::npos);
  assert(request.find("params.itvl_min = kStableConnIntervalMin") !=
         std::string::npos);
  assert(request.find("params.itvl_max = kStableConnIntervalMax") !=
         std::string::npos);
  assert(request.find("params.latency = kStableConnLatency") !=
         std::string::npos);
  assert(request.find(
             "params.supervision_timeout = kStableConnSupervisionTimeout") !=
         std::string::npos);

  const auto update_event = section(source,
                                    "case BLE_GAP_EVENT_CONN_UPDATE: {",
                                    "case BLE_GAP_EVENT_CONN_UPDATE_REQ:");
  assert(update_event.find("schedule_connection_update_retry_locked(now_us)") !=
         std::string::npos);
  assert(update_event.find("request_connection_reconcile()") !=
         std::string::npos);
}

void ble_notification_submission_gets_awake_grace_before_deep_sleep() {
  const auto header = read_source("main/platform/ble_hid.h");
  const auto source = read_source("main/platform/ble_hid.cpp");

  assert(header.find("ble_tx_grace_deadline_us_") != std::string::npos);
  assert(header.find("arm_ble_tx_grace(") != std::string::npos);

  const auto pending = section(
      source,
      "bool BleHidTransport::queued_input_delivery_pending() const",
      "bool BleHidTransport::next_work_deadline_us(");
  assert(pending.find("ble_tx_grace_pending(") != std::string::npos);

  const auto deadlines = section(
      source,
      "bool BleHidTransport::next_work_deadline_us(",
      "void BleHidTransport::clear_pending_input_reports(");
  assert(deadlines.find("if (queued_input_delivery_pending())") !=
         std::string::npos);
  assert(deadlines.find("tx_grace_deadline_us") != std::string::npos);
  assert(deadlines.find("add_deadline(static_cast<std::uint64_t>(tx_grace_deadline_us))") !=
         std::string::npos);

  const auto transmit = section(
      source,
      "ai_keyboard::BleInputTxResult BleHidTransport::transmit_scheduled_report(",
      "bool BleHidTransport::read_hidd_lifecycle(");
  const auto submit = transmit.find(
      "easy_input_hidd_dev_input_set_for_owner(");
  const auto accepted = transmit.find("if (err == ESP_OK)", submit);
  const auto arm = transmit.find("arm_ble_tx_grace(", accepted);
  const auto return_accepted = transmit.find(
      "return ai_keyboard::BleInputTxResult::Accepted", arm);
  assert(submit != std::string::npos);
  assert(accepted != std::string::npos);
  assert(arm != std::string::npos);
  assert(return_accepted != std::string::npos);
  assert(submit < accepted);
  assert(accepted < arm);
  assert(arm < return_accepted);

  const auto arm_impl = section(
      source,
      "void BleHidTransport::arm_ble_tx_grace(",
      "bool BleHidTransport::next_work_deadline_us(");
  const auto lookup = arm_impl.find("ble_gap_conn_find(conn_handle, &desc)");
  const auto lock = arm_impl.find(
      "portENTER_CRITICAL(&connection_power_mux_)", lookup);
  const auto owner_recheck = arm_impl.find(
      "active_owner_generation_ == expected_owner_generation", lock);
  const auto window = arm_impl.find("ble_tx_grace_window_us(", owner_recheck);
  assert(lookup != std::string::npos);
  assert(lock != std::string::npos);
  assert(owner_recheck != std::string::npos);
  assert(window != std::string::npos);
  assert(lookup < lock);
  assert(lock < owner_recheck);
  assert(owner_recheck < window);

  // One conservative deadline covers HID and auxiliary GATT connections, so
  // resetting one endpoint must not erase another endpoint's response guard.
  const auto reset = section(
      source,
      "void BleHidTransport::reset_connection_state()",
      "bool BleHidTransport::connected_config_window_active()");
  assert(reset.find("ble_tx_grace_deadline_us_ = 0") ==
         std::string::npos);

  const auto gap = section(
      source,
      "int BleHidTransport::handle_gap_event(",
      "int BleHidTransport::handle_config_access(");
  const auto notify_tx = section(
      gap,
      "case BLE_GAP_EVENT_NOTIFY_TX:",
      "default:");
  assert(notify_tx.find("not a radio/peer") != std::string::npos);
  assert(notify_tx.find("ble_tx_grace_deadline_us_") !=
         std::string::npos);

  const auto gatt = section(
      source,
      "int BleHidTransport::handle_config_access(",
      "void BleHidTransport::note_control_connection(");
  const auto response_ready = gatt.find("const int result = [&]() -> int");
  const auto response_drain =
      gatt.find("arm_ble_tx_grace(conn_handle)", response_ready);
  const auto callback_leave =
      gatt.find("leave_management_callback()", response_drain);
  assert(response_ready != std::string::npos);
  assert(response_drain != std::string::npos);
  assert(callback_leave != std::string::npos);
  assert(response_ready < response_drain);
  assert(response_drain < callback_leave);

  const auto battery = section(
      source,
      "void BleHidTransport::update_battery_level(",
      "void BleHidTransport::set_status_read_callback(");
  assert(battery.find("arm_conservative_ble_tx_grace()") !=
         std::string::npos);
  const auto status_publish = section(
      source,
      "void BleHidTransport::publish_status_json(",
      "bool BleHidTransport::send_firmware_event(");
  const auto changed =
      status_publish.find("ble_gatts_chr_updated(s_config_status_handle)");
  const auto status_grace =
      status_publish.find("arm_conservative_ble_tx_grace()", changed);
  assert(changed != std::string::npos);
  assert(status_grace != std::string::npos);
  assert(changed < status_grace);
}

void speaker_asset_store_priority_ignores_ble_sleep_grace() {
  const auto header = read_source("main/platform/ble_hid.h");
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto app_main = read_source("main/app_main.cpp");

  // Queued application reports and the post-submit radio grace are separate
  // facts. The latter protects only whole-device sleep admission and must not
  // starve bounded local Store work during cold boot.
  assert(header.find("bool input_reports_pending() const;") !=
         std::string::npos);
  assert(header.find("bool input_delivery_pending() const;") !=
         std::string::npos);

  const auto queued = section(
      ble,
      "bool BleHidTransport::input_reports_pending() const",
      "bool BleHidTransport::ble_tx_grace_pending(");
  assert(queued.find("queued_input_delivery_pending()") !=
         std::string::npos);
  assert(queued.find("ble_tx_grace_pending(") == std::string::npos);

  const auto sleep_fence = section(
      ble,
      "bool BleHidTransport::input_delivery_pending() const",
      "void BleHidTransport::arm_ble_tx_grace(");
  assert(sleep_fence.find("input_reports_pending()") !=
         std::string::npos);
  assert(sleep_fence.find("ble_tx_grace_pending(") !=
         std::string::npos);

  const auto sleep_prepare = section(
      ble,
      "bool BleHidTransport::try_begin_deep_sleep_quiesce()",
      "void BleHidTransport::cancel_deep_sleep_quiesce()");
  assert(sleep_prepare.find("input_delivery_pending()") !=
         std::string::npos);
  assert(sleep_prepare.find("input_reports_pending()") ==
         std::string::npos);
  const auto sleep_shutdown = section(
      ble,
      "esp_err_t BleHidTransport::shutdown_for_deep_sleep()",
      "void BleHidTransport::open_config_window(");
  assert(sleep_shutdown.find("input_delivery_pending()") !=
         std::string::npos);
  assert(sleep_shutdown.find("input_reports_pending()") ==
         std::string::npos);

  const auto store_priority = section(
      app_main,
      "bool speaker_asset_resource_steps_allowed(",
      "void flush_input_led_feedback(");
  assert(store_priority.find("app->ble.input_reports_pending()") !=
         std::string::npos);
  assert(store_priority.find("app->ble.input_delivery_pending()") ==
         std::string::npos);

  // The owner must publish the narrow Store gate after it has drained queued
  // input and retried coalesced reports. A false->true transition then grants
  // the waiting Store permit in the same pass instead of depending on a
  // notification that may never arrive.
  const auto owner_loop = section(
      app_main,
      "ESP_LOGI(kTag, \"platform loop started; BLE HID + USB HID + S3C config enabled\")",
      "wait_for_awake_work(app.next_awake_wait)");
  const auto wheel_flush =
      owner_loop.find("flush_pending_wheel_report(&app");
  const auto usb_drain = owner_loop.find("app.usb.poll_pending_reports()");
  const auto ble_drain = owner_loop.find("app.ble.poll_input_delivery(");
  const auto input_retry =
      owner_loop.find("flush_pending_keyboard_snapshot(&app)", ble_drain);
  const auto hotkey_retry =
      owner_loop.find("flush_pending_bridged_hotkey_events(&app)", input_retry);
  const auto store_poll = owner_loop.find("app.speaker_assets.poll(");
  assert(wheel_flush != std::string::npos);
  assert(usb_drain != std::string::npos);
  assert(ble_drain != std::string::npos);
  assert(input_retry != std::string::npos);
  assert(hotkey_retry != std::string::npos);
  assert(store_poll != std::string::npos);
  assert(wheel_flush < store_poll);
  assert(usb_drain < ble_drain);
  assert(ble_drain < input_retry);
  assert(input_retry < hotkey_retry);
  assert(hotkey_retry < store_poll);

  const auto sleep_inputs = section(
      app_main,
      "ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,",
      "// Whole-device sleep admission");
  assert(sleep_inputs.find("app->ble.input_delivery_pending()") !=
         std::string::npos);
  assert(sleep_inputs.find("app->ble.input_reports_pending()") ==
         std::string::npos);
}

void ble_management_publication_and_deep_sleep_are_ordered() {
  const auto header = read_source("main/platform/ble_hid.h");
  const auto source = read_source("main/platform/ble_hid.cpp");

  const char* lifecycle_api[] = {
      "set_work_ready_callback",
      "management_work_pending",
      "next_work_deadline_us",
      "try_begin_deep_sleep_quiesce",
      "cancel_deep_sleep_quiesce",
      "shutdown_for_deep_sleep",
  };
  for (const auto* api : lifecycle_api) {
    assert(header.find(api) != std::string::npos);
  }

  const auto callback_leave = section(
      source,
      "void BleHidTransport::leave_management_callback()",
      "bool BleHidTransport::management_callback_in_flight() const");
  const auto gate_leave = callback_leave.find("management_gate_.leave()");
  const auto gate_unlock = callback_leave.find(
      "portEXIT_CRITICAL(&management_mux_)", gate_leave);
  const auto owner_notify = callback_leave.find("notify_work_ready()", gate_unlock);
  assert(gate_leave != std::string::npos);
  assert(gate_unlock != std::string::npos);
  assert(owner_notify != std::string::npos);
  assert(gate_leave < gate_unlock);
  assert(gate_unlock < owner_notify);

  const auto deadlines = section(
      source,
      "bool BleHidTransport::next_work_deadline_us(",
      "void BleHidTransport::clear_pending_input_reports(");
  assert(deadlines.find("management_owner_work_pending()") !=
         std::string::npos);
  assert(deadlines.find("if (management_work_pending() ||") ==
         std::string::npos);

  const auto agent_publish = section(
      source,
      "bool BleHidTransport::receive_agent_status_report(",
      "void BleHidTransport::queue_completed_config(");
  const auto agent_ready =
      agent_publish.find("pending_agent_status_ready_ = true");
  const auto agent_unlock =
      agent_publish.find("portEXIT_CRITICAL(&pending_agent_status_mux_)");
  const auto agent_notify = agent_publish.find("notify_work_ready()");
  assert(agent_ready != std::string::npos);
  assert(agent_unlock != std::string::npos);
  assert(agent_notify != std::string::npos);
  assert(agent_ready < agent_unlock);
  assert(agent_unlock < agent_notify);

  const auto config_publish = section(
      source,
      "void BleHidTransport::queue_completed_config(",
      "bool BleHidTransport::send_hotkey_report(");
  const auto config_ready = config_publish.find("pending_config_ready_ = true");
  const auto config_unlock =
      config_publish.find("portEXIT_CRITICAL(&pending_config_mux_)");
  const auto config_notify = config_publish.find("notify_work_ready()");
  assert(config_ready != std::string::npos);
  assert(config_unlock != std::string::npos);
  assert(config_notify != std::string::npos);
  assert(config_ready < config_unlock);
  assert(config_unlock < config_notify);

  const auto gatt = section(
      source,
      "int BleHidTransport::handle_config_access(",
      "void BleHidTransport::note_control_connection(");
  const auto gatt_enter = gatt.find("try_enter_management_callback()");
  const auto gatt_body = gatt.find("const int result = [&]() -> int");
  const auto gatt_leave = gatt.find("leave_management_callback()");
  assert(gatt_enter != std::string::npos);
  assert(gatt_body != std::string::npos);
  assert(gatt_leave != std::string::npos);
  assert(gatt_enter < gatt_body);
  assert(gatt_body < gatt_leave);

  const auto hidd_feature = section(
      source,
      "void BleHidTransport::handle_hidd_config_feature(",
      "int BleHidTransport::handle_gap_event(");
  const auto feature_enter =
      hidd_feature.find("try_enter_management_callback()");
  const auto feature_receive =
      hidd_feature.find("receive_config_report(", feature_enter);
  const auto feature_leave =
      hidd_feature.find("leave_management_callback()", feature_receive);
  assert(feature_enter != std::string::npos);
  assert(feature_receive != std::string::npos);
  assert(feature_leave != std::string::npos);
  assert(feature_enter < feature_receive);
  assert(feature_receive < feature_leave);

  const auto hidd_adapter =
      read_source("components/esp_hid/src/nimble_hidd.c");
  const auto report_write_declaration =
      hidd_adapter.find("static void nimble_report_write_cb(");
  const auto report_write_definition = hidd_adapter.find(
      "static void nimble_report_write_cb(",
      report_write_declaration + 1U);
  const auto char_write_definition = hidd_adapter.find(
      "static void nimble_char_write_cb(",
      report_write_definition + 1U);
  assert(report_write_definition != std::string::npos);
  assert(char_write_definition != std::string::npos);
  const auto direct_feature = hidd_adapter.substr(
      report_write_definition,
      char_write_definition - report_write_definition);
  assert(direct_feature.find("direct_feature_handler(") !=
         std::string::npos);
  assert(direct_feature.find("post_hidd_event_bounded(") !=
         std::string::npos);
  assert(direct_feature.find("direct_feature_handler(") <
         direct_feature.find("post_hidd_event_bounded("));

  const auto prepare = section(
      source,
      "bool BleHidTransport::try_begin_deep_sleep_quiesce()",
      "void BleHidTransport::cancel_deep_sleep_quiesce()");
  const auto gate = prepare.find("management_gate_.try_begin_quiesce()");
  const auto recheck = prepare.find("management_work_pending()", gate);
  const auto cancel = prepare.find("cancel_deep_sleep_quiesce()", recheck);
  const auto reset = prepare.find("reset_config_transfer()", cancel);
  assert(gate != std::string::npos);
  assert(reset != std::string::npos);
  assert(recheck != std::string::npos);
  assert(cancel != std::string::npos);
  assert(gate < recheck);
  assert(recheck < cancel);
  assert(cancel < reset);

  const auto shutdown = section(
      source,
      "esp_err_t BleHidTransport::shutdown_for_deep_sleep()",
      "bool BleHidTransport::connected() const");
  const auto owner = shutdown.find("called_from_owner_task()");
  const auto pending = shutdown.find("management_work_pending()", owner);
  const auto terminal = shutdown.find("management_gate_.begin_terminal()", pending);
  const auto event_plane =
      shutdown.find("easy_input_hidd_event_plane_shutdown(hid_dev_)", terminal);
  const auto stop = shutdown.find("nimble_port_stop()", event_plane);
  const auto deinit = shutdown.find("nimble_port_deinit()", stop);
  assert(owner != std::string::npos);
  assert(pending != std::string::npos);
  assert(terminal != std::string::npos);
  assert(event_plane != std::string::npos);
  assert(stop != std::string::npos);
  assert(deinit != std::string::npos);
  assert(owner < pending);
  assert(pending < terminal);
  assert(terminal < event_plane);
  assert(event_plane < stop);
  assert(stop < deinit);
  assert(shutdown.find("esp_hidd_dev_deinit") == std::string::npos);

  const auto event_shutdown = section(
      hidd_adapter,
      "esp_err_t easy_input_hidd_event_plane_shutdown(",
      "esp_err_t easy_input_hidd_owner_terminate(");
  const auto close_posts =
      event_shutdown.find("begin_event_post_shutdown()");
  const auto detach_loop =
      event_shutdown.find("s_dev->event_loop_handle = NULL", close_posts);
  const auto unlock = event_shutdown.find("unlock_hidd()", detach_loop);
  const auto wait_posts =
      event_shutdown.find("wait_for_event_posts(wait_event_posts)", unlock);
  const auto delete_loop =
      event_shutdown.find("esp_event_loop_delete(event_loop)", wait_posts);
  assert(close_posts != std::string::npos);
  assert(detach_loop != std::string::npos);
  assert(unlock != std::string::npos);
  assert(wait_posts != std::string::npos);
  assert(delete_loop != std::string::npos);
  assert(close_posts < detach_loop);
  assert(detach_loop < unlock);
  assert(unlock < wait_posts);
  assert(wait_posts < delete_loop);

  assert(header.find("ai_keyboard::EndpointBoundConfigReceiver") !=
         std::string::npos);
  assert(header.find("config_receiver_mutex_") != std::string::npos);
  assert(header.find("config_transfer_in_progress_") !=
         std::string::npos);
  assert(header.find("config_transfer_deadline_us_") !=
         std::string::npos);
  const auto config_receive = section(
      source,
      "void BleHidTransport::receive_config_report(",
      "bool BleHidTransport::receive_agent_status_report(");
  assert(config_receive.find("xSemaphoreTake(config_receiver_mutex_") !=
         std::string::npos);
  assert(config_receive.find("kConfigTransferIdleTimeoutUs") !=
         std::string::npos);
  assert(config_receive.find("config_receiver_.receive(endpoint_epoch") !=
         std::string::npos);
  assert(config_receive.find("ConfigReceiveStatus::Pending") !=
         std::string::npos);
}

void config_fragments_use_exact_connection_lifetimes() {
  const auto header = read_source("main/platform/ble_hid.h");
  const auto source = read_source("main/platform/ble_hid.cpp");
  const auto hidd_header =
      read_source("components/esp_hid/include/easy_input_esp_hid_owner.h");
  const auto hidd_adapter =
      read_source("components/esp_hid/src/nimble_hidd.c");
  const auto hid_service =
      read_source("components/esp_hid/src/ble_svc_hid.c");
  const auto adapter_cmake =
      read_source("components/esp_hid/CMakeLists.txt");

  // The real writer handle must survive every adapter layer. In particular,
  // a FEATURE written by an auxiliary config connection must never be
  // attributed to the current HID owner.
  const auto public_handler = section(
      hidd_header,
      "typedef void (*easy_input_hidd_feature_write_handler_t)(",
      "/**\n * Return the current BLE HID owner");
  assert(public_handler.find("uint16_t conn_handle") != std::string::npos);

  const auto service_access = section(
      hid_service,
      "ble_svc_hid_access(uint16_t conn_handle",
      "/* can be called multiple times */");
  assert(service_access.find(
             "s_conn_report_write_cb(conn_handle, attr_handle") !=
         std::string::npos);
  assert(hid_service.find(
             "easy_input_ble_svc_hid_register_report_write_cb(") !=
         std::string::npos);

  const auto report_write_declaration =
      hidd_adapter.find("static void nimble_report_write_cb(");
  const auto report_write_definition = hidd_adapter.find(
      "static void nimble_report_write_cb(",
      report_write_declaration + 1U);
  const auto char_write_definition = hidd_adapter.find(
      "static void nimble_char_write_cb(",
      report_write_definition + 1U);
  assert(report_write_definition != std::string::npos);
  assert(char_write_definition != std::string::npos);
  const auto direct_feature = hidd_adapter.substr(
      report_write_definition,
      char_write_definition - report_write_definition);
  assert(direct_feature.find("uint16_t conn_handle") != std::string::npos);
  assert(direct_feature.find(
             "direct_feature_context, conn_handle, data, len") !=
         std::string::npos);
  assert(hidd_adapter.find(
             "easy_input_ble_svc_hid_register_report_write_cb("
             "nimble_report_write_cb)") != std::string::npos);

  const auto application_callback = section(
      source,
      "void hidd_config_feature_callback(",
      "int config_access_callback(");
  assert(application_callback.find("std::uint16_t conn_handle") !=
         std::string::npos);
  assert(application_callback.find(
             "handle_hidd_config_feature(conn_handle, data, len)") !=
         std::string::npos);
  const auto feature_handler = section(
      source,
      "void BleHidTransport::handle_hidd_config_feature(",
      "int BleHidTransport::handle_gap_event(");
  assert(feature_handler.find("owner.conn_handle == conn_handle") !=
         std::string::npos);
  assert(feature_handler.find(
             "receive_config_report(data, len, origin_owner, conn_handle)") !=
         std::string::npos);

  // Epochs are allocated once per successful GAP CONNECT and stored in a
  // fixed table. A reused numeric handle therefore receives a new epoch.
  assert(header.find("struct ConfigEndpointLifetime") != std::string::npos);
  assert(header.find("config_endpoint_lifetimes_") != std::string::npos);
  assert(header.find("config_endpoint_epoch_counter_") != std::string::npos);
  const auto lifetime_begin = section(
      source,
      "void BleHidTransport::begin_config_endpoint_lifetime(",
      "void BleHidTransport::end_config_endpoint_lifetime(");
  assert(lifetime_begin.find("next_config_endpoint_epoch_locked()") !=
         std::string::npos);
  assert(lifetime_begin.find(
             "config_endpoint_host_generation_ != host_generation") !=
         std::string::npos);
  assert(lifetime_begin.find("reset_all_config_endpoint_lifetimes_locked()") !=
         std::string::npos);

  const auto receive = section(
      source,
      "void BleHidTransport::receive_config_report(",
      "bool BleHidTransport::receive_agent_status_report(");
  const auto receiver_lock =
      receive.find("xSemaphoreTake(config_receiver_mutex_, portMAX_DELAY)");
  const auto epoch_lookup =
      receive.find("config_endpoint_epoch_locked(source_conn_handle)");
  const auto receiver_call =
      receive.find("config_receiver_.receive(endpoint_epoch", epoch_lookup);
  assert(receiver_lock != std::string::npos);
  assert(epoch_lookup != std::string::npos);
  assert(receiver_call != std::string::npos);
  assert(receiver_lock < epoch_lookup);
  assert(epoch_lookup < receiver_call);
  assert(receive.find("static_cast<std::uint32_t>(source_conn_handle) + 1U") ==
         std::string::npos);

  const auto gap = section(
      source,
      "int BleHidTransport::handle_gap_event(",
      "int BleHidTransport::handle_config_access(");
  const auto gap_connect = section(
      gap,
      "case BLE_GAP_EVENT_CONNECT:",
      "case BLE_GAP_EVENT_DISCONNECT:");
  const auto gap_disconnect = section(
      gap,
      "case BLE_GAP_EVENT_DISCONNECT:",
      "case BLE_GAP_EVENT_ADV_COMPLETE:");
  assert(gap_connect.find("begin_config_endpoint_lifetime(") !=
         std::string::npos);
  assert(gap_connect.find("host_generation") != std::string::npos);
  assert(gap_disconnect.find("end_config_endpoint_lifetime(") !=
         std::string::npos);

  const auto connection_reset = section(
      source,
      "void BleHidTransport::reset_connection_state()",
      "bool BleHidTransport::connected_config_window_active()");
  const auto connection_unlock = connection_reset.find(
      "portEXIT_CRITICAL(&connection_power_mux_)");
  const auto endpoint_retire = connection_reset.find(
      "end_config_endpoint_lifetime(retired_owner_handle)");
  assert(connection_unlock != std::string::npos);
  assert(endpoint_retire != std::string::npos);
  assert(connection_unlock < endpoint_retire);

  const auto advertising = section(
      source,
      "void BleHidTransport::service_advertising_reconcile()",
      "void BleHidTransport::service_owner_recovery(");
  assert(advertising.find(
             "observe_config_host_generation(host_generation)") !=
         std::string::npos);

  // The second pinned override is fail-closed just like nimble_hidd.c, and
  // CMake removes the upstream service source before adding the local one.
  assert(adapter_cmake.find("expected_upstream_ble_svc_hid_sha256") !=
         std::string::npos);
  assert(adapter_cmake.find("b9c92dce9a0a678ca96c36b2d0784564") !=
         std::string::npos);
  assert(adapter_cmake.find("NOT CMAKE_BUILD_EARLY_EXPANSION") !=
         std::string::npos);
  assert(adapter_cmake.find("NOT CMAKE_SCRIPT_MODE_FILE") !=
         std::string::npos);
  const auto remove_upstream =
      adapter_cmake.find("list(REMOVE_AT bt_component_sources");
  const auto add_adapter = adapter_cmake.find(
      "src/ble_svc_hid.c", remove_upstream);
  assert(remove_upstream != std::string::npos);
  assert(add_adapter != std::string::npos);
  assert(remove_upstream < add_adapter);
}

void ble_status_refresh_publishes_current_config_fingerprint() {
  const auto source = read_source("main/app_main.cpp");
  const auto refresh = section(source,
                               "void process_pending_status_refresh",
                               "void log_heartbeat");
  const auto ble_branch = refresh.substr(refresh.find("if (ble_requested)"));

  assert(refresh.find("config_state.last_applied_json()") != std::string::npos);
  assert(ble_branch.find("publish_config_status(") != std::string::npos);
  assert(ble_branch.find("applied_json.size()") != std::string::npos);
  assert(ble_branch.find("applied_crc") != std::string::npos);
  assert(ble_branch.find("publish_config_status_without_payload") ==
         std::string::npos);
}

void speaker_probe_status_uses_one_generation_for_usb_and_ble() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto refresh = section(app_main,
                               "void process_pending_status_refresh",
                               "void log_heartbeat");
  const auto diagnostic_branch = section(
      refresh,
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)",
      "#else");
  const auto publisher = section(app_main,
                                 "std::string publish_config_status",
                                 "void publish_config_status_for_json");
  const auto finalize = section(speaker_output,
                                "void SpeakerOutput::finalize_probe",
                                "void SpeakerOutput::refresh_probe_metrics_locked");
  const auto begin = section(speaker_output,
                             "esp_err_t SpeakerOutput::begin(",
                             "bool SpeakerOutput::ready() const");
  const auto request = section(
      speaker_output,
      "bool SpeakerOutput::request_diagnostic_tone()",
      "ai_keyboard::SpeakerPlaybackEvents SpeakerOutput::poll(");
  const auto busy_rejection_begin = request.find("if (!ticket.accepted)");
  const auto accepted_reset = request.find("reset_probe_run(ticket.generation)");
  assert(busy_rejection_begin != std::string::npos);
  assert(accepted_reset != std::string::npos);
  assert(busy_rejection_begin < accepted_reset);
  const auto busy_rejection =
      request.substr(busy_rejection_begin,
                     accepted_reset - busy_rejection_begin);

  assert(diagnostic_branch.find(
             "const auto speaker_probe = app->speaker.probe_snapshot();") !=
         std::string::npos);
  assert(diagnostic_branch.find(
             "const auto speaker_status_json = publish_config_status(") !=
         std::string::npos);
  assert(diagnostic_branch.find("\"spk_probe\"") != std::string::npos);
  assert(diagnostic_branch.find("&speaker_probe") != std::string::npos);
  assert(diagnostic_branch.find("speaker_status_json.empty()") !=
         std::string::npos);
  assert(diagnostic_branch.find(
             "speaker_status_json,\n           usb_request_epoch") !=
         std::string::npos);
  assert(diagnostic_branch.find("\"battery\"") == std::string::npos);
  assert(publisher.find("snapshot.speaker = speaker_probe;") !=
         std::string::npos);

  assert(speaker_output.find("portENTER_CRITICAL(&probe_mux_)") !=
         std::string::npos);
  assert(speaker_output.find("portEXIT_CRITICAL(&probe_mux_)") !=
         std::string::npos);
  assert(finalize.find("resolve_speaker_probe_terminal(") !=
         std::string::npos);
  assert(finalize.find("record_probe_terminal(terminal.stage") !=
         std::string::npos);
  assert(begin.find("begin_snapshot.generation = 0;") != std::string::npos);
  assert(begin.find("probe_snapshot_ = begin_snapshot;") != std::string::npos);
  assert(busy_rejection.find("record_probe_state") == std::string::npos);
  assert(busy_rejection.find("record_probe_terminal") == std::string::npos);
  assert(speaker_output.find("finish_opus_probe_metrics(generation)") !=
         std::string::npos);
  assert(speaker_output.find("record_cleanup_failure(generation") !=
         std::string::npos);
  assert(speaker_output.find("observe_speaker_probe_cleanup(") !=
         std::string::npos);
}

void release_build_rejects_internal_ram_profile_drift() {
  const auto cmake = read_source("CMakeLists.txt");
  const auto defaults = read_source("sdkconfig.defaults");

  assert(cmake.find("\"${CMAKE_BINARY_DIR}/sdkconfig\"") !=
         std::string::npos);
  assert(cmake.find("\"${CMAKE_SOURCE_DIR}/sdkconfig.defaults\"") !=
         std::string::npos);
  assert(cmake.find("EasyInput V2 only supports IDF_TARGET=esp32s3") !=
         std::string::npos);
  assert(cmake.find("\"EasyInput production SoC target\"") !=
         std::string::npos);
  assert(cmake.find("CONFIG_IDF_TARGET_ESP32S3") != std::string::npos);
  assert(cmake.find("easy_input_ignored_source_sdkconfig") !=
         std::string::npos);
  assert(cmake.find("The ignored source-tree sdkconfig is not a valid") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE EQUAL 4096") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT EQUAL 12") !=
         std::string::npos);
  assert(cmake.find("CONFIG_BT_NIMBLE_MAX_CONNECTIONS EQUAL 3") !=
         std::string::npos);
  assert(cmake.find("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE") !=
         std::string::npos);
  assert(cmake.find("CONFIG_FREERTOS_ISR_STACKSIZE EQUAL 1536") !=
         std::string::npos);
  assert(cmake.find(
             "CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL EQUAL 12") !=
         std::string::npos);
  assert(cmake.find(
             "CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO EQUAL 400") !=
         std::string::npos);
  assert(cmake.find("CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_MODE_OCT") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_SPEED_80M") !=
         std::string::npos);
  assert(cmake.find("CONFIG_SPIRAM_USE_CAPS_ALLOC") !=
         std::string::npos);
  assert(cmake.find("EasyInput V2 PSRAM memory-domain profile drifted") !=
         std::string::npos);
  assert(cmake.find("Do not reuse an ignored/stale sdkconfig") !=
         std::string::npos);

  assert(defaults.find("CONFIG_IDF_TARGET=\"esp32s3\"") !=
         std::string::npos);
  assert(defaults.find("CONFIG_IDF_TARGET_ESP32S3=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=12") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3") !=
         std::string::npos);
  assert(defaults.find("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_FREERTOS_ISR_STACKSIZE=1536") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL=12") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL=36") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SLAVE_LATENCY=0") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_SVC_GAP_PPCP_SUPERVISION_TMO=400") !=
         std::string::npos);
  assert(defaults.find("CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM=y") != std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_MODE_OCT=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_SPEED_80M=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_USE_CAPS_ALLOC=y") !=
         std::string::npos);
  assert(defaults.find("CONFIG_SPIRAM_IGNORE_NOTFOUND=n") !=
         std::string::npos);
}

void audio_start_reuses_boot_resource_pool() {
  const auto source = read_source("main/platform/keyboard_audio.cpp");
  const auto header = read_source("main/platform/keyboard_audio.h");
  const auto begin = section(source,
                             "esp_err_t KeyboardAudioLink::begin()",
                             "void KeyboardAudioLink::configure");
  const auto start = section(source,
                             "void KeyboardAudioLink::start_stream",
                             "void KeyboardAudioLink::stop_stream");
  const auto stream = section(source,
                              "void KeyboardAudioLink::run_audio_stream",
                              "void KeyboardAudioLink::run_audio_capture");
  const auto capture = section(source,
                               "void KeyboardAudioLink::run_audio_capture",
                               "esp_err_t KeyboardAudioLink::prepare_microphone_channel");
  const auto workers = section(source,
                               "void KeyboardAudioLink::task_entry",
                               "void KeyboardAudioLink::control_task_entry");

  assert(source.find("constexpr std::size_t kAudioCaptureQueueFrames = 64;") !=
         std::string::npos);
  assert(begin.find(
             "heap_caps_malloc(\n"
             "              kAudioCaptureQueueBytes,\n"
             "              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)") !=
         std::string::npos);
  assert(begin.find(
             "heap_caps_calloc(\n"
             "              1U,\n"
             "              sizeof(StaticQueue_t),\n"
             "              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)") !=
         std::string::npos);
  assert(begin.find(
             "xQueueCreateStatic(\n"
             "      kAudioCaptureQueueFrames,\n"
             "      sizeof(CapturedAudioFrame),\n"
             "      capture_queue_storage_,\n"
             "      capture_queue_control_)") !=
         std::string::npos);
  assert(begin.find("esp_psram_is_initialized()") !=
         std::string::npos);
  assert(begin.find("esp_psram_get_size()") !=
         std::string::npos);
  assert(begin.find("aud_psram") != std::string::npos);
  assert(begin.find("vQueueDelete(capture_queue_)") !=
         std::string::npos);
  assert(begin.find("heap_caps_free(capture_queue_storage_)") !=
         std::string::npos);
  assert(begin.find("heap_caps_free(capture_queue_control_)") !=
         std::string::npos);
  assert(begin.find("xQueueCreateWithCaps") == std::string::npos);
  assert(begin.find("vQueueDeleteWithCaps") == std::string::npos);
  assert(begin.find("xQueueCreate(kAudioCaptureQueueFrames") ==
         std::string::npos);
  assert(begin.find("xSemaphoreCreateBinary()") != std::string::npos);
  assert(begin.find("\"mic_udp\"") != std::string::npos);
  assert(begin.find("\"mic_capture\"") != std::string::npos);
  assert(begin.find("aud_psram") != std::string::npos);
  assert(begin.find("aud_pool_sig") != std::string::npos);
  assert(begin.find("aud_pool_tx") != std::string::npos);
  assert(begin.find("aud_pool_cap") != std::string::npos);
  assert(begin.find("aud_ctrl") != std::string::npos);
  assert(begin.find("init_state_ == InitState::Failed") != std::string::npos);
  assert(begin.find("init_state_ = InitState::Failed") != std::string::npos);

  assert(start.find("xTaskCreate") == std::string::npos);
  assert(start.find("new (std::nothrow)") == std::string::npos);
  assert(start.find("stream_job_generation_ = generation") != std::string::npos);
  assert(start.find("xTaskNotifyGive(stream_worker_task_)") != std::string::npos);

  assert(stream.find("xQueueCreate") == std::string::npos);
  assert(stream.find("xTaskCreate") == std::string::npos);
  assert(stream.find("vQueueDelete") == std::string::npos);
  assert(stream.find("xQueueReset(frame_queue)") != std::string::npos);
  assert(stream.find("xTaskNotifyGive(capture_worker_task_)") !=
         std::string::npos);
  assert(stream.find("capture_completed_generation_ == generation") !=
         std::string::npos);
  assert(stream.find("xSemaphoreTake(capture_done_") != std::string::npos);
  assert(capture.find("xSemaphoreGive(capture_done_)") != std::string::npos);
  assert(capture.find("xTaskNotifyGive(stream_task)") == std::string::npos);

  assert(workers.find("for (;;)") != std::string::npos);
  assert(workers.find("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)") !=
         std::string::npos);
  assert(header.find("QueueHandle_t capture_queue_ = nullptr;") !=
         std::string::npos);
  assert(header.find(
             "std::uint8_t* capture_queue_storage_ = nullptr;") !=
         std::string::npos);
  assert(header.find(
             "StaticQueue_t* capture_queue_control_ = nullptr;") !=
         std::string::npos);
  assert(header.find("SemaphoreHandle_t capture_done_ = nullptr;") !=
         std::string::npos);
  assert(header.find("TaskHandle_t stream_worker_task_ = nullptr;") !=
         std::string::npos);
  assert(header.find("TaskHandle_t capture_worker_task_ = nullptr;") !=
         std::string::npos);
  assert(header.find("InitState init_state_ = InitState::Uninitialized;") !=
         std::string::npos);
  assert(header.find("std::uint32_t capture_completed_generation_ = 0;") !=
         std::string::npos);
  assert(source.find("heap_caps_get_largest_free_block") != std::string::npos);
  assert(source.find("static_assert(sizeof(CapturedAudioFrame) == 652)") !=
         std::string::npos);
}

void speaker_probe_is_default_off_and_outside_input_hot_path() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto app_main = read_source("main/app_main.cpp");
  const auto keyboard_audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto input_handler = section(app_main,
        "bool handle_input_event(const easy_input::InputEvent& event, void* context) {",
                                     "void load_stored_config");
  const auto speaker_boot = section(
      app_main,
      "#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)\n"
      "  app.speaker.mark_boot_pending(",
      "#endif\n#if !defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)");

  const auto option = section(root_cmake,
                              "set(\n  EASY_INPUT_SPEAKER_DIAGNOSTIC",
                              "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "set(\n"
             "  EASY_INPUT_SPEAKER_ASSETS_PRODUCT\n"
             "  ON\n") != std::string::npos);
  assert(main_cmake.find("if(EASY_INPUT_SPEAKER_DIAGNOSTIC)") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_sources \"platform/speaker_output.cpp\")") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_DIAGNOSTIC=1") !=
         std::string::npos);
  assert(app_main.find("#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)") !=
         std::string::npos);
  assert(keyboard_audio.find(
             "constexpr i2s_port_t kMicI2sController = I2S_NUM_0;") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr i2s_port_t kSpeakerI2sController = I2S_NUM_1;") !=
         std::string::npos);
  assert(speaker_output.find(
             "ai_keyboard::kSpeakerPlaybackSampleRate") !=
         std::string::npos);
  assert(speaker_output.find(
             "std::array<std::int16_t, 48> kTone1kHz") !=
         std::string::npos);
  assert(speaker_output.find(
             "I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG") !=
         std::string::npos);
  assert(speaker_output.find(
             "standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kSamplesPerFrame == 480U);") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kFadeSamples == 720U);") !=
         std::string::npos);
  assert(speaker_output.find(
             "static_assert(kToneFrames == 60U);") !=
         std::string::npos);
  assert(speaker_output.find("kSampleRate = 24000") == std::string::npos);
  assert(app_main.find(
             "app.audio.set_audio_io_arbiter(&app.audio_io_arbiter);") !=
         std::string::npos);
  assert(keyboard_audio.find("wait_for_microphone_hardware(generation)") !=
         std::string::npos);
  assert(speaker_output.find(
             "audio_io_arbiter_->microphone_requested()") !=
         std::string::npos);
  const auto pending_assignment =
      app_main.find("app.speaker_probe_pending = true;");
  assert(pending_assignment != std::string::npos);
  assert(app_main.find("app.speaker_probe_pending = true;",
                       pending_assignment + 1) == std::string::npos);
  const auto tone_request =
      app_main.find("app->speaker.request_diagnostic_tone()");
  assert(tone_request != std::string::npos);
  assert(app_main.find("app->speaker.request_diagnostic_tone()",
                       tone_request + 1) == std::string::npos);
  assert(speaker_boot.find("wake_cause != ESP_SLEEP_WAKEUP_EXT1") !=
         std::string::npos);
  assert(speaker_boot.find(
             "speaker boot sound skipped after deep-sleep key wake") !=
         std::string::npos);
  assert(speaker_boot.find("external_power_status_active") ==
         std::string::npos);
  assert(speaker_boot.find("usb_vbus_status_present") ==
         std::string::npos);
  assert(speaker_boot.find(".ble.connected()") == std::string::npos);
  assert(speaker_boot.find(".usb.mounted()") == std::string::npos);
  assert(speaker_boot.find("skipped on battery power") ==
         std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
  assert(input_handler.find("play") == std::string::npos);
}

void speaker_opus_probe_is_conditional_fixed_and_reusable() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto main_manifest = read_source("main/idf_component.yml");
  const auto adapter_cmake =
      read_source("diagnostics/speaker_opus_probe/CMakeLists.txt");
  const auto adapter_manifest =
      read_source("diagnostics/speaker_opus_probe/idf_component.yml");
  const auto adapter =
      read_source("diagnostics/speaker_opus_probe/speaker_opus_probe.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto app_main = read_source("main/app_main.cpp");
  const auto fixture = read_source(
      "diagnostics/speaker_opus_probe/assets/easyinput_boot_probe.ogg");
  const auto input_handler = section(
      app_main,
        "bool handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");

  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC",
      "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "NOT EASY_INPUT_SPEAKER_DIAGNOSTIC") != std::string::npos);
  assert(option.find(
             "diagnostics/speaker_opus_probe") != std::string::npos);
  assert(root_cmake.find(
             "\"${CMAKE_BINARY_DIR}/dependencies.opus.lock\"") !=
         std::string::npos);
  assert(root_cmake.find(
             "EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED") !=
         std::string::npos);

  assert(main_manifest.find("esp_audio_codec") == std::string::npos);
  assert(main_cmake.find(
             "idf_build_get_property(\n"
             "  easy_input_speaker_opus_diagnostic_enabled\n"
             "  EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_opus_diagnostic_enabled)") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_requires speaker_opus_probe)") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC=1") !=
         std::string::npos);

  assert(adapter_manifest.find(
             "espressif/esp_audio_codec: \"==2.5.0\"") !=
         std::string::npos);
  assert(adapter_manifest.find("idf: \"==5.5.5\"") !=
         std::string::npos);
  assert(adapter_cmake.find(
             "\"assets/easyinput_boot_probe.ogg\"") !=
         std::string::npos);
  assert(fixture.size() == 1734U);
  assert(fixture.compare(0, 4, "OggS") == 0);
  assert(fixture.find("OpusHead") != std::string::npos);

  assert(adapter.find("esp_opus_dec_register()") != std::string::npos);
  assert(adapter.find("esp_ogg_dec_register()") != std::string::npos);
  assert(adapter.find("esp_audio_dec_register_default") ==
         std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_register_default") ==
         std::string::npos);
  assert(adapter.find("ESP_AUDIO_SIMPLE_DEC_TYPE_OGG") !=
         std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_open") != std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_reset") != std::string::npos);
  assert(adapter.find("esp_audio_simple_dec_process") != std::string::npos);
  assert(adapter.find("_binary_easyinput_boot_probe_ogg_start") !=
         std::string::npos);
  assert(adapter.find("_binary_easyinput_boot_probe_ogg_end") !=
         std::string::npos);
  assert(adapter.find("heap_caps_malloc") != std::string::npos);
  assert(adapter.find("realloc(") == std::string::npos);
  assert(adapter.find(
             "static_assert(kMaximumDecodedFrameBytes == 1920U);") !=
         std::string::npos);

  assert(speaker_output.find("opus_probe_.begin()") != std::string::npos);
  assert(speaker_output.find("opus_probe_.reset()") != std::string::npos);
  assert(speaker_output.find("opus_probe_.decode_next") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr std::uint32_t kWorkerStackBytes = 20U * 1024U;") !=
         std::string::npos);
  assert(speaker_output.find(
             "std::min(kSamplesPerFrame, decoded.sample_count - offset)") !=
         std::string::npos);
  assert(speaker_output.find("cancelled(generation)") !=
         std::string::npos);
  assert(speaker_output.find("heap_caps_get_minimum_free_size") !=
         std::string::npos);
  assert(speaker_output.find("uxTaskGetStackHighWaterMark") !=
         std::string::npos);
  assert(speaker_output.find("maximum_decode_call_us_") !=
         std::string::npos);
  const auto play_tone = section(
      speaker_output,
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(",
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)\n"
      "esp_err_t SpeakerOutput::prepare_opus_first_frame");
  const auto first_decode =
      play_tone.find("prepare_opus_first_frame(generation, &first_opus_frame)");
  const auto clocks_on = play_tone.find("i2s_channel_enable(tx_channel_)");
  assert(first_decode != std::string::npos);
  assert(clocks_on != std::string::npos);
  assert(first_decode < clocks_on);
  assert(input_handler.find("opus") == std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
}

void speaker_ima_probe_is_conditional_allocation_free_and_isolated() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto adapter_cmake =
      read_source("diagnostics/speaker_ima_adpcm_probe/CMakeLists.txt");
  const auto decoder_header = read_source(
      "diagnostics/speaker_ima_adpcm_probe/include/ima_adpcm_decoder.h");
  const auto decoder = read_source(
      "diagnostics/speaker_ima_adpcm_probe/ima_adpcm_decoder.cpp");
  const auto adapter = read_source(
      "diagnostics/speaker_ima_adpcm_probe/speaker_ima_adpcm_probe.cpp");
  const auto asset = read_source(
      "diagnostics/speaker_ima_adpcm_probe/assets/"
      "easyinput_boot_probe_eiad.h");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto audio_contract = read_source(
      "components/keyboard/include/keyboard/speaker_audio_contract.h");
  const auto app_main = read_source("main/app_main.cpp");
  const auto input_handler = section(
      app_main,
        "bool handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");

  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC",
      "# The production PCB");
  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find("NOT EASY_INPUT_SPEAKER_DIAGNOSTIC") !=
         std::string::npos);
  assert(option.find("mutually exclusive") != std::string::npos);
  assert(option.find("diagnostics/speaker_ima_adpcm_probe") !=
         std::string::npos);
  assert(root_cmake.find(
             "EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "idf_build_get_property(\n"
             "  easy_input_speaker_ima_adpcm_diagnostic_enabled\n"
             "  EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC_ENABLED") !=
         std::string::npos);
  assert(main_cmake.find(
             "list(APPEND easy_input_main_requires "
             "speaker_ima_adpcm_probe)") != std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC=1") !=
         std::string::npos);

  assert(adapter_cmake.find("idf_component_register") != std::string::npos);
  assert(adapter_cmake.find("esp_audio_codec") == std::string::npos);
  assert(adapter.find("esp_audio") == std::string::npos);
  assert(adapter.find("Opus") == std::string::npos);
  assert(decoder.find("malloc") == std::string::npos);
  assert(decoder.find("calloc") == std::string::npos);
  assert(decoder.find("realloc") == std::string::npos);
  assert(decoder.find("new ") == std::string::npos);
  assert(decoder.find("std::vector") == std::string::npos);
  assert(decoder_header.find(
             "kImaAdpcmAssetSampleRate = 48000") != std::string::npos);
  assert(decoder_header.find(
             "kImaAdpcmFrameSamples = 480") != std::string::npos);
  assert(decoder_header.find(
             "static_assert(sizeof(ImaAdpcmDecoder) <= 48U)") !=
         std::string::npos);
  assert(asset.find("decode_base64<6872U>") != std::string::npos);
  assert(asset.find(
             "static_assert(kEasyInputBootProbeEiad.size() == 6872U)") !=
         std::string::npos);

  assert(speaker_output.find("ima_probe_.begin()") != std::string::npos);
  assert(speaker_output.find("ima_probe_.reset()") != std::string::npos);
  assert(speaker_output.find("ima_probe_.decode_next") !=
         std::string::npos);
  assert(speaker_output.find(
             "constexpr std::uint32_t kWorkerStackBytes = 4096;") !=
         std::string::npos);
  assert(audio_contract.find(
             "kSpeakerPlaybackPreloadZeroFrames = 1U") !=
         std::string::npos);
  assert(audio_contract.find(
             "speaker_normal_drain_zero_frames(") !=
         std::string::npos);
  assert(audio_contract.find(
             "speaker_first_pcm_queue_upper_bound_us(") !=
         std::string::npos);
  const auto preload = section(
      speaker_output,
      "esp_err_t SpeakerOutput::preload_zero_dma(",
      "esp_err_t SpeakerOutput::write_samples(");
  const auto preload_call =
      preload.find("i2s_channel_preload_data(tx_channel_,");
  assert(preload_call != std::string::npos);
  assert(preload.find("i2s_channel_preload_data(tx_channel_,",
                      preload_call + 1U) == std::string::npos);
  assert(preload.find("bytes_loaded == expected_bytes") !=
         std::string::npos);
  const auto play_tone = section(
      speaker_output,
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(",
      "#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)\n"
      "esp_err_t SpeakerOutput::prepare_opus_first_frame");
  const auto first_decode =
      play_tone.find("prepare_ima_first_frame(generation,");
  const auto clocks_on = play_tone.find("i2s_channel_enable(tx_channel_)");
  assert(first_decode != std::string::npos);
  assert(clocks_on != std::string::npos);
  assert(first_decode < clocks_on);
  assert(play_tone.find(
             "play_ima_frames(generation, frame.data(), frame.size())") !=
         std::string::npos);
  assert(play_tone.find(
             "write_samples(frame.data(), frame.size())") !=
         std::string::npos);
  const auto cancellation_snapshot =
      play_tone.find("bool cancellation_seen = cancelled(generation);");
  const auto normal_drain =
      play_tone.find("index < kNormalDrainZeroFrames");
  const auto stop_clock =
      play_tone.find("i2s_channel_disable(tx_channel_)", normal_drain);
  assert(cancellation_snapshot != std::string::npos);
  assert(normal_drain != std::string::npos);
  assert(stop_clock != std::string::npos);
  assert(cancellation_snapshot < normal_drain);
  assert(normal_drain < stop_clock);
  assert(play_tone.find(
             "if (err == ESP_OK && !cancellation_seen)") !=
         std::string::npos);
  assert(speaker_output.find(
             "conservative_first_pcm_latency_us(requested)") !=
         std::string::npos);
  const auto prepare_ima = section(
      speaker_output,
      "esp_err_t SpeakerOutput::prepare_ima_first_frame(",
      "esp_err_t SpeakerOutput::play_ima_frames(");
  assert(prepare_ima.find(
             "std::fill(output + *output_samples, "
             "output + output_capacity, 0)") !=
         std::string::npos);
  const auto stream_ima = section(
      speaker_output,
      "esp_err_t SpeakerOutput::play_ima_frames(",
      "esp_err_t SpeakerOutput::finish_ima_probe_metrics(");
  assert(stream_ima.find(
             "std::fill(frame + decoded_samples, "
             "frame + frame_capacity, 0)") !=
         std::string::npos);
  assert(stream_ima.find(
             "write_samples(frame, frame_capacity)") !=
         std::string::npos);
  const auto ima_begin_failure = section(
      speaker_output,
      "err = ima_probe_.begin();",
      "#endif\n\n  supervisor_task_");
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeStage::DecodeReset") !=
         std::string::npos);
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeError::DecodeReset") !=
         std::string::npos);
  assert(ima_begin_failure.find(
             "ai_keyboard::SpeakerProbeError::InvalidArgument") ==
         std::string::npos);
  assert(app_main.find(
             "\"0.4.40-idf-v2-spk-ima-probe\"") != std::string::npos);
  assert(input_handler.find("ima") == std::string::npos);
  assert(input_handler.find("speaker") == std::string::npos);
}

void speaker_asset_storage_reserves_exact_banks_and_has_production_gate() {
  const auto root_cmake = read_source("CMakeLists.txt");
  const auto partitions = read_source("partitions.csv");
  const auto main_cmake = read_source("main/CMakeLists.txt");
  const auto app_main = read_source("main/app_main.cpp");
  const auto assets_link_anchor =
      read_source("features/speaker_assets/diagnostic_link_anchor.cpp");
  const auto nvs_store = read_source("main/platform/nvs_store.cpp");
  const auto config_state =
      read_source("components/keyboard/src/config_state.cpp");
  const auto input_handler = section(
      app_main,
        "bool handle_input_event(const easy_input::InputEvent& event, void* context) {",
      "void load_stored_config");
  const auto option = section(
      root_cmake,
      "set(\n  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC",
      "# The production PCB");
  const auto app_main_entry = section(
      app_main,
      "extern \"C\" void app_main(void) {",
      "  static AppContext app;");

  assert(option.find("\n  OFF\n") != std::string::npos);
  assert(option.find(
             "if(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC OR\n"
             "   EASY_INPUT_SPEAKER_ASSETS_PRODUCT)\n"
             "  list(APPEND EXTRA_COMPONENT_DIRS\n"
             "    \"${CMAKE_SOURCE_DIR}/features/speaker_assets\")\n"
             "endif()") != std::string::npos);
  const auto assets_component =
      read_source("features/speaker_assets/CMakeLists.txt");
  assert(assets_component.find("WHOLE_ARCHIVE") !=
         std::string::npos);
  assert(assets_component.find("diagnostic_link_anchor.cpp") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "#include \"speaker_assets/speaker_assets_flash_runner.h\"") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "SpeakerAssetsCooperativeStoreRunner runner(") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "runner.publish_priority_allowed(true);") !=
         std::string::npos);
  assert(assets_link_anchor.find(
             "runner.step(emission.action, &completion)") !=
         std::string::npos);
  assert(assets_link_anchor.find("runner.worker_run_once()") !=
         std::string::npos);
  assert(root_cmake.find(
             "idf_build_set_property(\n"
             "  EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC_ENABLED\n"
             "  \"${EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC}\"\n"
             ")") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_assets_diagnostic_enabled)\n"
             "  list(APPEND easy_input_main_requires speaker_assets)\n"
             "endif()") != std::string::npos);
  assert(main_cmake.find(
             "\"platform/speaker_assets_supervisor.cpp\"") !=
         std::string::npos);
  assert(main_cmake.find(
             "PRIVATE EASY_INPUT_SPEAKER_ASSETS_PRODUCT=1") !=
         std::string::npos);
  assert(main_cmake.find(
             "if(easy_input_speaker_assets_diagnostic_enabled)\n"
             "  target_compile_definitions(\n"
             "    ${COMPONENT_LIB}\n"
             "    PRIVATE EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC=1\n"
             "  )\n"
             "endif()") != std::string::npos);
  assert(app_main_entry.find(
             "#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)\n"
             "  // Link-only diagnostic:") != std::string::npos);
  assert(app_main_entry.find(
             "const void* volatile speaker_assets_link_anchor =\n"
             "      easy_input_speaker_assets_diagnostic_link_anchor();") !=
         std::string::npos);
  assert(app_main_entry.find(
             "static_cast<void>(speaker_assets_link_anchor);\n"
             "#endif") !=
         std::string::npos);
  assert(root_cmake.find("easy_input_expected_partition_entries") !=
         std::string::npos);
  assert(root_cmake.find(
             "\"sound_a,0x40,0x00,0x310000,0x90000,\"") !=
         std::string::npos);
  assert(root_cmake.find(
             "\"sound_b,0x40,0x01,0x3a0000,0x90000,\"") !=
         std::string::npos);

  const auto nvs = partitions.find(
      "nvs,      data, nvs,     0x9000,   0x6000,");
  const auto phy = partitions.find(
      "phy_init, data, phy,     0xf000,   0x1000,");
  const auto factory = partitions.find(
      "factory,  app,  factory, 0x10000,  0x300000,");
  const auto sound_a = partitions.find(
      "sound_a,  0x40, 0x00,    0x310000, 0x90000,");
  const auto sound_b = partitions.find(
      "sound_b,  0x40, 0x01,    0x3A0000, 0x90000,");
  assert(nvs != std::string::npos);
  assert(phy != std::string::npos);
  assert(factory != std::string::npos);
  assert(sound_a != std::string::npos);
  assert(sound_b != std::string::npos);
  assert(nvs < phy);
  assert(phy < factory);
  assert(factory < sound_a);
  assert(sound_a < sound_b);

  assert(main_cmake.find("sound_asset_store") == std::string::npos);
  assert(app_main.find(
             "app->speaker_assets.begin_local(\n"
             "              &app->usb, app->platform_task)") !=
         std::string::npos);
  assert(input_handler.find("sound_asset") == std::string::npos);
  assert(input_handler.find("sound_bank") == std::string::npos);
  assert(nvs_store.find("sound_asset") == std::string::npos);
  assert(nvs_store.find("sound_bank") == std::string::npos);
  assert(config_state.find("sound_asset") == std::string::npos);
  assert(config_state.find("sound_bank") == std::string::npos);
}

void speaker_asset_usb_lifetime_precedes_first_frame() {
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto callback_path = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::enqueue_usb_frame(",
      "void SpeakerAssetsSupervisor::store_task_entry");
  const auto ensure_position = callback_path.find(
      "ensure_mailbox_usb_route_locked(endpoint_epoch)");
  const auto frame_position = callback_path.find(
      "mailbox_.enqueue_usb_frame(");
  assert(ensure_position != std::string::npos);
  assert(frame_position != std::string::npos);
  assert(ensure_position < frame_position);

  const auto route_publication = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::ensure_mailbox_usb_route_locked(",
      "void SpeakerAssetsSupervisor::observe_usb_route");
  const auto close_position = route_publication.find(
      "mailbox_.enqueue_route_closed(");
  const auto open_position = route_publication.find(
      "mailbox_.enqueue_route_opened(");
  assert(close_position != std::string::npos);
  assert(open_position != std::string::npos);
  assert(close_position < open_position);
}

void speaker_asset_wifi_carrier_is_the_only_wireless_bulk_path() {
  const auto ble_header = read_source("main/platform/ble_hid.h");
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto protocol_header = read_source(
      "features/speaker_assets/include/speaker_assets/"
      "speaker_assets_protocol.h");
  const auto protocol = read_source(
      "features/speaker_assets/speaker_assets_protocol.cpp");
  const auto runtime_header = read_source(
      "features/speaker_assets/include/speaker_assets/"
      "speaker_assets_runtime.h");
  const auto runtime = read_source(
      "features/speaker_assets/speaker_assets_runtime.cpp");
  const auto carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");

  // Bluetooth remains the keyboard's HID/config transport, but it must not
  // expose or route the retired speaker bulk service.
  assert(ble_header.find("SpeakerAssets") == std::string::npos);
  assert(ble.find("SpeakerAssets") == std::string::npos);

  assert(supervisor_header.find("SpeakerAssetsWifiCarrier wifi_;") !=
         std::string::npos);
  assert(supervisor.find("enqueue_wifi_frame(") !=
         std::string::npos);
  assert(supervisor.find("retire_accepted_wifi_reply()") !=
         std::string::npos);
  assert(supervisor.find("offer_wifi_reply()") !=
         std::string::npos);
  assert(protocol.find("encode_speaker_assets_wifi_frame(") !=
         std::string::npos);
  assert(runtime_header.find("enqueue_wifi_frame(") !=
         std::string::npos);
  assert(runtime.find("SpeakerAssetsTransport::Wifi") !=
         std::string::npos);
  assert(protocol_header.find("Wifi = 3U") != std::string::npos);
  assert(carrier.find("TCP_NODELAY") != std::string::npos);
  assert(
      carrier.find(
          "setsockopt(\n"
          "            client,\n"
          "            IPPROTO_TCP,\n"
          "            TCP_NODELAY") != std::string::npos);

  const auto peer_probe = section(
      carrier,
      "bool peer_socket_open_non_consuming(int socket)",
      "}  // namespace");
  assert(peer_probe.find("MSG_PEEK | MSG_DONTWAIT") !=
         std::string::npos);
  assert(peer_probe.find(
             "classify_speaker_assets_wifi_peer_probe(") !=
         std::string::npos);

  const auto response_wait = section(
      carrier,
      "PendingResponse response{};",
      "if (!keep_running || !response.ready)");
  const auto probe_position = response_wait.find(
      "peer_socket_open_non_consuming(socket)");
  const auto response_position = response_wait.find(
      "take_pending_response(route, &response)");
  assert(probe_position != std::string::npos);
  assert(response_position != std::string::npos);
  assert(probe_position < response_position);

  const auto activate_route = section(
      carrier,
      "void SpeakerAssetsWifiCarrier::set_active_route(",
      "void SpeakerAssetsWifiCarrier::clear_active_route(");
  const auto publish_busy =
      activate_route.find("state_.route_active = true;");
  const auto unlock_route = activate_route.find("unlock();");
  const auto refresh_busy = activate_route.find(
      "audio_->request_heartbeat_refresh();");
  assert(publish_busy != std::string::npos);
  assert(unlock_route != std::string::npos);
  assert(refresh_busy != std::string::npos);
  assert(publish_busy < unlock_route);
  assert(unlock_route < refresh_busy);

  const auto cleanup = section(
      carrier,
      "clear_pending_response(route);",
      "speaker_assets::SpeakerAssetsWifiPolicyInputs");
  const auto clear_pending =
      cleanup.find("clear_pending_response(route);");
  const auto clear_route =
      cleanup.find("clear_active_route(route);");
  const auto close_route =
      cleanup.find("observe_route_(callback_context_, route, false)");
  const auto release_service =
      cleanup.find("release_wifi_service_lease(service_lease)");
  assert(clear_pending != std::string::npos);
  assert(clear_route != std::string::npos);
  assert(close_route != std::string::npos);
  assert(release_service != std::string::npos);
  assert(clear_pending < clear_route);
  assert(clear_route < close_route);
  assert(close_route < release_service);
}

void speaker_asset_endpoint_accept_retires_before_lifetime_unlock() {
  const auto usb_header =
      read_source("main/platform/usb_hid.h");
  const auto usb = read_source("main/platform/usb_hid.cpp");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");

  assert(usb_header.find(
             "using SpeakerAssetsResponseAcceptedCallback = bool (*)(") !=
         std::string::npos);
  assert(usb_header.find(
             "std::uint32_t endpoint_epoch,\n"
             "      std::uint32_t runtime_reply_sequence);") !=
         std::string::npos);

  const auto send_path = section(
      usb,
      "UsbHidTransport::try_send_speaker_assets_response()",
      "bool UsbHidTransport::status_response_pending()");
  const auto endpoint_accept =
      send_path.find("const bool accepted = tud_hid_report(");
  const auto synchronous_retirement =
      send_path.find("speaker_assets_response_accepted_callback_(");
  const auto response_reset =
      send_path.find("reset_speaker_assets_response();",
                     synchronous_retirement);
  const auto lifetime_unlock =
      send_path.find("unlock_lifetime();", response_reset);
  assert(endpoint_accept != std::string::npos);
  assert(synchronous_retirement != std::string::npos);
  assert(response_reset != std::string::npos);
  assert(lifetime_unlock != std::string::npos);
  assert(endpoint_accept < synchronous_retirement);
  assert(synchronous_retirement < response_reset);
  assert(response_reset < lifetime_unlock);
  assert(send_path.find(
             "if (!retired_synchronously) {\n"
             "    // Preserve the owner-task polling path") !=
         std::string::npos);
  assert(send_path.find(
             "speaker_assets_sent_sequence_ = sent_sequence;") !=
         std::string::npos);
  assert(send_path.find(
             "speaker_assets_sent_epoch_ = sent_epoch;") !=
         std::string::npos);

  const auto receive_path = section(
      usb,
      "void UsbHidTransport::receive_speaker_assets_report(",
      "void UsbHidTransport::queue_completed_config(");
  const auto request_lifetime_lock =
      receive_path.find("lock_current_epoch(endpoint_epoch)");
  const auto request_callback =
      receive_path.find("speaker_assets_frame_callback_(");
  const auto request_lifetime_unlock =
      receive_path.find("unlock_lifetime();", request_callback);
  assert(request_lifetime_lock != std::string::npos);
  assert(request_callback != std::string::npos);
  assert(request_lifetime_unlock != std::string::npos);
  assert(request_lifetime_lock < request_callback);
  assert(request_callback < request_lifetime_unlock);

  assert(supervisor.find(
             "set_speaker_assets_response_accepted_callback(\n"
             "      &SpeakerAssetsSupervisor::"
             "retire_usb_reply_on_endpoint_accept,") !=
         std::string::npos);
  const auto retirement = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::retire_accepted_usb_reply(",
      "void SpeakerAssetsSupervisor::retire_physically_sent_reply()");
  assert(retirement.find(
             "queued_reply_sequence_ != runtime_reply_sequence") !=
         std::string::npos);
  assert(retirement.find(
             "queued_reply_lease_.route.generation != endpoint_epoch") !=
         std::string::npos);
  assert(retirement.find(
             "core_.remove_reply_if_sequence(runtime_reply_sequence)") !=
         std::string::npos);
  assert(retirement.find("release_mailbox_lease(lease)") !=
         std::string::npos);
  assert(retirement.find("queued_reply_sequence_ = 0U;") !=
         std::string::npos);
  assert(retirement.find("queued_reply_lease_ = {};") !=
         std::string::npos);
  assert(retirement.find("usb_->") == std::string::npos);

  const auto fallback = section(
      supervisor,
      "void SpeakerAssetsSupervisor::retire_physically_sent_reply()",
      "void SpeakerAssetsSupervisor::offer_usb_reply()");
  assert(fallback.find(
             "take_speaker_assets_response_sent(\n"
             "          &sent_sequence, &sent_epoch)") !=
         std::string::npos);
  assert(fallback.find(
             "retire_accepted_usb_reply(sent_epoch, sent_sequence)") !=
         std::string::npos);
}

void power_telemetry_reports_awake_and_real_deep_sleep_facts_only() {
  const auto cycle_header =
      read_source("components/keyboard/include/keyboard/power_cycle.h");
  const auto cycle =
      read_source("components/keyboard/src/power_cycle.cpp");
  const auto status_header =
      read_source("components/keyboard/include/keyboard/config_status.h");
  const auto status =
      read_source("components/keyboard/src/config_status.cpp");

  assert(cycle_header.find("inactive_ms") != std::string::npos);
  assert(cycle_header.find("reached_deep_sleep") != std::string::npos);
  assert(cycle_header.find("LightSleepKey") == std::string::npos);
  assert(cycle_header.find("PowerCycleStage") == std::string::npos);
  assert(cycle_header.find("deep_idle_ms") == std::string::npos);
  assert(cycle.find("light_sleep_key_wake") == std::string::npos);

  assert(status_header.find("std::uint32_t inactive_ms") !=
         std::string::npos);
  assert(status_header.find("std::string deep_sleep_block") !=
         std::string::npos);
  assert(status_header.find("cycle_inactive_ms") != std::string::npos);
  assert(status_header.find("cycle_deep_sleep") != std::string::npos);
  const char* retired_members[] = {
      "poll_ms",       "deep_entries", "deep_ms",
      "last_enter_ms", "last_exit_ms", "cycle_idle_ms",
      "cycle_deep_ms", "cycle_flags",
  };
  for (const auto* retired : retired_members) {
    assert(status_header.find(retired) == std::string::npos);
  }

  assert(status.find("\"mode\", \"awake\"") != std::string::npos);
  assert(status.find("\"inactive_ms\"") != std::string::npos);
  assert(status.find("\"deep_sleep_block\"") != std::string::npos);
  assert(status.find("\"cycle_inactive_ms\"") != std::string::npos);
  assert(status.find("\"cycle_deep_sleep\"") != std::string::npos);
  const char* retired_wire_fields[] = {
      "\"poll_ms\"",       "\"deep_entries\"", "\"deep_ms\"",
      "\"last_enter_ms\"", "\"last_exit_ms\"", "\"cycle_idle_ms\"",
      "\"cycle_deep_ms\"", "\"cycle_flags\"",
  };
  for (const auto* retired : retired_wire_fields) {
    assert(status.find(retired) == std::string::npos);
  }
}

void usb_async_work_is_durable_before_owner_notification() {
  const auto usb_header = read_source("main/platform/usb_hid.h");
  const auto usb = read_source("main/platform/usb_hid.cpp");

  assert(usb_header.find("#include \"keyboard/awake_wait_planner.h\"") !=
         std::string::npos);
  assert(usb_header.find("using WorkReadyCallback = void (*)(void* context);") !=
         std::string::npos);
  assert(usb_header.find(
             "void set_work_ready_callback(WorkReadyCallback callback,") !=
         std::string::npos);
  assert(usb_header.find(
             "ai_keyboard::OwnerServiceSchedule work_schedule(") !=
         std::string::npos);
  assert(usb_header.find("void on_hid_report_complete();") !=
         std::string::npos);
  assert(usb_header.find("void on_hid_report_failed();") !=
         std::string::npos);
  assert(usb_header.find("void on_tinyusb_resume();") !=
         std::string::npos);

  const auto mount = section(
      usb,
      "void UsbHidTransport::on_tinyusb_mount()",
      "void UsbHidTransport::on_tinyusb_unmount()");
  const auto mount_publish = mount.find("lifetime_reset_pending_ = true;");
  const auto mount_unlock = mount.find("xSemaphoreGive(lifetime_mutex_);");
  const auto mount_notify = mount.find("notify_work_ready();");
  assert(mount_publish != std::string::npos);
  assert(mount_unlock != std::string::npos);
  assert(mount_notify != std::string::npos);
  assert(mount_publish < mount_unlock);
  assert(mount_unlock < mount_notify);

  const auto unmount = section(
      usb,
      "void UsbHidTransport::on_tinyusb_unmount()",
      "void UsbHidTransport::observe_physical_presence(bool present)");
  const auto unmount_publish =
      unmount.find("lifetime_reset_pending_ = true;");
  const auto unmount_unlock =
      unmount.find("xSemaphoreGive(lifetime_mutex_);");
  const auto unmount_notify = unmount.find("notify_work_ready();");
  assert(unmount_publish != std::string::npos);
  assert(unmount_unlock != std::string::npos);
  assert(unmount_notify != std::string::npos);
  assert(unmount_publish < unmount_unlock);
  assert(unmount_unlock < unmount_notify);

  const auto physical_presence = section(
      usb,
      "void UsbHidTransport::observe_physical_presence(bool present)",
      "bool UsbHidTransport::lock_current_epoch(");
  const auto physical_publish =
      physical_presence.find("lifetime_reset_pending_ = true;");
  const auto physical_unlock =
      physical_presence.find("xSemaphoreGive(lifetime_mutex_);");
  const auto physical_notify =
      physical_presence.find("notify_work_ready();");
  assert(physical_publish != std::string::npos);
  assert(physical_unlock != std::string::npos);
  assert(physical_notify != std::string::npos);
  assert(physical_publish < physical_unlock);
  assert(physical_unlock < physical_notify);

  const auto config = section(
      usb,
      "void UsbHidTransport::receive_config_report(",
      "void UsbHidTransport::receive_agent_status_report(");
  const auto config_publish = config.find("queue_completed_config(");
  const auto config_unlock = config.find("unlock_lifetime();");
  const auto config_notify = config.find("notify_work_ready();");
  assert(config_publish != std::string::npos);
  assert(config_unlock != std::string::npos);
  assert(config_notify != std::string::npos);
  assert(config_publish < config_unlock);
  assert(config_unlock < config_notify);

  const auto agent = section(
      usb,
      "void UsbHidTransport::receive_agent_status_report(",
      "void UsbHidTransport::receive_status_request_report(");
  const auto agent_publish = agent.find("pending_agent_status_ready_ = true;");
  const auto agent_unlock = agent.find("unlock_lifetime();", agent_publish);
  const auto agent_notify = agent.find("notify_work_ready();");
  assert(agent_publish != std::string::npos);
  assert(agent_unlock != std::string::npos);
  assert(agent_notify != std::string::npos);
  assert(agent_publish < agent_unlock);
  assert(agent_unlock < agent_notify);

  const auto status = section(
      usb,
      "void UsbHidTransport::receive_status_request_report(",
      "void UsbHidTransport::receive_speaker_assets_report(");
  const auto status_publish =
      status.find("pending_status_request_ready_ = true;");
  const auto status_unlock = status.find("unlock_lifetime();");
  const auto status_notify = status.find("notify_work_ready();");
  assert(status_publish != std::string::npos);
  assert(status_unlock != std::string::npos);
  assert(status_notify != std::string::npos);
  assert(status_publish < status_unlock);
  assert(status_unlock < status_notify);

  const auto schedule = section(
      usb,
      "ai_keyboard::OwnerServiceSchedule UsbHidTransport::work_schedule(",
      "void UsbHidTransport::on_hid_report_complete()");
  assert(schedule.find("schedule.deadline_armed = false;") !=
         std::string::npos);
  assert(schedule.find("report_in_flight_.load(") != std::string::npos);
  assert(schedule.find("schedule.outstanding =") != std::string::npos);
  assert(schedule.find("schedule.runnable_now =") != std::string::npos);
  assert(schedule.find("!report_in_flight") != std::string::npos);
  assert(schedule.find("tud_hid_ready()") != std::string::npos);

  const auto report_complete = section(
      usb,
      "void UsbHidTransport::on_hid_report_complete()",
      "void UsbHidTransport::on_hid_report_failed()");
  assert(report_complete.find(
             "report_in_flight_.store(false, std::memory_order_release);") <
         report_complete.find("notify_work_ready();"));
  const auto report_failed = section(
      usb,
      "void UsbHidTransport::on_hid_report_failed()",
      "void UsbHidTransport::observe_physical_presence(bool present)");
  assert(report_failed.find(
             "report_in_flight_.store(false, std::memory_order_release);") <
         report_failed.find("notify_work_ready();"));
  assert(count_occurrences(
             usb,
             "report_in_flight_.store(true, std::memory_order_release);") ==
         5);

  assert(usb.find("extern \"C\" void tud_hid_report_complete_cb(") !=
         std::string::npos);
  assert(usb.find("extern \"C\" void tud_hid_report_failed_cb(") !=
         std::string::npos);
  assert(usb.find("extern \"C\" void tud_resume_cb(void)") !=
         std::string::npos);
}

void speaker_asset_boot_read_uses_the_protocol_store_runner() {
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");
  const auto wifi_carrier_header =
      read_source("main/platform/speaker_assets_wifi.h");
  const auto wifi_carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");
  const auto app_main = read_source("main/app_main.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto speaker_assets_cmake =
      read_source("features/speaker_assets/CMakeLists.txt");
  const auto store =
      read_source("features/speaker_assets/sound_asset_store.cpp");

  assert(supervisor_header.find("SoundAssetStore playback_store_") ==
         std::string::npos);
  assert(supervisor.find("playback_store_") == std::string::npos);
  assert(supervisor.find(
             "store_runner_.start_prepare_boot_read(&boot_job_)") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.poll_prepare_boot_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.start_release_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "store_runner_.poll_release_read(") !=
         std::string::npos);
  assert(supervisor.find(
             "if (boot_read_state_ == BootReadState::Idle) {\n"
             "    run_core_steps(now_ms, resource_steps_allowed);") !=
         std::string::npos);
  assert(supervisor.find(
             "completion.acquire_result ==\n"
             "            speaker_assets::SoundStoreResult::FactoryBlank") !=
         std::string::npos);
  assert(supervisor.find(
             "SpeakerAssetsBootPlaybackResult::FactoryDefault") !=
         std::string::npos);
  assert(store.find(
             "bank_a_result == SoundStoreResult::FactoryBlank &&\n"
             "      bank_b_result == SoundStoreResult::FactoryBlank") !=
         std::string::npos);
  assert(speaker_assets_cmake.find("EMBED_FILES") !=
         std::string::npos);
  assert(speaker_assets_cmake.find(
             "\"assets/waytoagi.eiad\"") !=
         std::string::npos);
  assert(speaker_output.find(
             "asset_decoder_.open_embedded(encoded, encoded_bytes)") !=
         std::string::npos);

  const auto local_begin = section(
      supervisor,
      "esp_err_t SpeakerAssetsSupervisor::begin_local(",
      "esp_err_t SpeakerAssetsSupervisor::start_wifi(");
  assert(local_begin.find("set_speaker_assets_frame_callback(") !=
         std::string::npos);
  assert(local_begin.find(
             "set_speaker_assets_response_accepted_callback(") !=
         std::string::npos);
  assert(local_begin.find("wifi_.begin(") == std::string::npos);
  assert(local_begin.find("if (!local_setup_ready_)") !=
         std::string::npos);
  const auto store_task_failure = section(
      local_begin,
      "if (task_result != pdPASS)",
      "log_internal_heap(\"store_after\")");
  assert(store_task_failure.find("usb_ = nullptr") ==
         std::string::npos);
  assert(store_task_failure.find("platform_task_ = nullptr") ==
         std::string::npos);
  assert(supervisor_header.find(
             "bool local_setup_ready_ = false;") !=
         std::string::npos);

  const auto wifi_begin = section(
      supervisor,
      "esp_err_t SpeakerAssetsSupervisor::start_wifi(",
      "bool SpeakerAssetsSupervisor::ready() const");
  assert(wifi_begin.find("wifi_.begin(") != std::string::npos);
  const auto publish_wifi = wifi_begin.find(
      "wifi_started_.store(true, std::memory_order_release);");
  const auto set_assets_ready =
      wifi_begin.find("wifi_.set_assets_ready(storage_.is_open());");
  const auto activate_wifi = wifi_begin.find("wifi_.activate();");
  assert(publish_wifi != std::string::npos);
  assert(set_assets_ready != std::string::npos);
  assert(activate_wifi != std::string::npos);
  assert(publish_wifi < set_assets_ready);
  assert(set_assets_ready < activate_wifi);
  assert(supervisor_header.find(
             "std::atomic<bool> wifi_started_{false};") !=
         std::string::npos);

  const auto carrier_begin = section(
      wifi_carrier,
      "esp_err_t SpeakerAssetsWifiCarrier::begin(",
      "void SpeakerAssetsWifiCarrier::activate()");
  const auto create_task =
      carrier_begin.find("xTaskCreatePinnedToCore(");
  const auto publish_initialized =
      carrier_begin.find("initialized_ = true;");
  const auto publish_callback = carrier_begin.find(
      "audio_->set_heartbeat_extension_callback(");
  assert(create_task != std::string::npos);
  assert(publish_initialized != std::string::npos);
  assert(publish_callback != std::string::npos);
  assert(create_task < publish_initialized);
  assert(publish_initialized < publish_callback);
  assert(carrier_begin.find(
             "set_heartbeat_extension_callback(nullptr, nullptr)") ==
         std::string::npos);
  assert(wifi_carrier_header.find(
             "std::atomic<bool> activated_{false};") !=
         std::string::npos);
  const auto carrier_run = section(
      wifi_carrier,
      "void SpeakerAssetsWifiCarrier::run()",
      "void SpeakerAssetsWifiCarrier::service_connection(");
  const auto activation_gate = carrier_run.find(
      "while (!activated_.load(std::memory_order_acquire))");
  const auto listener_socket =
      carrier_run.find("listener = socket(");
  assert(activation_gate != std::string::npos);
  assert(listener_socket != std::string::npos);
  assert(activation_gate < listener_socket);
  assert(carrier_run.find(
             "std::uint32_t listener_generation = 0U;") !=
         std::string::npos);
  assert(carrier_run.find(
             "speaker_assets_wifi_listener_matches_generation(") !=
         std::string::npos);
  assert(carrier_run.find(
             "listener_generation = snapshot.generation;") !=
         std::string::npos);
  assert(carrier_run.find(
             "SpeakerAssetsWifiAcceptIo::RebuildListener") !=
         std::string::npos);
  assert(carrier_run.find(
             "retire_listener();") !=
         std::string::npos);
  const auto listener_ready = section(
      wifi_carrier,
      "void SpeakerAssetsWifiCarrier::set_listener_ready(bool ready)",
      "void SpeakerAssetsWifiCarrier::set_active_route(");
  assert(listener_ready.find(
             "changed = state_.listener_ready != ready;") !=
         std::string::npos);
  assert(listener_ready.find(
             "audio_->request_heartbeat_refresh();") !=
         std::string::npos);

  const auto product_boot = section(
      app_main,
      "#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)\n"
      "  // Keep the microphone's frozen resource pool first.",
      "#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)");
  assert(product_boot.find(
             "app.speaker_skip_boot_after_deep_sleep =") !=
         std::string::npos);
  assert(product_boot.find("speaker_assets.begin_local(") ==
         std::string::npos);
  assert(product_boot.find("speaker.begin(") == std::string::npos);
  assert(product_boot.find(
             "wake_cause == ESP_SLEEP_WAKEUP_EXT1") !=
         std::string::npos);

  const auto lifecycle = section(
      app_main,
      "void service_speaker(AppContext* app)",
      "bool speaker_asset_resource_steps_allowed(");
  const auto begin_local =
      lifecycle.find("app->speaker_assets.begin_local(");
  const auto resolve_boot =
      lifecycle.find("take_boot_playback(");
  const auto begin_output =
      lifecycle.find("app->speaker.begin(");
  const auto request_asset =
      lifecycle.find("app->speaker.request_asset(");
  const auto request_factory =
      lifecycle.find("app->speaker.request_embedded_asset(");
  const auto request_shutdown =
      lifecycle.find("app->speaker.request_shutdown()");
  const auto release_lease =
      lifecycle.find("queue_playback_lease_release(");
  const auto wait_idle =
      lifecycle.find("app->speaker_assets.boot_idle()", release_lease);
  const auto start_wifi =
      lifecycle.find("app->speaker_assets.start_wifi(&app->audio)");
  const auto refresh_heartbeat =
      lifecycle.find("app->audio.request_heartbeat_refresh()");
  assert(begin_local != std::string::npos);
  assert(resolve_boot != std::string::npos);
  assert(begin_output != std::string::npos);
  assert(request_asset != std::string::npos);
  assert(request_factory != std::string::npos);
  assert(request_shutdown != std::string::npos);
  assert(release_lease != std::string::npos);
  assert(wait_idle != std::string::npos);
  assert(start_wifi != std::string::npos);
  assert(refresh_heartbeat != std::string::npos);
  assert(begin_local < start_wifi);
  assert(start_wifi < resolve_boot);
  assert(resolve_boot < begin_output);
  assert(begin_output < request_asset);
  assert(begin_output < request_factory);
  assert(request_asset < request_shutdown);
  assert(request_factory < request_shutdown);
  assert(request_shutdown < release_lease);
  assert(release_lease < wait_idle);
  assert(start_wifi < refresh_heartbeat);
  assert(lifecycle.find(
             "startup_decision.attempt_wifi") !=
         std::string::npos);
  assert(lifecycle.find(
             "SpeakerWifiAdmissionState::Deferred") !=
         std::string::npos);
  assert(lifecycle.find(
             "startup_decision.wait_for_local ||\n"
             "      !startup_decision.boot_allowed") !=
         std::string::npos);
  assert(lifecycle.find(
             "static_cast<std::int32_t>(\n"
             "            now_ms - app->speaker_local_retry_after_ms) >= 0") !=
         std::string::npos);
  assert(lifecycle.find(
             "static_cast<std::int32_t>(\n"
             "          startup_now_ms - app->speaker_wifi_retry_after_ms) >= 0") !=
         std::string::npos);
  assert(lifecycle.find(
             "app->speaker.shutdown_complete() &&\n"
             "      app->speaker_assets.boot_idle()") !=
         std::string::npos);
  const auto local_retryable = section(
      lifecycle,
      "if (local_result == ESP_ERR_NO_MEM)",
      "} else {\n          app->speaker_local_retry_after_ms = 0U;");
  assert(local_retryable.find("speaker_boot_skip_requested") ==
         std::string::npos);
  assert(local_retryable.find("settle_cold_boot_silent") ==
         std::string::npos);
  const auto wifi_retryable = section(
      lifecycle,
      "} else if (wifi_result == ESP_ERR_NO_MEM)",
      "} else {\n      app->speaker_wifi_admission =");
  assert(wifi_retryable.find(
             "SpeakerWifiAdmissionState::Deferred") !=
         std::string::npos);
  assert(wifi_retryable.find("speaker_boot_skip_requested") ==
         std::string::npos);
  assert(wifi_retryable.find("settle_cold_boot_silent") ==
         std::string::npos);

  const auto worker_loop = section(
      speaker_output,
      "void SpeakerOutput::run()",
      "SpeakerOutput::WorkerResult SpeakerOutput::play_sound(");
  const auto decoder_close =
      worker_loop.find("asset_decoder_.close();");
  const auto completion =
      worker_loop.find("publish_completed(generation, result);");
  assert(decoder_close != std::string::npos);
  assert(completion != std::string::npos);
  assert(decoder_close < completion);
  assert(worker_loop.find("vTaskSuspend(nullptr);") !=
         std::string::npos);

  const auto shutdown_cleanup = section(
      speaker_output,
      "if (shutdown_requested_.load(std::memory_order_acquire) &&",
      "bool SpeakerOutput::request_shutdown()");
  const auto suspended =
      shutdown_cleanup.find("eTaskGetState(worker_task_) != eSuspended");
  const auto disable =
      shutdown_cleanup.find("i2s_channel_disable(tx_channel_)");
  const auto delete_channel =
      shutdown_cleanup.find("i2s_del_channel(tx_channel_)");
  const auto delete_task =
      shutdown_cleanup.find("vTaskDelete(worker_task_)");
  assert(suspended != std::string::npos);
  assert(disable != std::string::npos);
  assert(delete_channel != std::string::npos);
  assert(delete_task != std::string::npos);
  assert(suspended < disable);
  assert(disable < delete_channel);
  assert(delete_channel < delete_task);
  const auto failed_begin_cleanup = section(
      speaker_output,
      "ai_keyboard::SpeakerPlaybackEvents SpeakerOutput::poll(",
      "bool SpeakerOutput::request_shutdown()");
  const auto completed_event_load = failed_begin_cleanup.find(
      "completed_generation_.load(std::memory_order_acquire)");
  const auto first_pcm_event_load = failed_begin_cleanup.find(
      "started_generation_.load(std::memory_order_acquire)");
  const auto folded_events = failed_begin_cleanup.find(
      "events = playback_.consume_worker_events(");
  assert(completed_event_load != std::string::npos);
  assert(first_pcm_event_load != std::string::npos);
  assert(folded_events != std::string::npos);
  assert(completed_event_load < first_pcm_event_load);
  assert(first_pcm_event_load < folded_events);
  assert(failed_begin_cleanup.find("return events;") != std::string::npos);
  assert(failed_begin_cleanup.find(
             "if (worker_task_ == nullptr && tx_channel_ != nullptr)") !=
         std::string::npos);
  assert(failed_begin_cleanup.find(
             "const auto delete_result = i2s_del_channel(tx_channel_)") !=
         std::string::npos);

  const auto rejected_playback = lifecycle.find(
      "ESP_LOGW(kTag, \"boot asset playback request rejected\");");
  assert(rejected_playback != std::string::npos);
  const auto rejected_shutdown = lifecycle.find(
      "SpeakerStartupPhase::ShutdownOutput", rejected_playback);
  assert(rejected_shutdown != std::string::npos);
  assert(rejected_shutdown - rejected_playback < 300);
}

void awake_owner_waits_for_notifications_and_business_deadlines() {
  const auto app_main = read_source("main/app_main.cpp");

  const auto power_management = section(
      app_main,
      "void configure_power_management()",
      "std::uint32_t next_power_cycle_sequence(");
  assert(power_management.find("config.light_sleep_enable = false;") !=
         std::string::npos);

  const char* retired_awake_policy_paths[] = {
      "PowerPolicyMode",
      "try_controlled_light_sleep(",
      "esp_light_sleep_start(",
      "poll_interval_ms(",
      "sync_ble_connection_power_profile(",
      "prepare_for_input_delivery(",
  };
  for (const auto* retired : retired_awake_policy_paths) {
    assert(app_main.find(retired) == std::string::npos);
  }

  const auto waiter = section(
      app_main,
      "void wait_for_awake_work(",
      "bool bridged_hotkey_work_pending(");
  assert(waiter.find("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)") !=
         std::string::npos);
  assert(waiter.find(
             "ulTaskNotifyTake(pdTRUE, delay_ticks(decision.wait_ms))") !=
         std::string::npos);

  const auto planner = section(
      app_main,
      "ai_keyboard::AwakeWaitDecision plan_next_awake_work(",
      "}  // namespace");
  assert(planner.find("ai_keyboard::AwakeWaitPlanner planner(now_ms)") !=
         std::string::npos);
  assert(planner.find("next_transition_deadline_ms") != std::string::npos);
  assert(planner.find("next_update_deadline_ms") != std::string::npos);
  assert(planner.find("\"encoder_hold\"") != std::string::npos);
  assert(planner.find("\"platform_selection\"") != std::string::npos);
  assert(planner.find("\"wheel_flush\"") != std::string::npos);
  assert(planner.find("\"status_led\"") != std::string::npos);
  assert(planner.find("app->usb.work_schedule(now_ms)") !=
         std::string::npos);
  assert(planner.find("\"deep_sleep\"") != std::string::npos);
  assert(planner.find("return planner.decision();") != std::string::npos);

  const auto input_handler = section(
      app_main,
        "bool handle_input_event(",
      "void load_stored_config(");
  assert(input_handler.find(
             "mark_user_activity(app, now, \"debounced_input\")") !=
         std::string::npos);
  const auto config_apply = section(
      app_main,
      "void apply_pending_config(",
      "void apply_pending_agent_status(");
  const auto status_refresh = section(
      app_main,
      "void process_pending_status_refresh(",
      "void log_heartbeat(");
  assert(config_apply.find("mark_user_activity(") == std::string::npos);
  assert(status_refresh.find("mark_user_activity(") == std::string::npos);

  const auto sleep_inputs = section(
      app_main,
      "ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,",
      "// Whole-device sleep admission");
  assert(sleep_inputs.find("app->keyboard_delivery.pending()") !=
         std::string::npos);
  assert(sleep_inputs.find("app->ble.input_delivery_pending()") !=
         std::string::npos);
  assert(sleep_inputs.find("bridged_hotkey_work_pending(app)") !=
         std::string::npos);

  const auto loop_start = app_main.find(
      "ESP_LOGI(kTag, \"platform loop started; BLE HID + USB HID + S3C config enabled\")");
  const auto deep_sleep = app_main.find(
      "maybe_enter_deep_sleep(&app, millis())", loop_start);
  const auto plan = app_main.find(
      "app.next_awake_wait = plan_next_awake_work(&app, millis())", deep_sleep);
  const auto wait = app_main.find(
      "wait_for_awake_work(app.next_awake_wait)", plan);
  assert(loop_start != std::string::npos);
  assert(deep_sleep != std::string::npos);
  assert(plan != std::string::npos);
  assert(wait != std::string::npos);
  assert(loop_start < deep_sleep);
  assert(deep_sleep < plan);
  assert(plan < wait);
}

void speaker_boot_worker_completion_reenters_the_owner_without_other_events() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");

  // Preparing is callback-driven Store work and must remain asleep until the
  // worker notification. Once poll() consumes that completion into a terminal
  // Boot result, however, service_speaker() has already run for this owner
  // pass, so the next pass must be scheduled immediately. BLE, USB, LED, and
  // periodic telemetry are not valid substitutes for that continuation.
  assert(supervisor_header.find(
             "ai_keyboard::OwnerServiceSchedule boot_work_schedule() const;") !=
         std::string::npos);
  const auto schedule = section(
      supervisor,
      "SpeakerAssetsSupervisor::boot_work_schedule() const",
      "bool SpeakerAssetsSupervisor::boot_idle() const");
  assert(schedule.find("BootReadState::Preparing") != std::string::npos);
  assert(schedule.find("BootReadState::Ready") != std::string::npos);
  assert(schedule.find("BootReadState::FactoryDefault") !=
         std::string::npos);
  assert(schedule.find("BootReadState::Unavailable") != std::string::npos);
  assert(schedule.find("runnable_now") != std::string::npos);

  // ResolveBoot can lose the shared Store race while BootRead is still Idle.
  // Preserve that owner request explicitly: while the shared job is active it
  // waits for the Store notification, and after poll() retires that job it is
  // immediately runnable. Leaving this as plain Idle loses the continuation.
  assert(supervisor_header.find("bool boot_start_pending_ = false;") !=
         std::string::npos);
  assert(schedule.find("boot_start_pending_") != std::string::npos);
  assert(schedule.find("!store_runner_.job_active()") !=
         std::string::npos);
  const auto take_boot = section(
      supervisor,
      "SpeakerAssetsSupervisor::take_boot_playback(",
      "bool SpeakerAssetsSupervisor::queue_playback_lease_release(");
  assert(take_boot.find(
             "SpeakerAssetsInternalJobStartResult::Busy") !=
         std::string::npos);
  assert(take_boot.find("boot_start_pending_ = true") !=
         std::string::npos);
  assert(take_boot.find("boot_start_pending_ = false") !=
         std::string::npos);

  const auto planner = section(
      app_main,
      "ai_keyboard::AwakeWaitDecision plan_next_awake_work(",
      "}  // namespace");
  assert(planner.find("const auto speaker_boot_schedule") !=
         std::string::npos);
  assert(planner.find("app->speaker_assets.boot_work_schedule()") !=
         std::string::npos);
  assert(planner.find("\"speaker_boot\"") != std::string::npos);

  // Release completion is consumed by assets.poll() after service_speaker()
  // has already observed ReleasePending. The owner therefore needs one more
  // immediate pass while its phase is WaitLeaseIdle and the supervisor is
  // Idle. Requiring the phase guard prevents Idle from becoming a permanent
  // runnable state after startup reaches Ready.
  assert(planner.find("SpeakerStartupPhase::WaitLeaseIdle") !=
         std::string::npos);
  assert(planner.find("app->speaker_assets.boot_idle()") !=
         std::string::npos);
  assert(planner.find("\"speaker_boot_idle\"") != std::string::npos);

  // Store priority is intentionally denied for 30ms after input. A worker
  // notification can be consumed before that window expires, so outstanding
  // Boot work must arm the exact quiet-window deadline rather than relying on
  // a future BLE/USB/LED notification. Preparing itself remains non-runnable.
  assert(planner.find("speaker_boot_schedule.outstanding") !=
         std::string::npos);
  assert(planner.find("app->last_input_ms + kSpeakerAssetsInputQuietMs") !=
         std::string::npos);
  assert(planner.find("\"speaker_assets_input_quiet\"") !=
         std::string::npos);

  // A Wi-Fi carrier allocation failure may become retry-due while Boot still
  // owns the shared heap. The expired deadline must stay dormant until every
  // Boot resource has been released, otherwise the event owner spins instead
  // of sleeping on real worker publications.
  assert(planner.find("const bool speaker_wifi_retry_eligible") !=
         std::string::npos);
  assert(planner.find("SpeakerWifiAdmissionState::Deferred") !=
         std::string::npos);
  assert(planner.find("speaker_wifi_retry_eligible &&") !=
         std::string::npos);
  assert(planner.find("app->speaker_startup_phase == SpeakerStartupPhase::Ready") !=
         std::string::npos);
}

void speaker_shutdown_cleanup_has_a_bounded_owner_retry() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto planner = section(
      app_main,
      "ai_keyboard::AwakeWaitDecision plan_next_awake_work(",
      "}  // namespace");

  // The worker publishes quiesced and notifies before vTaskSuspend(). The
  // notified owner can therefore observe eTaskGetState()!=eSuspended and
  // return from poll() with no second callback forthcoming. I2S disable/delete
  // can fail transiently in the same cleanup path. Both cases need a positive,
  // bounded deadline until shutdown_complete(), never runnable_now spinning.
  assert(app_main.find("kSpeakerShutdownSettleRetryMs") !=
         std::string::npos);
  assert(planner.find("SpeakerStartupPhase::ShutdownOutput") !=
         std::string::npos);
  assert(planner.find("SpeakerStartupPhase::ReleaseLease") !=
         std::string::npos);
  assert(planner.find("!app->speaker.shutdown_complete()") !=
         std::string::npos);
  assert(planner.find("now_ms + kSpeakerShutdownSettleRetryMs") !=
         std::string::npos);
  assert(planner.find("\"speaker_shutdown_settle\"") !=
         std::string::npos);

  const auto cleanup = section(
      speaker_output,
      "if (shutdown_requested_.load(std::memory_order_acquire) &&",
      "bool SpeakerOutput::request_shutdown()");
  const auto quiesced_before_suspend = cleanup.find(
      "eTaskGetState(worker_task_) != eSuspended");
  const auto disable_retry = cleanup.find(
      "const auto disable_result = i2s_channel_disable(tx_channel_)");
  const auto delete_retry = cleanup.find(
      "const auto delete_result = i2s_del_channel(tx_channel_)");
  assert(quiesced_before_suspend != std::string::npos);
  assert(disable_retry != std::string::npos);
  assert(delete_retry != std::string::npos);
}

void speaker_assets_mailbox_and_core_publish_durable_owner_work() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto supervisor_header =
      read_source("main/platform/speaker_assets_supervisor.h");
  const auto supervisor =
      read_source("main/platform/speaker_assets_supervisor.cpp");

  // One first-epoch callback publishes two durable records (Open, then Frame)
  // but gives the shared platform task only one wake hint. The task notification
  // is deliberately binary (`ulTaskNotifyTake(pdTRUE, ...)`), so record counts
  // must be recovered from mailbox/Core state rather than notification counts.
  const auto usb_ingress = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::enqueue_usb_frame(",
      "bool SpeakerAssetsSupervisor::enqueue_wifi_frame(");
  const auto ensure_route =
      usb_ingress.find("ensure_mailbox_usb_route_locked(endpoint_epoch)");
  const auto enqueue_frame =
      usb_ingress.find("mailbox_.enqueue_usb_frame(", ensure_route);
  const auto notify =
      usb_ingress.find("xTaskNotifyGive(platform_task_)", enqueue_frame);
  assert(ensure_route != std::string::npos);
  assert(enqueue_frame != std::string::npos);
  assert(notify != std::string::npos);
  assert(ensure_route < enqueue_frame);
  assert(enqueue_frame < notify);
  assert(count_occurrences(
             usb_ingress, "xTaskNotifyGive(platform_task_)") == 1U);
  const auto ensure_usb = section(
      supervisor,
      "bool SpeakerAssetsSupervisor::ensure_mailbox_usb_route_locked(",
      "bool SpeakerAssetsSupervisor::ensure_mailbox_wifi_route_locked(");
  assert(ensure_usb.find("mailbox_.enqueue_route_opened(next_route)") !=
         std::string::npos);

  // Generic resource-sync work needs its own schedule; Boot scheduling cannot
  // stand in for mailbox/Core progress after startup is already Idle.
  assert(supervisor_header.find(
             "ai_keyboard::OwnerServiceSchedule work_schedule(\n"
             "      std::uint32_t now_ms,\n"
             "      bool resource_steps_allowed) const;") !=
         std::string::npos);
  const auto runtime_schedule = section(
      supervisor,
      "SpeakerAssetsSupervisor::work_schedule(",
      "SpeakerAssetsSupervisor::boot_work_schedule() const");
  assert(runtime_schedule.find("mailbox_.data_size()") !=
         std::string::npos);
  assert(runtime_schedule.find("mailbox_.close_size()") !=
         std::string::npos);
  assert(runtime_schedule.find("portENTER_CRITICAL(&mailbox_mux_)") !=
         std::string::npos);
  assert(runtime_schedule.find("portEXIT_CRITICAL(&mailbox_mux_)") !=
         std::string::npos);
  assert(runtime_schedule.find("BootReadState::Idle") !=
         std::string::npos);
  assert(runtime_schedule.find("const bool core_lifecycle_pending") !=
         std::string::npos);
  assert(runtime_schedule.find("core_.lifecycle_size()") !=
         std::string::npos);
  assert(runtime_schedule.find("const bool core_ingress_pending") !=
         std::string::npos);
  assert(runtime_schedule.find("core_.ingress_size()") !=
         std::string::npos);
  assert(runtime_schedule.find("core_.action_pending()") !=
         std::string::npos);
  assert(runtime_schedule.find("resource_steps_allowed") !=
         std::string::npos);
  assert(runtime_schedule.find("store_runner_.job_active()") !=
         std::string::npos);
  assert(runtime_schedule.find("schedule.outstanding") !=
         std::string::npos);
  assert(runtime_schedule.find("schedule.runnable_now") !=
         std::string::npos);
  const auto runnable_assignment = section(
      runtime_schedule,
      "schedule.runnable_now =",
      "// Partial assembly");
  // Mailbox import is one bounded owner unit and may continue while Boot owns
  // the Store runner. Core execution is different: it advances only while the
  // Boot read state is Idle. Do not additionally gate on boot_start_pending_:
  // that state can mean a protocol Store action must first be retired before
  // the retained Boot read can acquire the shared slot.
  assert(runnable_assignment.find("mailbox_pending") !=
         std::string::npos);
  assert(runnable_assignment.find("BootReadState::Idle") !=
         std::string::npos);
  assert(runnable_assignment.find("core_lifecycle_pending") !=
         std::string::npos);
  assert(runnable_assignment.find("core_ingress_pending") !=
         std::string::npos);
  // step_once() gives lifecycle traffic priority over an older action, so a
  // Close remains runnable. Ingress is behind that action and must stop until
  // the action retires; otherwise the owner immediately re-enters forever.
  assert(runnable_assignment.find("!core_action_pending") !=
         std::string::npos);
  assert(runnable_assignment.find("reply_capacity_available") !=
         std::string::npos);

  const auto runtime_header =
      read_source("features/speaker_assets/include/speaker_assets/speaker_assets_runtime.h");
  const auto runtime_core =
      read_source("features/speaker_assets/speaker_assets_runtime.cpp");
  assert(runtime_header.find(
             "bool next_deadline_ms(std::uint32_t now_ms,") !=
         std::string::npos);
  assert(runtime_header.find("std::uint32_t* deadline_ms) const;") !=
         std::string::npos);
  assert(runtime_schedule.find("core_.next_deadline_ms(") !=
         std::string::npos);
  const auto next_deadline = section(
      runtime_core,
      "SpeakerAssetsRuntimeCore::next_deadline_ms(",
      "SpeakerAssetsRuntimeCore::session_phase() const");
  assert(next_deadline.find("session_partial_active_") !=
         std::string::npos);
  assert(next_deadline.find("partial_timeout_ms_") !=
         std::string::npos);
  assert(next_deadline.find("session_lease_active_") !=
         std::string::npos);
  assert(next_deadline.find("session_lease_ms_") !=
         std::string::npos);
  assert(next_deadline.find("!pending_action_active_") !=
         std::string::npos);
  assert(next_deadline.find("!route_has_ingress(") !=
         std::string::npos);

  const auto planner = section(
      app_main,
      "ai_keyboard::AwakeWaitDecision plan_next_awake_work(",
      "}  // namespace");
  assert(planner.find("const auto speaker_assets_schedule") !=
         std::string::npos);
  assert(planner.find("app->speaker_assets.work_schedule(") !=
         std::string::npos);
  assert(planner.find(
             "planner.add_schedule(speaker_assets_schedule, "
             "\"speaker_assets\")") != std::string::npos);

  // A protocol Store action denied only by the 30ms post-input quiet window is
  // time-driven just like Boot Store work. Either schedule must arm that exact
  // deadline; Store-active and endpoint-backpressured states remain callback
  // driven and must not be made unconditionally runnable.
  const auto quiet_gate = section(
      planner,
      "const bool speaker_assets_input_quiet",
      "planner.add_deadline(");
  assert(quiet_gate.find("speaker_boot_schedule.outstanding") !=
         std::string::npos);
  assert(quiet_gate.find("speaker_assets_schedule.outstanding") !=
         std::string::npos);
}

void v2_cold_boot_feedback_starts_from_the_first_pcm_event() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto coordinator_source =
      read_source("components/keyboard/src/cold_boot_feedback.cpp");
  const auto led_header =
      read_source("main/platform/led_strip_status.h");
  const auto led_source =
      read_source("main/platform/led_strip_status.cpp");
  const auto speaker_output =
      read_source("main/platform/speaker_output.cpp");
  const auto startup = section(
      app_main,
      "ESP_ERROR_CHECK(app.leds.begin());",
      "ESP_LOGI(kTag, \"platform loop started; BLE HID + USB HID + S3C config enabled\")");
  const auto owner_loop = section(
      app_main,
      "ESP_LOGI(kTag, \"platform loop started; BLE HID + USB HID + S3C config enabled\")",
      "wait_for_awake_work(app.next_awake_wait)");

  // Battery and USB share one path. Setup reserves visual ownership before the
  // first status sync but cannot render Pixel 0 or guess an audio delay. The
  // explicit maximum wait is a liveness/fallback contract, not the normal
  // trigger for either medium.
  assert(startup.find("external_power_status_active") == std::string::npos);
  assert(startup.find("skipped on battery power") == std::string::npos);
  assert(startup.find("run_v2_led_bringup_probe(&app)") ==
         std::string::npos);
  assert(startup.find("vTaskDelay(") == std::string::npos);
  const auto coordinator_begin =
      startup.find("app.cold_boot_feedback.begin(");
  const auto initial_led_status =
      startup.find("sync_led_status(&app, millis())");
  const auto admission_window =
      startup.find("cold_boot_feedback.start_admission_window(");
  assert(coordinator_begin != std::string::npos);
  assert(initial_led_status != std::string::npos);
  assert(admission_window != std::string::npos);
  assert(coordinator_begin < initial_led_status);
  assert(initial_led_status < admission_window);
  assert(startup.find("kColdBootFeedbackMaxAdmissionWaitMs") !=
         std::string::npos);
  assert(startup.find("app.leds.start_cold_boot_sequence(millis())") ==
         std::string::npos);

  // A deep-sleep KEY_WAKE is a resume path, not a cold boot. It must not spend
  // time in decorative startup feedback or replay the Boot sound.
  assert(startup.find("wake_cause != ESP_SLEEP_WAKEUP_EXT1") !=
         std::string::npos);
  assert(startup.find("speaker_skip_boot_after_deep_sleep") !=
         std::string::npos);

  // SpeakerOutput publishes Started only after the first decoded PCM buffer is
  // accepted by I2S. The owner consumes that exact generation-tagged edge and
  // starts the reserved strip before its normal LED update in the same pass.
  const auto first_pcm_write =
      speaker_output.find("err = write_samples(frame.data(), frame.size())");
  const auto started_publish =
      speaker_output.find("publish_started(generation)", first_pcm_write);
  assert(first_pcm_write != std::string::npos);
  assert(started_publish != std::string::npos);
  assert(first_pcm_write < started_publish);
  const auto speaker_service = section(
      app_main,
      "void service_speaker(AppContext* app)",
      "bool speaker_asset_resource_steps_allowed(");
  assert(speaker_service.find("const auto speaker_events =") !=
         std::string::npos);
  assert(speaker_service.find("speaker_events.first_pcm()") !=
         std::string::npos);
  assert(speaker_service.find("observe_cold_boot_first_pcm(") !=
         std::string::npos);
  assert(speaker_service.find("speaker_events.terminal()") !=
         std::string::npos);
  assert(speaker_service.find("settle_cold_boot_silent(") !=
         std::string::npos);
  const auto speaker_producer = owner_loop.find("service_speaker(&app)");
  const auto store_poll = owner_loop.find("app.speaker_assets.poll(");
  const auto speaker_consumer =
      owner_loop.find("service_speaker(&app)", speaker_producer + 1U);
  const auto liveness_service = owner_loop.find(
      "service_cold_boot_feedback_liveness(&app, millis())");
  const auto led_update =
      owner_loop.find("app.leds.update(millis())");
  assert(speaker_producer != std::string::npos);
  assert(store_poll != std::string::npos);
  assert(speaker_consumer != std::string::npos);
  assert(liveness_service != std::string::npos);
  assert(led_update != std::string::npos);
  // The first pass can create a Store job, poll() grants or consumes it, and
  // the second pass observes any synchronous completion before timeout. This
  // exact bounded pump avoids both the initial priority-denied stall and the
  // completion-vs-deadline race.
  assert(speaker_producer < store_poll);
  assert(store_poll < speaker_consumer);
  assert(speaker_consumer < liveness_service);
  assert(speaker_consumer < led_update);

  // Reservation is logical ownership only: no frame or RMT flush. Its owner
  // coordinator publishes an exact liveness deadline to the event scheduler;
  // input priority and that deadline both abandon Boot audio before starting
  // LED-only feedback, so a pending Store/Wi-Fi operation cannot leave it black.
  // Starting from that reservation preserves any status deferred while audio
  // was preparing. Every explicit silent terminal branch uses the same one-shot
  // coordinator instead of allowing a late Boot sound.
  assert(led_header.find("void reserve_cold_boot_sequence()") !=
         std::string::npos);
  assert(led_header.find("void start_cold_boot_sequence(std::uint32_t now_ms)") !=
         std::string::npos);
  assert(led_header.find("ai_keyboard::BootLedSequence cold_boot_sequence_") !=
         std::string::npos);
  const auto reserve_led = section(
      led_source,
      "void StatusLedStrip::reserve_cold_boot_sequence()",
      "void StatusLedStrip::start_cold_boot_sequence(");
  assert(reserve_led.find("cold_boot_sequence_.reserve()") !=
         std::string::npos);
  assert(reserve_led.find("flush()") == std::string::npos);
  assert(app_main.find(
             "cold_boot_feedback.next_deadline_ms(&deadline_ms)") !=
         std::string::npos);
  assert(app_main.find("\"cold_boot_feedback_liveness\"") !=
         std::string::npos);
  assert(app_main.find("on_liveness_deadline(now_ms)") !=
         std::string::npos);
  assert(coordinator_source.find("start_admission_window(") !=
         std::string::npos);
  assert(app_main.find("on_priority_preempted()") !=
         std::string::npos);
  assert(app_main.find(
             "preempt_cold_boot_audio(app, \"input_priority_preempted\")") !=
         std::string::npos);
  assert(app_main.find("app->speaker_boot_skip_requested = true") !=
         std::string::npos);
  assert(speaker_service.find("cold_boot_audio_allowed") !=
         std::string::npos);
  const auto request_rejected =
      speaker_service.find("if (!requested)");
  const auto attempt_committed =
      speaker_service.find("on_audio_attempt_committed()", request_rejected);
  const auto wait_playback =
      speaker_service.find("SpeakerStartupPhase::WaitPlayback", attempt_committed);
  assert(request_rejected != std::string::npos);
  assert(attempt_committed != std::string::npos);
  assert(wait_playback != std::string::npos);
  assert(request_rejected < attempt_committed);
  assert(attempt_committed < wait_playback);
  assert(coordinator_source.find("if (audio_attempt_committed_)") !=
         std::string::npos);
  const auto commit_method = section(
      coordinator_source,
      "void ColdBootFeedbackCoordinator::on_audio_attempt_committed()",
      "ColdBootFeedbackCoordinator::on_first_pcm_submitted(");
  assert(commit_method.find("liveness_deadline_armed_ = false") !=
         std::string::npos);
  const auto start_led = section(
      led_source,
      "void StatusLedStrip::start_cold_boot_sequence(",
      "bool StatusLedStrip::cold_boot_sequence_active()");
  assert(start_led.find("started_from_reservation") != std::string::npos);
  assert(start_led.find("if (!started_from_reservation)") !=
         std::string::npos);
  assert(led_source.find("cold_boot_sequence_.next_deadline_ms(") !=
         std::string::npos);
  assert(led_header.find("ai_keyboard::BootLedDeferredFeedback deferred_feedback_") !=
         std::string::npos);
  assert(led_source.find("deferred_feedback_.defer(") !=
         std::string::npos);
  assert(led_source.find("deferred_feedback_.take(") != std::string::npos);
  assert(led_source.find("vTaskDelay(") == std::string::npos);
  assert(app_main.find("local_service_terminal_failure") !=
         std::string::npos);
  assert(app_main.find(
             "Wi-Fi carrier allocation deferred; local Boot remains eligible") !=
         std::string::npos);
  assert(app_main.find("boot_sound_unavailable") != std::string::npos);
  assert(app_main.find("speaker_begin_failed") != std::string::npos);
  assert(app_main.find("playback_request_rejected") != std::string::npos);
  assert(app_main.find("microphone_priority_before_boot") !=
         std::string::npos);
}

void speaker_asset_wifi_admission_owns_ingress_power_and_listener_epoch() {
  const auto audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto acquire = section(
      audio,
      "bool KeyboardAudioLink::acquire_wifi_service_lease(",
      "bool KeyboardAudioLink::release_wifi_service_lease(");
  const auto lease_publish =
      acquire.find("wifi_service_lease_ = {");
  const auto ingress_active =
      acquire.find("esp_wifi_set_ps(WIFI_PS_NONE)");
  const auto successful_return =
      acquire.rfind("return true;");
  assert(acquire.find("WifiOpGuard wifi_guard(wifi_op_mutex_);") !=
         std::string::npos);
  assert(lease_publish != std::string::npos);
  assert(ingress_active != std::string::npos);
  assert(successful_return != std::string::npos);
  assert(lease_publish < ingress_active);
  assert(ingress_active < successful_return);
  assert(acquire.find("wifi service ingress activation failed") !=
         std::string::npos);

  const auto carrier =
      read_source("main/platform/speaker_assets_wifi.cpp");
  const auto run = section(
      carrier,
      "void SpeakerAssetsWifiCarrier::run()",
      "void SpeakerAssetsWifiCarrier::service_connection(");
  const auto nonce_rotation =
      run.find("rotate_endpoint_nonce();");
  const auto ready_publish =
      run.find("set_listener_ready(true);", nonce_rotation);
  assert(nonce_rotation != std::string::npos);
  assert(ready_publish != std::string::npos);
  assert(nonce_rotation < ready_publish);

  const auto service =
      run.find("service_connection(client, accepted_snapshot);");
  const auto client_close =
      run.find("close_socket(&client);", service);
  const auto listener_retire =
      run.find("retire_listener();", client_close);
  assert(service != std::string::npos);
  assert(client_close != std::string::npos);
  assert(listener_retire != std::string::npos);
  assert(service < client_close);
  assert(client_close < listener_retire);
}

void encoder_text_caret_selection_uses_native_keyboard_chords() {
  const auto app_main = read_source("main/app_main.cpp");
  const auto usb = read_source("main/platform/usb_hid.cpp");
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto gpio_header = read_source("main/platform/gpio_keys.h");
  const auto gpio_source = read_source("main/platform/gpio_keys.cpp");

  const auto click = section(
      app_main,
      "void dispatch_encoder_press_click(AppContext* app)",
      "ai_keyboard::EncoderScrollAxis active_encoder_axis(");
  assert(click.find("ActionKind::TextCaretSelect") != std::string::npos);
  assert(click.find("encoder_text_selection_active = true") !=
         std::string::npos);
  assert(click.find("encoder_text_selection_active = false") !=
         std::string::npos);
  assert(click.find("encoder_text_selection_exit_pending = true") !=
         std::string::npos);
  assert(click.find("flush_encoder_text_selection(app)") != std::string::npos);

  const auto chord = section(
      app_main,
      "bool queue_encoder_text_selection_chord(",
      "bool flush_encoder_text_selection(");
  assert(chord.find("Shift+ArrowRight") != std::string::npos);
  assert(chord.find("Shift+ArrowLeft") != std::string::npos);
  const auto baseline_flush = chord.find("flush_pending_keyboard_snapshot(app)");
  const auto owner_recheck = chord.find(
      "!encoder_text_selection_owns_keyboard_transport(app)",
      baseline_flush);
  const auto press_source = chord.find("held_keyboard.press(source, report)");
  const auto release_source = chord.find("held_keyboard.release(source)");
  const auto send_press = chord.find("HidReportClass::KeyboardPress");
  const auto send_release = chord.find("send_keyboard_snapshot(", send_press);
  assert(press_source != std::string::npos);
  assert(baseline_flush != std::string::npos);
  assert(owner_recheck != std::string::npos);
  assert(release_source != std::string::npos);
  assert(send_press != std::string::npos);
  assert(send_release != std::string::npos);
  assert(press_source < release_source);
  assert(baseline_flush < owner_recheck);
  assert(owner_recheck < press_source);
  assert(release_source < send_press);
  assert(chord.find("HidReportClass::KeyboardRelease") != std::string::npos);
  assert(chord.find("HidReportClass::KeyboardAllReleased") !=
         std::string::npos);
  assert(chord.find("keyboard_transport.commit_snapshot(false)") !=
         std::string::npos);
  assert(chord.find("Keep the exact host latched across detents") !=
         std::string::npos);

  const auto flush = section(
      app_main,
      "bool flush_encoder_text_selection(AppContext* app)",
      "void release_keyboard_reports(");
  const auto queue_front = flush.find("pending_encoder_text_selection_steps.front");
  const auto queue_chord =
      flush.find("queue_encoder_text_selection_chord", queue_front);
  const auto queue_consume = flush.find("consume_if_sequence(");
  assert(queue_front != std::string::npos);
  assert(queue_chord != std::string::npos);
  assert(queue_consume != std::string::npos);
  assert(queue_front < queue_chord);
  assert(queue_chord < queue_consume);
  assert(flush.find("kEncoderSelectionChordsPerFlush") != std::string::npos);
  assert(flush.find("keyboard_transport.commit_snapshot(true)") !=
         std::string::npos);

  const auto transport_reconcile = section(
      app_main,
      "void reconcile_keyboard_transport_lifetimes(AppContext* app)",
      "bool send_keyboard_snapshot(");
  assert(transport_reconcile.find(
             "encoder_text_selection_owns_keyboard_transport(app)") !=
         std::string::npos);
  assert(transport_reconcile.find(
             "pending_encoder_text_selection_steps.clear()") !=
         std::string::npos);

  assert(gpio_header.find("std::uint32_t timestamp_ms = 0") !=
         std::string::npos);
  assert(gpio_header.find("std::uint32_t order_sequence = 0") !=
         std::string::npos);
  assert(gpio_header.find("using InputEventCallback = bool (*)") !=
         std::string::npos);
  const auto input_poll = section(
      gpio_source,
      "void GpioInputScanner::poll(",
      "void GpioInputScanner::process_input_snapshot(");
  const auto peek_input = input_poll.find("peek_input_edge_snapshot(&snapshot)");
  const auto peek_encoder = input_poll.find(
      "peek_pending_encoder_steps(&encoder_run)");
  const auto chronological_deadline = input_poll.find(
      "process_debounce_deadlines_through(", peek_encoder);
  const auto emitted_timestamp = input_poll.find(
      "encoder_run.first_timestamp_ms", chronological_deadline);
  const auto ordered_tie_break = input_poll.find(
      "encoder_run.first_order_sequence");
  const auto claim_encoder_run = input_poll.find(
      "claim_pending_encoder_steps(&encoder_run)");
  const auto admit_encoder_run = input_poll.find("if (!emit(");
  const auto retain_on_backpressure = input_poll.find(
      "break;", admit_encoder_run);
  const auto pop_admitted_run = input_poll.find(
      "take_pending_encoder_steps(&accepted_run)", admit_encoder_run);
  assert(peek_input != std::string::npos);
  assert(peek_encoder != std::string::npos);
  assert(chronological_deadline != std::string::npos);
  assert(emitted_timestamp != std::string::npos);
  assert(ordered_tie_break != std::string::npos);
  assert(claim_encoder_run != std::string::npos);
  assert(admit_encoder_run != std::string::npos);
  assert(retain_on_backpressure != std::string::npos);
  assert(pop_admitted_run != std::string::npos);
  assert(peek_input < chronological_deadline);
  assert(peek_encoder < chronological_deadline);
  assert(chronological_deadline < emitted_timestamp);
  assert(claim_encoder_run < admit_encoder_run);
  assert(admit_encoder_run < retain_on_backpressure);
  assert(retain_on_backpressure < pop_admitted_run);
  assert(input_poll.find("if (!source_backpressured)", pop_admitted_run) !=
         std::string::npos);
  assert(gpio_source.find("bool ordered_event_before_or_equal(") !=
         std::string::npos);
  assert(gpio_source.find("pending_encoder_steps_.break_coalescing()") !=
         std::string::npos);
  const auto input_isr = section(
      gpio_source,
      "void GpioInputScanner::handle_input_edge_from_isr()",
      "bool GpioInputScanner::peek_input_edge_snapshot(");
  const auto input_isr_lock = input_isr.find(
      "portENTER_CRITICAL_ISR(&wake_mux_)");
  const auto input_isr_timestamp = input_isr.find("esp_timer_get_time()");
  const auto input_isr_snapshot = input_isr.find("active_input_mask()");
  const auto input_isr_sequence = input_isr.find(
      "snapshot.order_sequence = next_input_order_sequence_++");
  assert(input_isr_lock < input_isr_timestamp);
  assert(input_isr_timestamp < input_isr_snapshot);
  assert(input_isr_snapshot < input_isr_sequence);

  const auto encoder_isr = section(
      gpio_source,
      "void GpioInputScanner::handle_encoder_edge_from_isr()",
      "bool GpioInputScanner::peek_pending_encoder_steps(");
  const auto encoder_isr_lock = encoder_isr.find(
      "portENTER_CRITICAL_ISR(&wake_mux_)");
  const auto encoder_isr_timestamp = encoder_isr.find("esp_timer_get_time()");
  const auto encoder_isr_snapshot = encoder_isr.find("encoder_state()");
  const auto encoder_isr_sequence = encoder_isr.find(
      "const auto order_sequence = next_input_order_sequence_++");
  assert(encoder_isr_lock < encoder_isr_timestamp);
  assert(encoder_isr_timestamp < encoder_isr_snapshot);
  assert(encoder_isr_snapshot < encoder_isr_sequence);

  const auto selection_dispatch = section(
      app_main,
      "bool dispatch_encoder_text_selection_step(",
      "bool dispatch_encoder_rotation(");
  assert(selection_dispatch.find("const auto retain_steps") !=
         std::string::npos);
  assert(selection_dispatch.find("retained = retain_steps()") !=
         std::string::npos);
  assert(selection_dispatch.find("return false") != std::string::npos);
  assert(selection_dispatch.find("split_on_saturation") != std::string::npos);
  const auto initial_flush = selection_dispatch.find(
      "flush_encoder_text_selection(app)");
  const auto canceled_source = selection_dispatch.find(
      "if (!app->encoder_text_selection_active)", initial_flush);
  const auto retain_source = selection_dispatch.find(
      "const auto retain_steps", canceled_source);
  assert(initial_flush != std::string::npos);
  assert(canceled_source != std::string::npos);
  assert(retain_source != std::string::npos);
  assert(initial_flush < canceled_source);
  assert(canceled_source < retain_source);
  assert(selection_dispatch.find("return true;", canceled_source) <
         retain_source);

  const auto rotation_dispatch = section(
      app_main,
      "bool dispatch_encoder_rotation(",
      "void toggle_encoder_scroll_axis(");
  const auto drain_exit = rotation_dispatch.find(
      "encoder_text_selection_owns_keyboard_transport(app)");
  const auto normal_cursor = rotation_dispatch.find("dispatch_encoder_cursor");
  assert(drain_exit != std::string::npos);
  assert(normal_cursor != std::string::npos);
  assert(drain_exit < normal_cursor);
  assert(rotation_dispatch.find("return false;") != std::string::npos);

  const auto transport_poll = app_main.find("app.ble.poll_input_delivery(millis())");
  const auto selection_retry = app_main.find(
      "flush_encoder_text_selection(&app)", transport_poll);
  assert(transport_poll != std::string::npos);
  assert(selection_retry != std::string::npos);
  assert(transport_poll < selection_retry);

  assert(app_main.find("kReportIdMouse") == std::string::npos);
  assert(app_main.find("MouseDrag") == std::string::npos);
  assert(app_main.find("TextCaretSelectionPhase") == std::string::npos);
  assert(app_main.find("send_text_caret_selection_event") == std::string::npos);
  assert(usb.find("kAppCommandKindTextCaretSelection") == std::string::npos);
  assert(ble.find("kAppCommandKindTextCaretSelection") == std::string::npos);
  assert(usb.find("send_text_caret_selection_event") == std::string::npos);
  assert(ble.find("send_text_caret_selection_event") == std::string::npos);
  assert(usb.find("queue_mouse_drag_report_for_epoch") == std::string::npos);
  assert(ble.find("send_mouse_drag_report_for_owner") == std::string::npos);
}

void ble_persistence_is_capacity_safe_and_migrated_before_advertising() {
  const auto defaults = read_source("sdkconfig.defaults");
  const auto ble_hid = read_source("main/platform/ble_hid.cpp");
  const auto compact_ble_hid = without_ascii_whitespace(ble_hid);

  assert(defaults.find("CONFIG_BT_NIMBLE_MAX_BONDS=3") !=
         std::string::npos);
  assert(defaults.find("CONFIG_BT_NIMBLE_MAX_CCCDS=32") !=
         std::string::npos);
  assert(compact_ble_hid.find(
             "CONFIG_BT_NIMBLE_MAX_BONDS=="
             "ai_keyboard::BlePersistencePolicy::kRememberedPeers") !=
         std::string::npos);
  assert(compact_ble_hid.find(
             "ai_keyboard::BlePersistencePolicy::cccd_capacity_supports_product("
             "CONFIG_BT_NIMBLE_MAX_CCCDS)") != std::string::npos);
  assert(ble_hid.find("ble_hs_cfg.store_status_cb = product_ble_store_status") !=
         std::string::npos);
  assert(ble_hid.find("ble_store_util_status_rr") == std::string::npos);
  assert(ble_hid.find("ble_gap_unpair_oldest") == std::string::npos);
  const auto store_status = section(ble_hid,
                                    "int product_ble_store_status",
                                    "int migrate_cccds_for_schema_change");
  const auto full_preflight =
      store_status.find("event->event_code == BLE_STORE_EVENT_FULL");
  const auto allow_preflight = store_status.find("return 0;", full_preflight);
  const auto actual_overflow =
      store_status.find("event->event_code == BLE_STORE_EVENT_OVERFLOW",
                        allow_preflight);
  assert(full_preflight != std::string::npos);
  assert(allow_preflight != std::string::npos);
  assert(actual_overflow != std::string::npos);
  assert(full_preflight < allow_preflight);
  assert(allow_preflight < actual_overflow);

  const auto start_event = section(ble_hid,
                                   "case ESP_HIDD_START_EVENT:",
                                   "case ESP_HIDD_CONNECT_EVENT:");
  const auto migrate =
      start_event.find("migrate_cccds_for_schema_change()");
  const auto service_changed =
      start_event.find("ble_svc_gatt_changed(0x0001, 0xFFFF)");
  const auto persist =
      start_event.find("save_gatt_schema_revision(kGattSchemaRevision");
  const auto advertise = start_event.find("request_advertising_reconcile()");
  assert(migrate != std::string::npos);
  assert(service_changed != std::string::npos);
  assert(persist != std::string::npos);
  assert(advertise != std::string::npos);
  assert(migrate < service_changed);
  assert(service_changed < persist);
  assert(persist < advertise);
  const auto migration_body = section(ble_hid,
                                      "int migrate_cccds_for_schema_change()",
                                      "void host_task(void* param)");
  const auto mark_changed = migration_body.find("value.value_changed = 1");
  const auto checked_write =
      migration_body.find("ble_store_write_cccd(&value)", mark_changed);
  assert(mark_changed != std::string::npos);
  assert(checked_write != std::string::npos);
  assert(mark_changed < checked_write);
}

void peripheral_power_lifecycle_is_system_owned_and_ordered() {
  const auto controller =
      read_source("main/platform/peripheral_power.cpp");
  const auto controller_header =
      read_source("main/platform/peripheral_power.h");
  const auto leds =
      read_source("main/platform/led_strip_status.cpp");
  const auto leds_header =
      read_source("main/platform/led_strip_status.h");
  const auto audio_arbiter =
      read_source("components/keyboard/src/audio_io_arbiter.cpp");
  const auto keyboard_audio =
      read_source("main/platform/keyboard_audio.cpp");
  const auto app_main = read_source("main/app_main.cpp");

  assert(controller_header.find("class PeripheralPowerController") !=
         std::string::npos);
  assert(controller.find("PeripheralPowerOwner::DeviceAwake") !=
         std::string::npos);
  assert(controller.find(
             "kPeripheralPowerSettleMs = 50U") !=
         std::string::npos);

  const auto awake = section(
      controller,
      "esp_err_t PeripheralPowerController::begin_awake()",
      "bool PeripheralPowerController::ready() const");
  const auto preload_inactive =
      awake.find("write_power_enable_latch(false)");
  const auto configure = awake.find("gpio_config(&power_config)");
  const auto preserve_light_sleep = awake.find("gpio_sleep_sel_dis(");
  const auto safe_before_power = awake.find(
      "configure_command_pins_safe_for_rail_transition()");
  const auto drive_high = awake.find("apply_power_state()");
  const auto settle = awake.find(
      "vTaskDelay(peripheral_power_settle_ticks())");
  assert(preload_inactive != std::string::npos);
  assert(configure != std::string::npos);
  assert(preserve_light_sleep != std::string::npos);
  assert(safe_before_power != std::string::npos);
  assert(drive_high != std::string::npos);
  assert(settle != std::string::npos);
  assert(preload_inactive < safe_before_power);
  assert(safe_before_power < configure);
  assert(configure < preserve_light_sleep);
  assert(preserve_light_sleep < drive_high);
  assert(drive_high < settle);
  const auto ready_after_settle = awake.find("ready_ = true", settle);
  assert(ready_after_settle != std::string::npos);
  assert(settle < ready_after_settle);
  assert(awake.find("esp_rom_delay_us") == std::string::npos);

  const auto safe_pin_setup = section(
      controller,
      "PeripheralPowerController::configure_command_pins_safe_for_rail_transition()",
      "esp_err_t PeripheralPowerController::apply_power_state()");
  const auto command_latch_low = safe_pin_setup.find(
      "gpio_set_level(static_cast<gpio_num_t>(pin), 0)");
  const auto command_output_enable =
      safe_pin_setup.find("gpio_config(&output_config)");
  assert(command_latch_low != std::string::npos);
  assert(command_output_enable != std::string::npos);
  assert(command_latch_low < command_output_enable);

  const auto settle_ticks = section(
      controller,
      "constexpr TickType_t peripheral_power_settle_ticks()",
      "constexpr std::array<std::int8_t, 6> kSharedRailCommandOutputPins");
  assert(settle_ticks.find("configTICK_RATE_HZ") != std::string::npos);
  assert(settle_ticks.find("kMillisecondsPerSecond - 1U") !=
         std::string::npos);
  assert(settle_ticks.find("rounded_up_ticks + 1U") != std::string::npos);

  assert(leds.find("kPeripheralPowerEnablePin") == std::string::npos);
  assert(leds.find("set_peripheral_power_enabled") == std::string::npos);
  assert(leds.find("PeripheralPowerLeaseSet") == std::string::npos);
  assert(leds_header.find("set_audio_power_hold") == std::string::npos);
  assert(leds_header.find("set_speaker_power_hold") == std::string::npos);
  const auto led_prepare = section(
      leds,
      "esp_err_t StatusLedStrip::prepare_for_deep_sleep()",
      "void StatusLedStrip::set_agent_status(");
  const auto black_flush = led_prepare.find("const esp_err_t err = flush()");
  const auto redraw_idle =
      led_prepare.find("idle_rendered_ = false", black_flush);
  const auto redraw_agent =
      led_prepare.find("agent_status_rendered_ = false", black_flush);
  assert(black_flush != std::string::npos);
  assert(redraw_idle != std::string::npos);
  assert(redraw_agent != std::string::npos);
  assert(black_flush < redraw_idle);
  assert(redraw_idle < redraw_agent);

  std::size_t gpio8_writer_count = 0;
  const std::array<const char*, 2> source_roots{{"main", "components"}};
  for (const auto* relative_root : source_roots) {
    const auto root = std::filesystem::path(EASY_INPUT_REPO_ROOT) /
                      relative_root;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto extension = entry.path().extension().string();
      if (extension != ".cpp" && extension != ".h" && extension != ".c") {
        continue;
      }
      std::ifstream input(entry.path());
      std::ostringstream contents;
      contents << input.rdbuf();
      gpio8_writer_count += count_occurrences(
          without_ascii_whitespace(contents.str()),
          "gpio_set_level(static_cast<gpio_num_t>(ai_keyboard::kPeripheralPowerEnablePin),");
    }
  }
  assert(gpio8_writer_count == 1);

  const auto startup = section(
      app_main,
      "extern \"C\" void app_main(void)",
      "while (true) {");
  const auto power_begin =
      startup.find("app.peripheral_power.begin_awake()");
  const auto boot_summary = startup.find("log_boot_summary()");
  const auto audio_begin = startup.find("app.audio.begin()");
  const auto led_begin = startup.find("app.leds.begin()");
  assert(boot_summary != std::string::npos);
  assert(power_begin != std::string::npos);
  assert(audio_begin != std::string::npos);
  assert(led_begin != std::string::npos);
  assert(boot_summary < power_begin);
  assert(power_begin < audio_begin);
  assert(audio_begin < led_begin);

  const auto deep_sleep = section(
      app_main,
      "void maybe_enter_deep_sleep(",
      "std::int8_t wheel_chunk(");
  const auto wake_setup =
      deep_sleep.find("esp_sleep_enable_ext1_wakeup_io(");
  const auto audio_gate =
      deep_sleep.find("try_begin_audio_deep_sleep_quiesce(app)");
  const auto first_final_gate =
      deep_sleep.find("const auto final_decision");
  const auto led_quiesce =
      deep_sleep.find("app->leds.prepare_for_deep_sleep()");
  const auto commit_gate =
      deep_sleep.find("const auto commit_decision");
  const auto audio_terminal =
      deep_sleep.find("begin_audio_deep_sleep_terminal(app)", commit_gate);
  const auto ble_terminal =
      deep_sleep.find("app->ble.shutdown_for_deep_sleep()", audio_terminal);
  const auto power_off =
      deep_sleep.find("app->peripheral_power.prepare_for_deep_sleep()");
  const auto sleep_start = deep_sleep.find("esp_deep_sleep_start()");
  assert(wake_setup != std::string::npos);
  assert(audio_gate != std::string::npos);
  assert(first_final_gate != std::string::npos);
  assert(led_quiesce != std::string::npos);
  assert(commit_gate != std::string::npos);
  assert(audio_terminal != std::string::npos);
  assert(ble_terminal != std::string::npos);
  assert(power_off != std::string::npos);
  assert(sleep_start != std::string::npos);
  assert(wake_setup < audio_gate);
  assert(audio_gate < first_final_gate);
  assert(first_final_gate < led_quiesce);
  assert(led_quiesce < commit_gate);
  assert(commit_gate < audio_terminal);
  assert(audio_terminal < ble_terminal);
  assert(ble_terminal < power_off);
  assert(power_off < sleep_start);
  assert(count_occurrences(deep_sleep, "app->inputs.activity_pending()") >=
         2);
  assert(deep_sleep.find("cancel_audio_deep_sleep_quiesce(app)") !=
         std::string::npos);
  assert(deep_sleep.find("esp_restart()") != std::string::npos);

  assert(audio_arbiter.find("try_begin_deep_sleep_quiesce()") !=
         std::string::npos);
  assert(audio_arbiter.find("begin_deep_sleep_terminal()") !=
         std::string::npos);
  assert(audio_arbiter.find("try_enter_runtime_transition()") !=
         std::string::npos);
  const auto runtime_admission = section(
      audio_arbiter,
      "bool AudioIoArbiter::try_enter_runtime_transition()",
      "void AudioIoArbiter::leave_runtime_transition()");
  const auto terminal_only =
      runtime_admission.find("(state & kDeepSleepTerminalBit) == 0");
  const auto interruption =
      runtime_admission.find("admitted_state |= kDeepSleepInterruptedBit");
  const auto owner_wake = runtime_admission.find("notify_work_ready()", interruption);
  assert(terminal_only != std::string::npos);
  assert(interruption != std::string::npos);
  assert(owner_wake != std::string::npos);
  assert(terminal_only < interruption);
  assert(interruption < owner_wake);
  assert(keyboard_audio.find(
             "!audio_io_arbiter_->request_microphone(generation)") !=
         std::string::npos);
  assert(keyboard_audio.find("deep_sleep_terminal") !=
         std::string::npos);
  assert(keyboard_audio.find("deep_sleep_quiescing") ==
         std::string::npos);
  assert(app_main.find(
             "app->audio_io_arbiter.microphone_generation() != 0") !=
         std::string::npos);
  assert(app_main.find(
             "app->audio_io_arbiter.deep_sleep_quiesce_interrupted()") !=
         std::string::npos);
  assert(app_main.find(
             "app.audio_io_arbiter.set_work_ready_callback(signal_async_work, &app)") !=
         std::string::npos);

  const auto power_down = section(
      controller,
      "esp_err_t PeripheralPowerController::prepare_for_deep_sleep()",
      "esp_err_t PeripheralPowerController::set_owner_hold(");
  const auto safe_outputs =
      power_down.find("configure_command_pins_safe_for_rail_transition()");
  const auto clear_awake = power_down.find("power_leases_.clear()");
  const auto rail_low = power_down.find("apply_power_state()");
  assert(safe_outputs != std::string::npos);
  assert(clear_awake != std::string::npos);
  assert(rail_low != std::string::npos);
  assert(safe_outputs < clear_awake);
  assert(clear_awake < rail_low);
  assert(power_down.find("ready_ = false") < safe_outputs);
  assert(power_down.find("DeviceAwake", rail_low) == std::string::npos);

  assert(app_main.find("esp_reset_reason()") != std::string::npos);
  assert(app_main.find("reset_reason=%d brownout=%d") != std::string::npos);
  assert(app_main.find("0.4.53-idf-v2-ble-store-v1") !=
         std::string::npos);
}

void runtime_logs_do_not_emit_user_or_device_identifiers() {
  const auto audio = read_source("main/platform/keyboard_audio.cpp");
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto usb = read_source("main/platform/usb_hid.cpp");
  const auto app = read_source("main/app_main.cpp");

  for (const auto* forbidden : {
           "ssid=%s",
           "host=%s",
           "peer=%s",
           "peer_id=%s",
           "addr=%s",
           "source_mac=%02X",
           "value=%s",
           "ptt_hotkey=%s",
           "edit_ptt_hotkey=%s",
           "hotkey %s %s",
       }) {
    assert(audio.find(forbidden) == std::string::npos);
    assert(ble.find(forbidden) == std::string::npos);
    assert(usb.find(forbidden) == std::string::npos);
    assert(app.find(forbidden) == std::string::npos);
  }
}

void host_action_uses_shared_encoding_and_existing_single_channel_route() {
  const auto app = read_source("main/app_main.cpp");
  const auto usb = read_source("main/platform/usb_hid.cpp");
  const auto ble = read_source("main/platform/ble_hid.cpp");

  const auto dispatch = section(app,
                                "void dispatch_firmware_event",
                                "void dispatch_encoder_press_click");
  assert(dispatch.find(
             "case ai_keyboard::FirmwareEventKind::HostAction:") !=
         std::string::npos);
  assert(dispatch.find("kind = \"host_action\"") != std::string::npos);
  const auto usb_attempt = dispatch.find(
      "app->usb.send_firmware_event_for_epoch(source, event, usb_epoch)");
  const auto suppress_fallback = dispatch.find("BLE fallback suppressed");
  const auto usb_return = dispatch.find("return;", suppress_fallback);
  const auto ble_without_usb = dispatch.find(
      "app->ble.send_firmware_event(source, event)", usb_return);
  assert(usb_attempt != std::string::npos);
  assert(suppress_fallback != std::string::npos);
  assert(usb_return != std::string::npos);
  assert(ble_without_usb != std::string::npos);
  assert(usb_attempt < suppress_fallback);
  assert(suppress_fallback < usb_return);
  assert(usb_return < ble_without_usb);

  const auto usb_send = section(
      usb,
      "bool UsbHidTransport::send_firmware_event_for_epoch",
      "bool UsbHidTransport::tap_hotkey");
  const auto ble_send = section(
      ble,
      "bool BleHidTransport::send_firmware_event_for_owner",
      "bool BleHidTransport::send_keyboard_report");
  for (const auto* adapter : {&usb_send, &ble_send}) {
    assert(adapter->find(
               "case ai_keyboard::FirmwareEventKind::HostAction:") !=
           std::string::npos);
    assert(count_occurrences(*adapter, "encode_host_action_v1") == 1);
    assert(adapter->find("report.payload[0]") != std::string::npos);
    assert(adapter->find("report.payload[1]") != std::string::npos);
    assert(adapter->find("report.payload[2]") != std::string::npos);
    assert(adapter->find("report.payload[3]") != std::string::npos);
    assert(adapter->find("send_app_command_report(") != std::string::npos);
    assert(adapter->find("kAppCommandKindHostAction") == std::string::npos);
    assert(adapter->find("0x05") == std::string::npos);
    assert(adapter->find("36") == std::string::npos);
    assert(adapter->find("48") == std::string::npos);
  }

  assert(usb.find("ai_keyboard::kHostActionV1ReportId") !=
         std::string::npos);
  assert(ble.find("ai_keyboard::kHostActionV1ReportId") !=
         std::string::npos);

  const auto usb_app_command = section(
      usb,
      "bool UsbHidTransport::push_app_command_report_locked",
      "bool UsbHidTransport::send_fixed_text_command");
  const auto ble_app_command = section(
      ble,
      "bool BleHidTransport::send_app_command_report",
      "bool BleHidTransport::send_fixed_text_command");
  for (const auto* builder : {&usb_app_command, &ble_app_command}) {
    assert(builder->find(
               "std::array<std::uint8_t, kAppCommandReportPayloadLen> "
               "report{};") != std::string::npos);
    assert(builder->find(
               "std::copy_n(data, len, report.data() + "
               "kAppCommandHeaderLen)") != std::string::npos);
  }
}

void config_status_publication_uses_shared_bounded_ble_encoders() {
  const auto ble = read_source("main/platform/ble_hid.cpp");
  const auto publish = section(
      ble,
      "void BleHidTransport::publish_status_json",
      "bool BleHidTransport::send_firmware_event");
  assert(count_occurrences(
             publish, "ai_keyboard::kConfigStatusFallbackJson") == 2);
  assert(publish.find("wire_status.size() >") != std::string::npos);
  assert(publish.find("ai_keyboard::kConfigStatusGattSafeLen") !=
         std::string::npos);

  const auto final_wire = section(
      ble,
      "std::string BleHidTransport::status_json_for_publish",
      "bool BleHidTransport::copy_status_json_for_read");
  assert(final_wire.find("append_ble_status_wire_json") != std::string::npos);
  assert(final_wire.find("append_ble_detailed_status_wire_json") !=
         std::string::npos);
  assert(final_wire.find("std::snprintf") == std::string::npos);
}

}  // namespace

int main() {
  usb_async_work_is_durable_before_owner_notification();
  power_telemetry_reports_awake_and_real_deep_sleep_facts_only();
  battery_update_releases_hidd_lock_before_entering_nimble();
  gatt_status_read_callback_is_cache_only_and_refreshes_once();
  stable_connection_reset_has_one_balanced_critical_section();
  stable_connection_update_failures_share_bounded_backoff();
  ble_notification_submission_gets_awake_grace_before_deep_sleep();
  speaker_asset_store_priority_ignores_ble_sleep_grace();
  ble_management_publication_and_deep_sleep_are_ordered();
  config_fragments_use_exact_connection_lifetimes();
  ble_status_refresh_publishes_current_config_fingerprint();
  speaker_probe_status_uses_one_generation_for_usb_and_ble();
  release_build_rejects_internal_ram_profile_drift();
  audio_start_reuses_boot_resource_pool();
  speaker_probe_is_default_off_and_outside_input_hot_path();
  speaker_opus_probe_is_conditional_fixed_and_reusable();
  speaker_ima_probe_is_conditional_allocation_free_and_isolated();
  speaker_asset_storage_reserves_exact_banks_and_has_production_gate();
  speaker_asset_usb_lifetime_precedes_first_frame();
  speaker_asset_wifi_carrier_is_the_only_wireless_bulk_path();
  awake_owner_waits_for_notifications_and_business_deadlines();
  speaker_boot_worker_completion_reenters_the_owner_without_other_events();
  speaker_shutdown_cleanup_has_a_bounded_owner_retry();
  speaker_assets_mailbox_and_core_publish_durable_owner_work();
  v2_cold_boot_feedback_starts_from_the_first_pcm_event();
  speaker_asset_wifi_admission_owns_ingress_power_and_listener_epoch();
  speaker_asset_endpoint_accept_retires_before_lifetime_unlock();
  speaker_asset_boot_read_uses_the_protocol_store_runner();
  encoder_text_caret_selection_uses_native_keyboard_chords();
  ble_persistence_is_capacity_safe_and_migrated_before_advertising();
  peripheral_power_lifecycle_is_system_owned_and_ordered();
  runtime_logs_do_not_emit_user_or_device_identifiers();
  host_action_uses_shared_encoding_and_existing_single_channel_route();
  config_status_publication_uses_shared_bounded_ble_encoders();
  return 0;
}
