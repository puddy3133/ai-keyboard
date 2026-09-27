#pragma once

#include <atomic>
#include <cstdint>

namespace ai_keyboard {

// Cross-task ownership barrier for the board microphone and speaker. The
// microphone has priority: a new microphone generation makes an active
// speaker cancel, then waits until both the speaker hardware is drained and
// the platform task has established the microphone power lease.
class AudioIoArbiter {
 public:
  using WorkReadyCallback = void (*)(void* context);

  // The callback is installed before worker tasks start. A runtime request
  // that arrives during reversible Deep Sleep quiesce uses it to wake the
  // platform owner after atomically invalidating that sleep attempt.
  void set_work_ready_callback(WorkReadyCallback callback, void* context);

  bool try_begin_speaker(std::uint32_t generation);
  bool finish_speaker(std::uint32_t generation);

  bool request_microphone(std::uint32_t generation);
  bool mark_microphone_power_ready(std::uint32_t generation);
  bool finish_microphone(std::uint32_t generation);

  bool speaker_active() const;
  std::uint32_t speaker_generation() const;
  bool microphone_requested() const;
  std::uint32_t microphone_generation() const;
  bool microphone_hardware_ready(std::uint32_t generation) const;

  // Atomically closes admission for new microphone/speaker ownership before
  // the platform commits to deep sleep. Success guarantees that no ownership
  // transition is in flight and neither audio side currently owns hardware.
  bool try_begin_deep_sleep_quiesce();
  bool cancel_deep_sleep_quiesce();
  // Irreversible admission cutoff. It succeeds only when no request arrived
  // after quiesce and no runtime transition is still publishing ownership.
  bool begin_deep_sleep_terminal();
  bool deep_sleep_quiescing() const;
  bool deep_sleep_quiesce_interrupted() const;
  bool deep_sleep_terminal() const;

 private:
  bool try_enter_runtime_transition();
  void leave_runtime_transition();
  void notify_work_ready() const;

  static constexpr std::uint32_t kDeepSleepQuiescingBit = 1U << 31;
  static constexpr std::uint32_t kDeepSleepInterruptedBit = 1U << 30;
  static constexpr std::uint32_t kDeepSleepTerminalBit = 1U << 29;
  static constexpr std::uint32_t kRuntimeTransitionCountMask =
      ~(kDeepSleepQuiescingBit | kDeepSleepInterruptedBit |
        kDeepSleepTerminalBit);

  std::atomic<std::uint32_t> speaker_generation_{0};
  std::atomic<std::uint32_t> microphone_generation_{0};
  std::atomic<std::uint32_t> microphone_power_ready_generation_{0};
  std::atomic<std::uint32_t> runtime_transition_gate_{0};
  std::atomic<WorkReadyCallback> work_ready_callback_{nullptr};
  std::atomic<void*> work_ready_context_{nullptr};
};

}  // namespace ai_keyboard
