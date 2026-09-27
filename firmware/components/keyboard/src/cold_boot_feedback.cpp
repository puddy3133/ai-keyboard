#include "keyboard/cold_boot_feedback.h"

namespace ai_keyboard {

ColdBootFeedbackAction ColdBootFeedbackCoordinator::begin(
    bool cold_boot) {
  started_generation_ = 0;
  state_ = cold_boot ? ColdBootFeedbackState::AwaitingAudioOutcome
                     : ColdBootFeedbackState::Suppressed;
  liveness_deadline_ms_ = 0;
  liveness_deadline_armed_ = false;
  audio_attempt_committed_ = false;
  return cold_boot ? ColdBootFeedbackAction::ReserveVisual
                   : ColdBootFeedbackAction::None;
}

void ColdBootFeedbackCoordinator::start_admission_window(
    std::uint32_t now_ms,
    std::uint32_t max_admission_wait_ms) {
  if (state_ != ColdBootFeedbackState::AwaitingAudioOutcome ||
      audio_attempt_committed_) {
    return;
  }
  liveness_deadline_ms_ = now_ms + max_admission_wait_ms;
  liveness_deadline_armed_ = true;
}

void ColdBootFeedbackCoordinator::on_audio_attempt_committed() {
  if (state_ != ColdBootFeedbackState::AwaitingAudioOutcome) {
    return;
  }
  // Once the owner has synchronously handed a request to SpeakerOutput, its
  // bounded worker owns the outcome. Input/deadline may no longer reinterpret
  // an already submitted (but not yet owner-observed) first PCM as silence.
  audio_attempt_committed_ = true;
  liveness_deadline_armed_ = false;
}

ColdBootFeedbackAction
ColdBootFeedbackCoordinator::on_first_pcm_submitted(
    std::uint32_t generation) {
  if (state_ != ColdBootFeedbackState::AwaitingAudioOutcome ||
      generation == 0) {
    return ColdBootFeedbackAction::None;
  }
  started_generation_ = generation;
  state_ = ColdBootFeedbackState::VisualRunning;
  liveness_deadline_armed_ = false;
  return ColdBootFeedbackAction::StartVisual;
}

ColdBootFeedbackAction
ColdBootFeedbackCoordinator::on_silent_terminal() {
  return start_visual_without_audio();
}

ColdBootFeedbackAction
ColdBootFeedbackCoordinator::on_priority_preempted() {
  if (audio_attempt_committed_) {
    return ColdBootFeedbackAction::None;
  }
  return start_visual_without_audio();
}

ColdBootFeedbackAction
ColdBootFeedbackCoordinator::on_liveness_deadline(std::uint32_t now_ms) {
  if (!liveness_deadline_armed_ ||
      static_cast<std::int32_t>(now_ms - liveness_deadline_ms_) < 0) {
    return ColdBootFeedbackAction::None;
  }
  return start_visual_without_audio();
}

void ColdBootFeedbackCoordinator::mark_visual_complete() {
  if (state_ == ColdBootFeedbackState::VisualRunning) {
    state_ = ColdBootFeedbackState::Complete;
  }
}

ColdBootFeedbackState ColdBootFeedbackCoordinator::state() const {
  return state_;
}

bool ColdBootFeedbackCoordinator::awaiting_audio_outcome() const {
  return state_ == ColdBootFeedbackState::AwaitingAudioOutcome;
}

bool ColdBootFeedbackCoordinator::next_deadline_ms(
    std::uint32_t* deadline_ms) const {
  if (deadline_ms == nullptr || !liveness_deadline_armed_ ||
      state_ != ColdBootFeedbackState::AwaitingAudioOutcome) {
    return false;
  }
  *deadline_ms = liveness_deadline_ms_;
  return true;
}

std::uint32_t ColdBootFeedbackCoordinator::started_generation() const {
  return started_generation_;
}

ColdBootFeedbackAction
ColdBootFeedbackCoordinator::start_visual_without_audio() {
  if (state_ != ColdBootFeedbackState::AwaitingAudioOutcome) {
    return ColdBootFeedbackAction::None;
  }
  state_ = ColdBootFeedbackState::VisualRunning;
  liveness_deadline_armed_ = false;
  return ColdBootFeedbackAction::StartVisual;
}

}  // namespace ai_keyboard
