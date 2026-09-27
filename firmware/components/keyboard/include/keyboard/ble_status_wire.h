#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "keyboard/config_status.h"

namespace ai_keyboard {

struct BleStatusWireSnapshot {
  bool connected = false;
  bool parameters_valid = false;
  std::uint16_t interval = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
};

struct BleDetailedStatusWireSnapshot {
  bool connected = false;
  bool update_in_flight = false;
  std::uint16_t connection_handle = 0;
  bool parameters_valid = false;
  std::uint16_t interval = 0;
  std::uint16_t latency = 0;
  std::uint16_t supervision_timeout = 0;
  std::int32_t update_status = 0;
  std::uint32_t queued_reports = 0;
  std::uint32_t enqueued_reports = 0;
  std::uint32_t transmitted_reports = 0;
  std::uint32_t dropped_reports = 0;
  std::uint32_t retryable_reports = 0;
  std::uint32_t hid_queue_high_watermark = 0;
  std::uint32_t queued_wheel_reports = 0;
  std::uint32_t enqueued_wheel_reports = 0;
  std::uint32_t coalesced_wheel_reports = 0;
  std::uint32_t transmitted_wheel_reports = 0;
  std::uint32_t dropped_wheel_reports = 0;
};

inline std::string append_ble_status_wire_json(
    std::string status_json,
    const BleStatusWireSnapshot& snapshot) {
  if (status_json.size() < 2 || status_json.front() != '{' ||
      status_json.back() != '}') {
    return status_json;
  }

  std::array<char, kConfigStatusBatteryBleReserveLen> fragment{};
  const int fragment_len = std::snprintf(
      fragment.data(),
      fragment.size(),
      ",\"ble\":{\"connected\":%u,\"valid\":%u,\"itvl\":%u,\"latency\":%u,\"timeout\":%u}",
      snapshot.connected ? 1U : 0U,
      snapshot.parameters_valid ? 1U : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.interval) : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.latency) : 0U,
      snapshot.parameters_valid
          ? static_cast<unsigned>(snapshot.supervision_timeout)
          : 0U);
  if (fragment_len <= 0 ||
      static_cast<std::size_t>(fragment_len) >= fragment.size() ||
      status_json.size() + static_cast<std::size_t>(fragment_len) >
          kConfigStatusGattSafeLen) {
    return status_json;
  }

  status_json.insert(status_json.size() - 1,
                     fragment.data(),
                     static_cast<std::size_t>(fragment_len));
  return status_json;
}

inline std::string append_ble_detailed_status_wire_json(
    std::string status_json,
    const BleDetailedStatusWireSnapshot& snapshot) {
  if (status_json.size() < 2 || status_json.front() != '{' ||
      status_json.back() != '}') {
    return status_json;
  }

  std::array<char, 448> fragment{};
  const int fragment_len = std::snprintf(
      fragment.data(),
      fragment.size(),
      ",\"ble\":{\"connected\":%u,\"pending\":%u,\"handle\":%u,\"valid\":%u,\"itvl\":%u,\"latency\":%u,\"timeout\":%u,\"upd\":%ld,\"hidq\":%u,\"hid_enq\":%lu,\"hid_tx\":%lu,\"hid_drop\":%lu,\"hid_retry\":%lu,\"hid_hwm\":%lu,\"wheelq\":%u,\"wheel_enq\":%lu,\"wheel_merge\":%lu,\"wheel_tx\":%lu,\"wheel_drop\":%lu}",
      snapshot.connected ? 1U : 0U,
      snapshot.update_in_flight ? 1U : 0U,
      snapshot.connected ? static_cast<unsigned>(snapshot.connection_handle) : 0U,
      snapshot.parameters_valid ? 1U : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.interval) : 0U,
      snapshot.parameters_valid ? static_cast<unsigned>(snapshot.latency) : 0U,
      snapshot.parameters_valid
          ? static_cast<unsigned>(snapshot.supervision_timeout)
          : 0U,
      static_cast<long>(snapshot.update_status),
      static_cast<unsigned>(snapshot.queued_reports),
      static_cast<unsigned long>(snapshot.enqueued_reports),
      static_cast<unsigned long>(snapshot.transmitted_reports),
      static_cast<unsigned long>(snapshot.dropped_reports),
      static_cast<unsigned long>(snapshot.retryable_reports),
      static_cast<unsigned long>(snapshot.hid_queue_high_watermark),
      static_cast<unsigned>(snapshot.queued_wheel_reports),
      static_cast<unsigned long>(snapshot.enqueued_wheel_reports),
      static_cast<unsigned long>(snapshot.coalesced_wheel_reports),
      static_cast<unsigned long>(snapshot.transmitted_wheel_reports),
      static_cast<unsigned long>(snapshot.dropped_wheel_reports));
  if (fragment_len <= 0 ||
      static_cast<std::size_t>(fragment_len) >= fragment.size() ||
      status_json.size() + static_cast<std::size_t>(fragment_len) >
          kConfigStatusGattSafeLen) {
    return status_json;
  }

  status_json.insert(status_json.size() - 1,
                     fragment.data(),
                     static_cast<std::size_t>(fragment_len));
  return status_json;
}

}  // namespace ai_keyboard
