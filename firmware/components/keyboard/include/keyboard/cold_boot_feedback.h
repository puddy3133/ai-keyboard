#pragma once

#include <cstdint>

namespace ai_keyboard {

enum class ColdBootFeedbackAction : std::uint8_t {
  None,
  ReserveVisual,
  StartVisual,
};

enum class ColdBootFeedbackState : std::uint8_t {
  Suppressed,
  AwaitingAudioOutcome,
  VisualRunning,
  Complete,
};

// Coordinates only the user-visible cold-boot start boundary. Audio remains
// owned by SpeakerOutput and pixels remain owned by StatusLedStrip. A real
// cold boot first reserves the strip without rendering. The strip starts only
// after either the first PCM submit or an explicit silent terminal outcome.
class ColdBootFeedbackCoordinator {
 public:
  ColdBootFeedbackAction begin(bool cold_boot);
  void start_admission_window(std::uint32_t now_ms,
                              std::uint32_t max_admission_wait_ms);
  void on_audio_attempt_committed();
  ColdBootFeedbackAction on_first_pcm_submitted(std::uint32_t generation);
  ColdBootFeedbackAction on_silent_terminal();
  ColdBootFeedbackAction on_priority_preempted();
  ColdBootFeedbackAction on_liveness_deadline(std::uint32_t now_ms);
  void mark_visual_complete();

  ColdBootFeedbackState state() const;
  bool awaiting_audio_outcome() const;
  bool next_deadline_ms(std::uint32_t* deadline_ms) const;
  std::uint32_t started_generation() const;

 private:
  ColdBootFeedbackAction start_visual_without_audio();

  ColdBootFeedbackState state_ = ColdBootFeedbackState::Suppressed;
  std::uint32_t started_generation_ = 0;
  std::uint32_t liveness_deadline_ms_ = 0;
  bool liveness_deadline_armed_ = false;
  bool audio_attempt_committed_ = false;
};

}  // namespace ai_keyboard
