#include <cassert>
#include <cstdint>

#include "keyboard/ble_persistence_policy.h"

namespace {

void capacity_is_derived_from_the_product_contract() {
  using ai_keyboard::BlePersistencePolicy;

  static_assert(BlePersistencePolicy::kRememberedPeers == 3);
  static_assert(BlePersistencePolicy::kServiceChangedSubscriptions == 1);
  static_assert(BlePersistencePolicy::kScanRefreshSubscriptions == 1);
  static_assert(BlePersistencePolicy::kHidReportInputSubscriptions == 3);
  static_assert(BlePersistencePolicy::kHidBootInputSubscriptions == 2);
  static_assert(BlePersistencePolicy::kConfigStatusSubscriptions == 1);
  static_assert(BlePersistencePolicy::kPersistentSubscriptionsPerPeer == 8);
  static_assert(BlePersistencePolicy::kMigrationReserve == 8);
  static_assert(BlePersistencePolicy::kRequiredCccdCapacity == 32);

  assert(!BlePersistencePolicy::cccd_capacity_supports_product(8));
  assert(!BlePersistencePolicy::cccd_capacity_supports_product(31));
  assert(BlePersistencePolicy::cccd_capacity_supports_product(32));
}

void schema_migration_preserves_only_service_changed_subscription() {
  using ai_keyboard::BlePersistencePolicy;

  constexpr std::uint16_t kServiceChangedHandle = 0x0003;
  assert(BlePersistencePolicy::preserve_cccd_during_schema_migration(
      kServiceChangedHandle, kServiceChangedHandle));
  assert(!BlePersistencePolicy::preserve_cccd_during_schema_migration(
      0x0038, kServiceChangedHandle));
  assert(!BlePersistencePolicy::preserve_cccd_during_schema_migration(
      0x0038, 0));
}

void storage_pressure_never_selects_bond_eviction() {
  using ai_keyboard::BleStorePressureAction;
  using ai_keyboard::BleStorePressureObject;
  using ai_keyboard::ble_store_pressure_action;

  assert(ble_store_pressure_action(BleStorePressureObject::Cccd) ==
         BleStorePressureAction::ReclaimOrphanCccdOnly);
  assert(ble_store_pressure_action(
             BleStorePressureObject::SecurityOrIdentity) ==
         BleStorePressureAction::RejectWithoutDeletingBond);
  assert(ble_store_pressure_action(BleStorePressureObject::Other) ==
         BleStorePressureAction::RejectWithoutDeletingBond);
}

}  // namespace

int main() {
  capacity_is_derived_from_the_product_contract();
  schema_migration_preserves_only_service_changed_subscription();
  storage_pressure_never_selects_bond_eviction();
  return 0;
}
