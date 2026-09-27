#include "keyboard/power_policy.h"

#include <string_view>

namespace ai_keyboard {
namespace {

const char* deep_sleep_block_reason(const PowerPolicyInputs& inputs,
                                    std::uint32_t inactive_ms,
                                    const PowerPolicyConfig& config) {
  if (!inputs.wake_source_configured) return "not_configured";
  if (!inputs.key_wake_verified) return "key_wake_unverified";
  if (inactive_ms < config.deep_sleep_after_ms) return "user_active";
  if (inputs.external_power) return "external_power";
  if (inputs.usb_mounted) return "usb_hid";
  if (inputs.config_window_active) return "config_window";
  if (inputs.input_active) return "input";
  if (inputs.encoder_press_pending) return "encoder_press";
  if (inputs.wheel_report_pending) return "wheel_queue";
  if (inputs.management_work_pending) return "management_work";
  if (inputs.hid_work_pending) return "hid_work";
  if (inputs.audio_streaming) return "keyboard_mic";
  if (inputs.speaker_playback_active) return "speaker_playback";
  if (inputs.key_wake_asserted) return "key_wake_low";
  if (inputs.wifi_active) return "wifi_control";
  return "ok";
}

}  // namespace

PowerPolicyDecision evaluate_deep_sleep_policy(
    const PowerPolicyInputs& inputs,
    const PowerPolicyConfig& config) {
  const std::uint32_t inactive_ms =
      inputs.now_ms - inputs.last_user_activity_ms;
  PowerPolicyDecision decision;
  decision.deep_sleep_block =
      deep_sleep_block_reason(inputs, inactive_ms, config);
  decision.deep_sleep_allowed =
      std::string_view(decision.deep_sleep_block) == "ok";
  decision.wifi_release_required =
      std::string_view(decision.deep_sleep_block) == "wifi_control";
  return decision;
}

WifiPowerStage evaluate_wifi_power_stage(std::uint32_t inactive_ms,
                                         bool session_active,
                                         bool release_requested,
                                         const PowerPolicyConfig& config) {
  if (session_active || inactive_ms < config.wifi_throttle_after_ms) {
    return WifiPowerStage::Active;
  }
  if (release_requested) {
    return WifiPowerStage::Released;
  }
  return WifiPowerStage::Throttled;
}

bool wifi_release_ready_for_deep_sleep(std::uint32_t request_elapsed_ms,
                                       bool session_active,
                                       bool release_requested,
                                       const PowerPolicyConfig& config) {
  return release_requested && !session_active &&
         request_elapsed_ms >= config.wifi_release_quiesce_ms;
}

const char* wifi_power_stage_name(WifiPowerStage stage) {
  switch (stage) {
    case WifiPowerStage::Active:
      return "active";
    case WifiPowerStage::Throttled:
      return "throttled";
    case WifiPowerStage::Released:
      return "released";
  }
  return "unknown";
}

}  // namespace ai_keyboard
