#pragma once

#include <cstdint>

namespace ai_keyboard {

struct BleConnectionParameters {
  std::uint16_t interval = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
};

struct BleConnectionProfileBounds {
  std::uint16_t minimum_interval = 0;
  std::uint16_t maximum_interval = 0;
  std::uint16_t maximum_latency = 0;
  std::uint16_t minimum_supervision_timeout = 0;
};

enum class BleConnectionUpdateDisposition : std::uint8_t {
  Settled,
  RetryWithBackoff,
};

constexpr bool ble_connection_parameters_match(
    const BleConnectionParameters& actual,
    const BleConnectionProfileBounds& expected) {
  return actual.interval >= expected.minimum_interval &&
         actual.interval <= expected.maximum_interval &&
         actual.latency <= expected.maximum_latency &&
         actual.supervision_timeout >= expected.minimum_supervision_timeout;
}

constexpr BleConnectionUpdateDisposition classify_ble_connection_update(
    bool update_succeeded,
    bool actual_parameters_match) {
  if (update_succeeded && actual_parameters_match) {
    return BleConnectionUpdateDisposition::Settled;
  }
  return BleConnectionUpdateDisposition::RetryWithBackoff;
}

constexpr std::int64_t ble_connection_update_retry_delay_us(
    std::uint8_t retry_attempt) {
  constexpr std::int64_t kInitialDelayUs = 500LL * 1000;
  constexpr std::uint8_t kMaximumShift = 4;
  const auto shift =
      retry_attempt < kMaximumShift ? retry_attempt : kMaximumShift;
  return kInitialDelayUs << shift;
}

// A successful NimBLE notification call only proves that the packet was
// accepted by the host stack. Keep the radio alive across at least two
// connection events before a whole-device deep-sleep commit. This is a
// conservative handoff grace, not a controller-TX-empty or peer-ACK fence.
// The requested product profile is not proof of what the central negotiated;
// when the actual interval is unknown, use Bluetooth LE's legal 4 s maximum.
constexpr std::uint64_t ble_tx_grace_window_us(
    std::uint16_t actual_interval,
    bool actual_interval_valid) {
  constexpr std::uint16_t kMinimumIntervalUnits = 6;     // 7.5 ms
  constexpr std::uint16_t kMaximumIntervalUnits = 3200;  // 4 s
  constexpr std::uint16_t kFallbackIntervalUnits =
      kMaximumIntervalUnits;
  constexpr std::uint64_t kIntervalUnitUs = 1250;
  constexpr std::uint64_t kConnectionEvents = 2;
  constexpr std::uint64_t kSafetyMarginUs = 10 * 1000;
  constexpr std::uint64_t kMinimumGraceUs = 100 * 1000;

  std::uint16_t interval = actual_interval_valid
                               ? actual_interval
                               : kFallbackIntervalUnits;
  if (interval < kMinimumIntervalUnits) {
    interval = kMinimumIntervalUnits;
  } else if (interval > kMaximumIntervalUnits) {
    interval = kMaximumIntervalUnits;
  }
  const auto calculated =
      static_cast<std::uint64_t>(interval) * kIntervalUnitUs *
          kConnectionEvents +
      kSafetyMarginUs;
  return calculated < kMinimumGraceUs ? kMinimumGraceUs : calculated;
}

}  // namespace ai_keyboard
