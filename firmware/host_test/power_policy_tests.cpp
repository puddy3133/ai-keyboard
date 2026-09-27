#include <cassert>
#include <string_view>

#include "keyboard/power_policy.h"

namespace {

ai_keyboard::PowerPolicyInputs ready_inputs(std::uint32_t now_ms) {
  ai_keyboard::PowerPolicyInputs inputs;
  inputs.now_ms = now_ms;
  inputs.last_user_activity_ms = 0;
  inputs.wake_source_configured = true;
  inputs.key_wake_verified = true;
  return inputs;
}

void only_true_deep_sleep_has_a_time_threshold() {
  auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms - 1U);
  auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(!decision.deep_sleep_allowed);
  assert(std::string_view(decision.deep_sleep_block) == "user_active");

  inputs.now_ms = ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(decision.deep_sleep_allowed);
  assert(std::string_view(decision.deep_sleep_block) == "ok");
}

void physical_and_in_flight_work_block_the_commit() {
  auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms);

  inputs.input_active = true;
  auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "input");

  inputs.input_active = false;
  inputs.management_work_pending = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "management_work");

  inputs.management_work_pending = false;
  inputs.hid_work_pending = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "hid_work");

  inputs.hid_work_pending = false;
  inputs.audio_streaming = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "keyboard_mic");
}

void every_mutable_commit_blocker_is_fail_closed() {
  struct BlockerCase {
    bool ai_keyboard::PowerPolicyInputs::*member;
    const char* reason;
  };
  constexpr BlockerCase cases[] = {
      {&ai_keyboard::PowerPolicyInputs::external_power, "external_power"},
      {&ai_keyboard::PowerPolicyInputs::usb_mounted, "usb_hid"},
      {&ai_keyboard::PowerPolicyInputs::config_window_active,
       "config_window"},
      {&ai_keyboard::PowerPolicyInputs::input_active, "input"},
      {&ai_keyboard::PowerPolicyInputs::encoder_press_pending,
       "encoder_press"},
      {&ai_keyboard::PowerPolicyInputs::wheel_report_pending,
       "wheel_queue"},
      {&ai_keyboard::PowerPolicyInputs::management_work_pending,
       "management_work"},
      {&ai_keyboard::PowerPolicyInputs::hid_work_pending, "hid_work"},
      {&ai_keyboard::PowerPolicyInputs::audio_streaming, "keyboard_mic"},
      {&ai_keyboard::PowerPolicyInputs::speaker_playback_active,
       "speaker_playback"},
      {&ai_keyboard::PowerPolicyInputs::key_wake_asserted, "key_wake_low"},
      {&ai_keyboard::PowerPolicyInputs::wifi_active, "wifi_control"},
  };

  for (const auto& test_case : cases) {
    auto inputs = ready_inputs(
        ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms);
    inputs.*(test_case.member) = true;
    const auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
    assert(!decision.deep_sleep_allowed);
    assert(std::string_view(decision.deep_sleep_block) == test_case.reason);
  }
}

void background_management_does_not_change_the_user_clock() {
  auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms + 8'000U);
  inputs.last_user_activity_ms = 8'000U;
  auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(decision.deep_sleep_allowed);

  inputs.management_work_pending = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(!decision.deep_sleep_allowed);
  inputs.management_work_pending = false;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(decision.deep_sleep_allowed);
}

void connected_ble_is_not_a_permanent_sleep_blocker() {
  // There is intentionally no ble_connected input. Only concrete pending HID,
  // GATT or management work can block the transactional transition.
  const auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms);
  assert(ai_keyboard::evaluate_deep_sleep_policy(inputs).deep_sleep_allowed);
}

void wifi_release_is_requested_only_after_every_earlier_gate_passes() {
  auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms);
  inputs.wifi_active = true;
  auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(!decision.deep_sleep_allowed);
  assert(decision.wifi_release_required);
  assert(std::string_view(decision.deep_sleep_block) == "wifi_control");

  inputs.management_work_pending = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(!decision.wifi_release_required);
  assert(std::string_view(decision.deep_sleep_block) == "management_work");
}

void wake_source_and_power_gates_fail_closed() {
  auto inputs = ready_inputs(
      ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms);
  inputs.wake_source_configured = false;
  auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "not_configured");

  inputs.wake_source_configured = true;
  inputs.key_wake_verified = false;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) ==
         "key_wake_unverified");

  inputs.key_wake_verified = true;
  inputs.external_power = true;
  decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(std::string_view(decision.deep_sleep_block) == "external_power");
}

void elapsed_time_handles_uptime_wraparound() {
  auto inputs = ready_inputs(100U);
  inputs.last_user_activity_ms = 0xFFFF'FF00U;
  const auto decision = ai_keyboard::evaluate_deep_sleep_policy(inputs);
  assert(!decision.deep_sleep_allowed);
  assert(std::string_view(decision.deep_sleep_block) == "user_active");
}

void wifi_power_stages_only_release_for_coordinated_deep_sleep() {
  using ai_keyboard::WifiPowerStage;
  assert(ai_keyboard::evaluate_wifi_power_stage(119'999, false, false) ==
         WifiPowerStage::Active);
  assert(ai_keyboard::evaluate_wifi_power_stage(120'000, false, false) ==
         WifiPowerStage::Throttled);
  assert(ai_keyboard::evaluate_wifi_power_stage(1'800'000, false, true) ==
         WifiPowerStage::Released);
  assert(ai_keyboard::evaluate_wifi_power_stage(1'800'000, true, true) ==
         WifiPowerStage::Active);
}

void wifi_release_waits_for_a_quiet_control_window() {
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(4'999, false, true));
  assert(ai_keyboard::wifi_release_ready_for_deep_sleep(5'000, false, true));
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(30'000, true, true));
  assert(!ai_keyboard::wifi_release_ready_for_deep_sleep(30'000, false, false));
}

}  // namespace

int main() {
  only_true_deep_sleep_has_a_time_threshold();
  physical_and_in_flight_work_block_the_commit();
  every_mutable_commit_blocker_is_fail_closed();
  background_management_does_not_change_the_user_clock();
  connected_ble_is_not_a_permanent_sleep_blocker();
  wifi_release_is_requested_only_after_every_earlier_gate_passes();
  wake_source_and_power_gates_fail_closed();
  elapsed_time_handles_uptime_wraparound();
  wifi_power_stages_only_release_for_coordinated_deep_sleep();
  wifi_release_waits_for_a_quiet_control_window();
  return 0;
}
