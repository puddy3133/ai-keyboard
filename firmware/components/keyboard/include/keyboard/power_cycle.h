#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class PowerCycleWakeReason : std::uint8_t {
  Unknown = 0,
  Input,
  Key,
  DeepSleepKey,
  StatusRead,
  Config,
  WifiAudio,
  Timer,
  ExternalPower,
  Other,
};

struct PowerCycleSnapshot {
  std::uint32_t sequence = 0;
  // Total time since the last confirmed user activity. This is an observed
  // duration, not a synthetic Active/Idle/DeepIdle stage split.
  std::uint32_t inactive_ms = 0;
  bool reached_deep_sleep = false;
  PowerCycleWakeReason wake_reason = PowerCycleWakeReason::Unknown;

  bool valid() const { return sequence != 0; }
};

PowerCycleSnapshot build_power_cycle_snapshot(
    std::uint32_t sequence,
    std::uint32_t inactive_ms,
    PowerCycleWakeReason wake_reason,
    bool reached_deep_sleep);

PowerCycleWakeReason power_cycle_wake_reason(const char* reason);
const char* power_cycle_wake_reason_name(PowerCycleWakeReason reason);

}  // namespace ai_keyboard
