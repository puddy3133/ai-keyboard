#pragma once

#include <cstdint>

namespace ai_keyboard {

// Pure admission state used by the platform BLE transport. The platform owns
// synchronization; this type makes the Prepare / Quiesce / Commit contract
// explicit and testable without FreeRTOS.
class BleManagementGate {
 public:
  enum class Phase : std::uint8_t {
    Closed,
    Running,
    Quiescing,
    Terminal,
  };

  bool start() {
    if (phase_ != Phase::Closed) {
      return false;
    }
    phase_ = Phase::Running;
    quiesce_interrupted_ = false;
    return true;
  }

  bool try_enter() {
    if (phase_ == Phase::Quiescing) {
      // A callback that arrives after reversible admission was closed must
      // abort that sleep attempt, but the callback itself is still admitted.
      // Otherwise an acknowledged remote operation can be silently discarded
      // in the prepare-to-commit race window.
      quiesce_interrupted_ = true;
      ++in_flight_;
      return true;
    }
    if (phase_ != Phase::Running) {
      return false;
    }
    ++in_flight_;
    return true;
  }

  void leave() {
    if (in_flight_ != 0) {
      --in_flight_;
    }
  }

  bool try_begin_quiesce() {
    if (phase_ == Phase::Quiescing) {
      return true;
    }
    if (phase_ != Phase::Running || in_flight_ != 0) {
      return false;
    }
    phase_ = Phase::Quiescing;
    quiesce_interrupted_ = false;
    return true;
  }

  bool cancel_quiesce() {
    if (phase_ != Phase::Quiescing) {
      return false;
    }
    phase_ = Phase::Running;
    quiesce_interrupted_ = false;
    return true;
  }

  bool begin_terminal() {
    if (phase_ != Phase::Quiescing || in_flight_ != 0 ||
        quiesce_interrupted_) {
      return false;
    }
    phase_ = Phase::Terminal;
    return true;
  }

  Phase phase() const { return phase_; }
  std::uint32_t in_flight() const { return in_flight_; }
  bool admission_open() const {
    return phase_ == Phase::Running || phase_ == Phase::Quiescing;
  }
  bool quiesce_interrupted() const { return quiesce_interrupted_; }

 private:
  Phase phase_ = Phase::Closed;
  std::uint32_t in_flight_ = 0;
  bool quiesce_interrupted_ = false;
};

}  // namespace ai_keyboard
