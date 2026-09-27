#include <cassert>
#include <cstdint>

#include "keyboard/ble_connection_profile.h"

namespace {

constexpr ai_keyboard::BleConnectionProfileBounds kStable{
    0,
    36,
    0,
    400,
};

void stable_profile_rejects_out_of_contract_parameters() {
  assert(ai_keyboard::ble_connection_parameters_match({12, 0, 400}, kStable));
  assert(ai_keyboard::ble_connection_parameters_match({36, 0, 800}, kStable));
  assert(!ai_keyboard::ble_connection_parameters_match({12, 0, 72}, kStable));
  assert(!ai_keyboard::ble_connection_parameters_match({37, 0, 400}, kStable));
  assert(!ai_keyboard::ble_connection_parameters_match({12, 1, 400}, kStable));
}

void connection_update_outcomes_do_not_spin_on_peer_rejection() {
  using ai_keyboard::BleConnectionUpdateDisposition;
  assert(ai_keyboard::classify_ble_connection_update(true, true) ==
         BleConnectionUpdateDisposition::Settled);
  assert(ai_keyboard::classify_ble_connection_update(true, false) ==
         BleConnectionUpdateDisposition::RetryWithBackoff);
  assert(ai_keyboard::classify_ble_connection_update(false, false) ==
         BleConnectionUpdateDisposition::RetryWithBackoff);
}

void connection_update_backoff_is_bounded() {
  assert(ai_keyboard::ble_connection_update_retry_delay_us(0) == 500000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(1) == 1000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(2) == 2000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(3) == 4000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(4) == 8000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(5) == 8000000);
  assert(ai_keyboard::ble_connection_update_retry_delay_us(UINT8_MAX) ==
         8000000);
}

void accepted_notification_gets_a_connection_event_grace_window() {
  // A requested 45 ms profile is not proof of the central's negotiated
  // interval. Unknown state therefore uses the legal 4 s maximum.
  assert(ai_keyboard::ble_tx_grace_window_us(0, false) == 8010000);
  assert(ai_keyboard::ble_tx_grace_window_us(36, true) == 100000);

  // A peer-selected 100 ms interval must extend the guard rather than relying
  // on the normal product profile.
  assert(ai_keyboard::ble_tx_grace_window_us(80, true) == 210000);

  // Inputs outside the Bluetooth LE interval range are clamped defensively.
  assert(ai_keyboard::ble_tx_grace_window_us(1, true) == 100000);
  assert(ai_keyboard::ble_tx_grace_window_us(UINT16_MAX, true) == 8010000);
}

}  // namespace

int main() {
  stable_profile_rejects_out_of_contract_parameters();
  connection_update_outcomes_do_not_spin_on_peer_rejection();
  connection_update_backoff_is_bounded();
  accepted_notification_gets_a_connection_event_grace_window();
  return 0;
}
