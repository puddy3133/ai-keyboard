#include <cassert>

#include "keyboard/cold_boot_feedback.h"

namespace {

using ai_keyboard::ColdBootFeedbackAction;
using ai_keyboard::ColdBootFeedbackCoordinator;
using ai_keyboard::ColdBootFeedbackState;

void cold_boot_reserves_until_audio_has_a_real_outcome() {
  ColdBootFeedbackCoordinator coordinator;
  assert(coordinator.begin(true) ==
         ColdBootFeedbackAction::ReserveVisual);
  assert(coordinator.awaiting_audio_outcome());
  assert(coordinator.state() ==
         ColdBootFeedbackState::AwaitingAudioOutcome);

  assert(coordinator.on_first_pcm_submitted(0) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.awaiting_audio_outcome());
  std::uint32_t deadline_ms = 0;
  // Visual ownership is reserved during unrelated platform initialization,
  // but its admission watchdog starts only when the owner loop can actually
  // advance Store/Wi-Fi/Speaker work.
  assert(!coordinator.next_deadline_ms(&deadline_ms));
  coordinator.start_admission_window(100, 5000);
  assert(coordinator.next_deadline_ms(&deadline_ms));
  assert(deadline_ms == 5100);
}

void first_pcm_starts_visual_exactly_once() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(0, 5000);
  assert(coordinator.on_first_pcm_submitted(7) ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.started_generation() == 7);
  assert(coordinator.state() ==
         ColdBootFeedbackState::VisualRunning);

  assert(coordinator.on_first_pcm_submitted(7) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_first_pcm_submitted(8) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_silent_terminal() ==
         ColdBootFeedbackAction::None);

  coordinator.mark_visual_complete();
  assert(coordinator.state() == ColdBootFeedbackState::Complete);
}

void silent_terminal_starts_led_only_and_rejects_late_start() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(0, 5000);
  assert(coordinator.on_silent_terminal() ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.started_generation() == 0);
  assert(coordinator.on_first_pcm_submitted(11) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_silent_terminal() ==
         ColdBootFeedbackAction::None);
}

void priority_preemption_starts_led_only_and_rejects_late_audio() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(100, 5000);
  assert(coordinator.on_priority_preempted() ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.started_generation() == 0);
  assert(coordinator.on_first_pcm_submitted(4) ==
         ColdBootFeedbackAction::None);
  std::uint32_t deadline_ms = 0;
  assert(!coordinator.next_deadline_ms(&deadline_ms));
}

void committed_audio_attempt_wins_unobserved_first_pcm_race() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(100, 5000);
  coordinator.on_audio_attempt_committed();

  // Model a worker that has accepted (and may already have submitted) PCM,
  // while its owner notification is coalesced with input/deadline wakeups.
  assert(coordinator.on_priority_preempted() ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_liveness_deadline(5100) ==
         ColdBootFeedbackAction::None);
  std::uint32_t deadline_ms = 0;
  assert(!coordinator.next_deadline_ms(&deadline_ms));
  assert(coordinator.on_first_pcm_submitted(12) ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.started_generation() == 12);
}

void committed_audio_terminal_before_first_pcm_starts_led_only() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(100, 5000);
  coordinator.on_audio_attempt_committed();

  assert(coordinator.on_priority_preempted() ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_liveness_deadline(5100) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_silent_terminal() ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.started_generation() == 0);
  assert(coordinator.on_first_pcm_submitted(13) ==
         ColdBootFeedbackAction::None);
}

void liveness_deadline_is_a_terminal_not_a_startup_estimate() {
  ColdBootFeedbackCoordinator coordinator;
  coordinator.begin(true);
  coordinator.start_admission_window(0xfffffff0U, 32U);
  assert(coordinator.on_liveness_deadline(0x0000000fU) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_liveness_deadline(0x00000010U) ==
         ColdBootFeedbackAction::StartVisual);
  assert(coordinator.on_first_pcm_submitted(9) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_liveness_deadline(0x00000011U) ==
         ColdBootFeedbackAction::None);
}

void deep_sleep_resume_suppresses_both_outcomes() {
  ColdBootFeedbackCoordinator coordinator;
  assert(coordinator.begin(false) ==
         ColdBootFeedbackAction::None);
  coordinator.start_admission_window(100, 5000);
  assert(coordinator.state() == ColdBootFeedbackState::Suppressed);
  assert(coordinator.on_first_pcm_submitted(3) ==
         ColdBootFeedbackAction::None);
  assert(coordinator.on_silent_terminal() ==
         ColdBootFeedbackAction::None);
}

void power_source_is_not_part_of_the_state_contract() {
  ColdBootFeedbackCoordinator battery;
  ColdBootFeedbackCoordinator usb;
  assert(battery.begin(true) == usb.begin(true));
  battery.start_admission_window(42, 5000);
  usb.start_admission_window(42, 5000);
  assert(battery.on_first_pcm_submitted(1) ==
         usb.on_first_pcm_submitted(1));
  assert(battery.state() == usb.state());
}

}  // namespace

int main() {
  cold_boot_reserves_until_audio_has_a_real_outcome();
  first_pcm_starts_visual_exactly_once();
  silent_terminal_starts_led_only_and_rejects_late_start();
  priority_preemption_starts_led_only_and_rejects_late_audio();
  committed_audio_attempt_wins_unobserved_first_pcm_race();
  committed_audio_terminal_before_first_pcm_starts_led_only();
  liveness_deadline_is_a_terminal_not_a_startup_estimate();
  deep_sleep_resume_suppresses_both_outcomes();
  power_source_is_not_part_of_the_state_contract();
  return 0;
}
