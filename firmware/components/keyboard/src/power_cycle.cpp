#include "keyboard/power_cycle.h"

#include <cstring>

namespace ai_keyboard {

PowerCycleSnapshot build_power_cycle_snapshot(
    std::uint32_t sequence,
    std::uint32_t inactive_ms,
    PowerCycleWakeReason wake_reason,
    bool reached_deep_sleep) {
  PowerCycleSnapshot snapshot{};
  snapshot.sequence = sequence == 0 ? 1 : sequence;
  snapshot.inactive_ms = inactive_ms;
  snapshot.reached_deep_sleep = reached_deep_sleep;
  snapshot.wake_reason = wake_reason;
  return snapshot;
}

PowerCycleWakeReason power_cycle_wake_reason(const char* reason) {
  if (reason == nullptr || *reason == '\0') {
    return PowerCycleWakeReason::Unknown;
  }
  if (std::strcmp(reason, "unknown") == 0) {
    return PowerCycleWakeReason::Unknown;
  }
  if (std::strcmp(reason, "input") == 0 || std::strcmp(reason, "input_edge") == 0) {
    return PowerCycleWakeReason::Input;
  }
  if (std::strcmp(reason, "key_wake") == 0) {
    return PowerCycleWakeReason::Key;
  }
  if (std::strcmp(reason, "deep_sleep_key_wake") == 0) {
    return PowerCycleWakeReason::DeepSleepKey;
  }
  if (std::strcmp(reason, "status_read") == 0) {
    return PowerCycleWakeReason::StatusRead;
  }
  if (std::strcmp(reason, "config") == 0) {
    return PowerCycleWakeReason::Config;
  }
  if (std::strcmp(reason, "wifi_audio_control") == 0) {
    return PowerCycleWakeReason::WifiAudio;
  }
  if (std::strcmp(reason, "timer") == 0) {
    return PowerCycleWakeReason::Timer;
  }
  if (std::strcmp(reason, "external_power") == 0) {
    return PowerCycleWakeReason::ExternalPower;
  }
  return PowerCycleWakeReason::Other;
}

const char* power_cycle_wake_reason_name(PowerCycleWakeReason reason) {
  switch (reason) {
    case PowerCycleWakeReason::Input:
      return "input";
    case PowerCycleWakeReason::Key:
      return "key";
    case PowerCycleWakeReason::DeepSleepKey:
      return "deep_key";
    case PowerCycleWakeReason::StatusRead:
      return "status";
    case PowerCycleWakeReason::Config:
      return "config";
    case PowerCycleWakeReason::WifiAudio:
      return "wifi_audio";
    case PowerCycleWakeReason::Timer:
      return "timer";
    case PowerCycleWakeReason::ExternalPower:
      return "power";
    case PowerCycleWakeReason::Other:
      return "other";
    case PowerCycleWakeReason::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace ai_keyboard
