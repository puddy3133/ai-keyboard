#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class WifiPowerStage : std::uint8_t {
  Active,
  Throttled,
  Released,
};

struct PowerPolicyConfig {
  std::uint32_t deep_sleep_after_ms = 30 * 60 * 1000;
  std::uint32_t wifi_throttle_after_ms = 2 * 60 * 1000;
  // Matches the App's keyboard-microphone cold-start wait. Once whole-device
  // deep sleep is requested, keep the control socket reachable for this quiet
  // window so an in-flight PTT/config packet wins over shutdown.
  std::uint32_t wifi_release_quiesce_ms = 5 * 1000;
};

inline constexpr PowerPolicyConfig kDefaultPowerPolicy{};

struct PowerPolicyInputs {
  std::uint32_t now_ms = 0;
  std::uint32_t last_user_activity_ms = 0;
  bool external_power = false;
  bool input_active = false;
  bool usb_mounted = false;
  bool config_window_active = false;
  bool encoder_press_pending = false;
  bool wheel_report_pending = false;
  bool management_work_pending = false;
  bool hid_work_pending = false;
  bool audio_streaming = false;
  bool speaker_playback_active = false;
  bool wifi_active = false;
  bool wake_source_configured = false;
  bool key_wake_verified = false;
  bool key_wake_asserted = false;
};

struct PowerPolicyDecision {
  bool deep_sleep_allowed = false;
  bool wifi_release_required = false;
  const char* deep_sleep_block = "user_active";
};

// Awake is one product state. This policy decides only whether the owner task
// may begin the transactional transition into true ESP32 deep sleep.
PowerPolicyDecision evaluate_deep_sleep_policy(
    const PowerPolicyInputs& inputs,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

WifiPowerStage evaluate_wifi_power_stage(
    std::uint32_t inactive_ms,
    bool session_active,
    bool release_requested,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

bool wifi_release_ready_for_deep_sleep(
    std::uint32_t request_elapsed_ms,
    bool session_active,
    bool release_requested,
    const PowerPolicyConfig& config = kDefaultPowerPolicy);

const char* wifi_power_stage_name(WifiPowerStage stage);

}  // namespace ai_keyboard
