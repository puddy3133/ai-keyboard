#include "keyboard/encoder_press_gesture.h"

namespace ai_keyboard {

void EncoderPressGesture::press(std::uint32_t now_ms) {
  phase_ = EncoderPressGesturePhase::Pending;
  down_ms_ = now_ms;
}

bool EncoderPressGesture::trigger_config_if_due(
    std::uint32_t now_ms,
    std::uint32_t hold_ms,
    bool physically_pressed) {
  if (phase_ != EncoderPressGesturePhase::Pending ||
      !physically_pressed ||
      static_cast<std::uint32_t>(now_ms - down_ms_) < hold_ms) {
    return false;
  }
  phase_ = EncoderPressGesturePhase::ConfigTriggered;
  return true;
}

EncoderPressReleaseResult EncoderPressGesture::release() {
  EncoderPressReleaseResult result;
  switch (phase_) {
    case EncoderPressGesturePhase::Pending:
      result.dispatch_click = true;
      break;
    case EncoderPressGesturePhase::ConfigTriggered:
      result.ignored_after_config = true;
      break;
    case EncoderPressGesturePhase::Idle:
      break;
  }
  reset();
  return result;
}

void EncoderPressGesture::reset() {
  phase_ = EncoderPressGesturePhase::Idle;
  down_ms_ = 0;
}

EncoderPressGesturePhase EncoderPressGesture::phase() const {
  return phase_;
}

bool EncoderPressGesture::pending() const {
  return phase_ != EncoderPressGesturePhase::Idle;
}

bool EncoderPressGesture::config_triggered() const {
  return phase_ == EncoderPressGesturePhase::ConfigTriggered;
}

bool EncoderPressGesture::config_deadline(
    std::uint32_t hold_ms,
    std::uint32_t* deadline_ms) const {
  if (phase_ != EncoderPressGesturePhase::Pending || deadline_ms == nullptr) {
    return false;
  }
  *deadline_ms = down_ms_ + hold_ms;
  return true;
}

}  // namespace ai_keyboard
