#pragma once

#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

// This is a product capacity contract, not a copy of NimBLE defaults.
// Each remembered host can persist one CCCD for every notify / indicate
// characteristic in the current GATT database. One additional complete host
// subscription set is reserved for schema migration and transient cleanup.
struct BlePersistencePolicy {
  static constexpr std::size_t kRememberedPeers = 3;
  static constexpr std::size_t kServiceChangedSubscriptions = 1;
  static constexpr std::size_t kScanRefreshSubscriptions = 1;
  static constexpr std::size_t kHidReportInputSubscriptions = 3;
  static constexpr std::size_t kHidBootInputSubscriptions = 2;
  static constexpr std::size_t kConfigStatusSubscriptions = 1;
  static constexpr std::size_t kPersistentSubscriptionsPerPeer =
      kServiceChangedSubscriptions + kScanRefreshSubscriptions +
      kHidReportInputSubscriptions + kHidBootInputSubscriptions +
      kConfigStatusSubscriptions;
  static constexpr std::size_t kMigrationReserve =
      kPersistentSubscriptionsPerPeer;
  static constexpr std::size_t kRequiredCccdCapacity =
      kRememberedPeers * kPersistentSubscriptionsPerPeer +
      kMigrationReserve;

  static constexpr bool cccd_capacity_supports_product(
      std::size_t capacity) {
    return capacity >= kRequiredCccdCapacity;
  }

  static constexpr bool preserve_cccd_during_schema_migration(
      std::uint16_t characteristic_value_handle,
      std::uint16_t service_changed_value_handle) {
    return service_changed_value_handle != 0 &&
           characteristic_value_handle == service_changed_value_handle;
  }
};

enum class BleStorePressureObject : std::uint8_t {
  Cccd,
  SecurityOrIdentity,
  Other,
};

enum class BleStorePressureAction : std::uint8_t {
  ReclaimOrphanCccdOnly,
  RejectWithoutDeletingBond,
};

constexpr BleStorePressureAction ble_store_pressure_action(
    BleStorePressureObject object) {
  return object == BleStorePressureObject::Cccd
             ? BleStorePressureAction::ReclaimOrphanCccdOnly
             : BleStorePressureAction::RejectWithoutDeletingBond;
}

}  // namespace ai_keyboard
