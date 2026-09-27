#include <cassert>
#include <cstring>

#include "keyboard/power_cycle.h"

namespace {

void records_observed_awake_inactivity_without_synthetic_stages() {
  const auto snapshot = ai_keyboard::build_power_cycle_snapshot(
      3,
      12'345,
      ai_keyboard::PowerCycleWakeReason::Input,
      false);

  assert(snapshot.valid());
  assert(snapshot.sequence == 3);
  assert(snapshot.inactive_ms == 12'345);
  assert(!snapshot.reached_deep_sleep);
  assert(snapshot.wake_reason == ai_keyboard::PowerCycleWakeReason::Input);
}

void records_only_a_real_deep_sleep_commit() {
  const auto awake = ai_keyboard::build_power_cycle_snapshot(
      4,
      30U * 60U * 1000U - 1U,
      ai_keyboard::PowerCycleWakeReason::Timer,
      false);
  assert(!awake.reached_deep_sleep);

  const auto committed = ai_keyboard::build_power_cycle_snapshot(
      5,
      30U * 60U * 1000U,
      ai_keyboard::PowerCycleWakeReason::Unknown,
      true);
  assert(committed.valid());
  assert(committed.sequence == 5);
  assert(committed.inactive_ms == 30U * 60U * 1000U);
  assert(committed.reached_deep_sleep);
}

void zero_sequence_is_normalized_without_discarding_short_cycles() {
  const auto snapshot = ai_keyboard::build_power_cycle_snapshot(
      0,
      0,
      ai_keyboard::PowerCycleWakeReason::ExternalPower,
      false);
  assert(snapshot.valid());
  assert(snapshot.sequence == 1);
  assert(snapshot.inactive_ms == 0);
  assert(snapshot.wake_reason ==
         ai_keyboard::PowerCycleWakeReason::ExternalPower);
}

void wake_reasons_exclude_retired_manual_light_sleep() {
  using ai_keyboard::PowerCycleWakeReason;

  assert(ai_keyboard::power_cycle_wake_reason("input_edge") ==
         PowerCycleWakeReason::Input);
  assert(ai_keyboard::power_cycle_wake_reason("deep_sleep_key_wake") ==
         PowerCycleWakeReason::DeepSleepKey);
  assert(ai_keyboard::power_cycle_wake_reason("status_read") ==
         PowerCycleWakeReason::StatusRead);
  assert(ai_keyboard::power_cycle_wake_reason("light_sleep_key_wake") ==
         PowerCycleWakeReason::Other);
  assert(ai_keyboard::power_cycle_wake_reason("unknown") ==
         PowerCycleWakeReason::Unknown);
  assert(ai_keyboard::power_cycle_wake_reason("unexpected") ==
         PowerCycleWakeReason::Other);
  assert(std::strcmp(
             ai_keyboard::power_cycle_wake_reason_name(
                 PowerCycleWakeReason::WifiAudio),
             "wifi_audio") == 0);
  assert(std::strcmp(
             ai_keyboard::power_cycle_wake_reason_name(
                 PowerCycleWakeReason::DeepSleepKey),
             "deep_key") == 0);
}

}  // namespace

int main() {
  records_observed_awake_inactivity_without_synthetic_stages();
  records_only_a_real_deep_sleep_commit();
  zero_sequence_is_normalized_without_discarding_short_cycles();
  wake_reasons_exclude_retired_manual_light_sleep();
  return 0;
}
