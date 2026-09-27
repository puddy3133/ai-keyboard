#include "keyboard/boot_led_sequence.h"

#include <cstdint>

namespace ai_keyboard {
namespace {

bool deadline_reached(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
}

bool deadline_pending(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(deadline_ms - now_ms) > 0;
}

}  // namespace

void BootLedDeferredFeedback::clear() {
  feedback_ = {};
  effect_until_ms_ = 0;
  replay_full_duration_after_boot_ = false;
}

void BootLedDeferredFeedback::defer(
    const InputActivityFeedback& feedback,
    std::uint32_t now_ms,
    bool replay_full_duration_after_boot) {
  if (!feedback.active) {
    return;
  }
  feedback_ = feedback;
  effect_until_ms_ = now_ms + feedback.duration_ms;
  replay_full_duration_after_boot_ = replay_full_duration_after_boot;
}

bool BootLedDeferredFeedback::pending() const {
  return feedback_.active;
}

bool BootLedDeferredFeedback::take(
    std::uint32_t now_ms,
    BootLedDeferredPlayback* playback) {
  if (playback == nullptr || !pending()) {
    return false;
  }

  const auto feedback = feedback_;
  const auto original_effect_until_ms = effect_until_ms_;
  const bool replay_full_duration = replay_full_duration_after_boot_;
  clear();

  if (!replay_full_duration &&
      !deadline_pending(now_ms, original_effect_until_ms)) {
    return false;
  }
  playback->feedback = feedback;
  playback->effect_until_ms = replay_full_duration
                                  ? now_ms + feedback.duration_ms
                                  : original_effect_until_ms;
  return true;
}

void BootLedSequence::reserve() {
  phase_ = Phase::Reserved;
  pixel_index_ = 0;
  deadline_ms_ = 0;
}

bool BootLedSequence::reserved() const {
  return phase_ == Phase::Reserved;
}

bool BootLedSequence::start(std::uint32_t now_ms) {
  const bool started_from_reservation = reserved();
  phase_ = Phase::Pixel;
  pixel_index_ = 0;
  deadline_ms_ = now_ms;
  return started_from_reservation;
}

void BootLedSequence::cancel() {
  phase_ = Phase::Idle;
  pixel_index_ = 0;
  deadline_ms_ = 0;
}

bool BootLedSequence::active() const {
  return phase_ != Phase::Idle;
}

bool BootLedSequence::take_due_frame(std::uint32_t now_ms,
                                     BootLedFrame* frame) {
  if (frame == nullptr || !active() ||
      !deadline_reached(now_ms, deadline_ms_)) {
    return false;
  }

  switch (phase_) {
    case Phase::Reserved:
      return false;
    case Phase::Pixel:
      *frame = {BootLedFrameKind::Pixel, pixel_index_};
      if (pixel_index_ + 1U < kPixelCount) {
        ++pixel_index_;
      } else {
        phase_ = Phase::AllPixels;
      }
      deadline_ms_ = now_ms + kFrameHoldMs;
      return true;
    case Phase::AllPixels:
      *frame = {BootLedFrameKind::AllPixels, 0};
      phase_ = Phase::Rainbow;
      deadline_ms_ = now_ms + kFrameHoldMs;
      return true;
    case Phase::Rainbow:
      *frame = {BootLedFrameKind::Rainbow, 0};
      phase_ = Phase::RainbowHold;
      deadline_ms_ = now_ms + kRainbowOwnershipMs;
      return true;
    case Phase::RainbowHold:
      *frame = {BootLedFrameKind::Complete, 0};
      cancel();
      return true;
    case Phase::Idle:
      return false;
  }
  return false;
}

bool BootLedSequence::next_deadline_ms(
    std::uint32_t* deadline_ms) const {
  if (deadline_ms == nullptr || !active() || reserved()) {
    return false;
  }
  *deadline_ms = deadline_ms_;
  return true;
}

}  // namespace ai_keyboard
