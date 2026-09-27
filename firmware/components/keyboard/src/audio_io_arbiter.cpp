#include "keyboard/audio_io_arbiter.h"

namespace ai_keyboard {

void AudioIoArbiter::set_work_ready_callback(WorkReadyCallback callback,
                                             void* context) {
  if (callback == nullptr) {
    work_ready_callback_.store(nullptr, std::memory_order_release);
    work_ready_context_.store(nullptr, std::memory_order_release);
    return;
  }
  work_ready_context_.store(context, std::memory_order_release);
  work_ready_callback_.store(callback, std::memory_order_release);
}

bool AudioIoArbiter::try_begin_speaker(std::uint32_t generation) {
  if (generation == 0 || !try_enter_runtime_transition()) {
    return false;
  }

  bool accepted = false;
  if (microphone_generation_.load(std::memory_order_acquire) == 0) {
    std::uint32_t expected = 0;
    accepted = speaker_generation_.compare_exchange_strong(
        expected,
        generation,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

  // Close the race where a microphone request arrives between the first
  // microphone check and the speaker reservation.
  if (accepted &&
      microphone_generation_.load(std::memory_order_acquire) != 0) {
    std::uint32_t expected = generation;
    speaker_generation_.compare_exchange_strong(
        expected,
        0,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
    accepted = false;
  }
  leave_runtime_transition();
  return accepted;
}

bool AudioIoArbiter::finish_speaker(std::uint32_t generation) {
  if (generation == 0) {
    return false;
  }
  std::uint32_t expected = generation;
  return speaker_generation_.compare_exchange_strong(
      expected,
      0,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
}

bool AudioIoArbiter::request_microphone(std::uint32_t generation) {
  if (generation == 0 || !try_enter_runtime_transition()) {
    return false;
  }
  microphone_power_ready_generation_.store(0, std::memory_order_release);
  microphone_generation_.store(generation, std::memory_order_release);
  leave_runtime_transition();
  return true;
}

bool AudioIoArbiter::mark_microphone_power_ready(std::uint32_t generation) {
  if (generation == 0 ||
      microphone_generation_.load(std::memory_order_acquire) != generation) {
    return false;
  }
  microphone_power_ready_generation_.store(generation,
                                           std::memory_order_release);
  return true;
}

bool AudioIoArbiter::finish_microphone(std::uint32_t generation) {
  if (generation == 0) {
    return false;
  }
  std::uint32_t expected = generation;
  if (!microphone_generation_.compare_exchange_strong(
          expected,
          0,
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  expected = generation;
  microphone_power_ready_generation_.compare_exchange_strong(
      expected,
      0,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
  return true;
}

bool AudioIoArbiter::speaker_active() const {
  return speaker_generation() != 0;
}

std::uint32_t AudioIoArbiter::speaker_generation() const {
  return speaker_generation_.load(std::memory_order_acquire);
}

bool AudioIoArbiter::microphone_requested() const {
  return microphone_generation() != 0;
}

std::uint32_t AudioIoArbiter::microphone_generation() const {
  return microphone_generation_.load(std::memory_order_acquire);
}

bool AudioIoArbiter::microphone_hardware_ready(
    std::uint32_t generation) const {
  return generation != 0 &&
         microphone_generation_.load(std::memory_order_acquire) == generation &&
         microphone_power_ready_generation_.load(std::memory_order_acquire) ==
             generation &&
         speaker_generation_.load(std::memory_order_acquire) == 0;
}

bool AudioIoArbiter::try_begin_deep_sleep_quiesce() {
  std::uint32_t expected = 0;
  if (!runtime_transition_gate_.compare_exchange_strong(
          expected,
          kDeepSleepQuiescingBit,
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  if (speaker_generation_.load(std::memory_order_acquire) != 0 ||
      microphone_generation_.load(std::memory_order_acquire) != 0) {
    // Preserve any runtime-transition count installed by a request that raced
    // this ownership recheck. cancel_deep_sleep_quiesce() only clears the
    // phase bits, so the admitted request can finish normally.
    cancel_deep_sleep_quiesce();
    return false;
  }
  return true;
}

bool AudioIoArbiter::cancel_deep_sleep_quiesce() {
  auto state = runtime_transition_gate_.load(std::memory_order_acquire);
  while ((state & kDeepSleepQuiescingBit) != 0 &&
         (state & kDeepSleepTerminalBit) == 0) {
    const auto running_state = state & kRuntimeTransitionCountMask;
    if (runtime_transition_gate_.compare_exchange_weak(
            state,
            running_state,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

bool AudioIoArbiter::begin_deep_sleep_terminal() {
  std::uint32_t expected = kDeepSleepQuiescingBit;
  return runtime_transition_gate_.compare_exchange_strong(
      expected,
      kDeepSleepTerminalBit,
      std::memory_order_acq_rel,
      std::memory_order_acquire);
}

bool AudioIoArbiter::deep_sleep_quiescing() const {
  return (runtime_transition_gate_.load(std::memory_order_acquire) &
          kDeepSleepQuiescingBit) != 0;
}

bool AudioIoArbiter::deep_sleep_quiesce_interrupted() const {
  return (runtime_transition_gate_.load(std::memory_order_acquire) &
          kDeepSleepInterruptedBit) != 0;
}

bool AudioIoArbiter::deep_sleep_terminal() const {
  return (runtime_transition_gate_.load(std::memory_order_acquire) &
          kDeepSleepTerminalBit) != 0;
}

bool AudioIoArbiter::try_enter_runtime_transition() {
  auto state = runtime_transition_gate_.load(std::memory_order_acquire);
  while ((state & kDeepSleepTerminalBit) == 0) {
    if ((state & kRuntimeTransitionCountMask) ==
        kRuntimeTransitionCountMask) {
      return false;
    }
    auto admitted_state = state + 1U;
    const bool interrupts_quiesce =
        (state & kDeepSleepQuiescingBit) != 0;
    if (interrupts_quiesce) {
      admitted_state |= kDeepSleepInterruptedBit;
    }
    if (runtime_transition_gate_.compare_exchange_weak(
            state,
            admitted_state,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      if (interrupts_quiesce) {
        notify_work_ready();
      }
      return true;
    }
  }
  return false;
}

void AudioIoArbiter::leave_runtime_transition() {
  runtime_transition_gate_.fetch_sub(1, std::memory_order_release);
}

void AudioIoArbiter::notify_work_ready() const {
  const auto callback =
      work_ready_callback_.load(std::memory_order_acquire);
  if (callback != nullptr) {
    callback(work_ready_context_.load(std::memory_order_acquire));
  }
}

}  // namespace ai_keyboard
