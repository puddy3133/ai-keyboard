#include <cassert>

#include "keyboard/ble_management_gate.h"

namespace {

using ai_keyboard::BleManagementGate;

void callbacks_are_admitted_only_while_running() {
  BleManagementGate gate;
  assert(!gate.try_enter());
  assert(gate.start());
  assert(!gate.start());
  assert(gate.try_enter());
  assert(gate.in_flight() == 1);
  gate.leave();
  assert(gate.in_flight() == 0);
}

void quiesce_is_atomic_with_callback_admission() {
  BleManagementGate gate;
  assert(gate.start());
  assert(gate.try_enter());
  assert(!gate.try_begin_quiesce());
  assert(gate.admission_open());
  gate.leave();

  assert(gate.try_begin_quiesce());
  assert(gate.phase() == BleManagementGate::Phase::Quiescing);
  assert(gate.try_enter());
  assert(gate.quiesce_interrupted());
  assert(gate.in_flight() == 1);
  assert(!gate.begin_terminal());
  gate.leave();
  assert(gate.try_begin_quiesce());
}

void callback_arrival_cancels_only_the_reversible_attempt() {
  BleManagementGate gate;
  assert(gate.start());
  assert(gate.try_begin_quiesce());
  assert(gate.try_enter());
  assert(gate.quiesce_interrupted());
  gate.leave();
  assert(gate.cancel_quiesce());
  assert(!gate.quiesce_interrupted());
  assert(gate.try_enter());
  gate.leave();
  assert(gate.try_begin_quiesce());
  assert(!gate.quiesce_interrupted());
  assert(gate.begin_terminal());
}

void prepare_can_cancel_but_terminal_commit_cannot_reopen() {
  BleManagementGate gate;
  assert(gate.start());
  assert(gate.try_begin_quiesce());
  assert(gate.cancel_quiesce());
  assert(gate.admission_open());
  assert(gate.try_enter());
  gate.leave();

  assert(gate.try_begin_quiesce());
  assert(gate.begin_terminal());
  assert(gate.phase() == BleManagementGate::Phase::Terminal);
  assert(!gate.cancel_quiesce());
  assert(!gate.try_enter());
}

}  // namespace

int main() {
  callbacks_are_admitted_only_while_running();
  quiesce_is_atomic_with_callback_admission();
  callback_arrival_cancels_only_the_reversible_attempt();
  prepare_can_cancel_but_terminal_commit_cannot_reopen();
  return 0;
}
