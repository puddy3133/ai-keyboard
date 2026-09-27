#include <cassert>
#include <cstdint>

#include "keyboard/boot_led_sequence.h"

namespace {

using ai_keyboard::BootLedFrame;
using ai_keyboard::BootLedDeferredFeedback;
using ai_keyboard::BootLedDeferredPlayback;
using ai_keyboard::BootLedFrameKind;
using ai_keyboard::BootLedSequence;
using ai_keyboard::FeedbackEffectKind;
using ai_keyboard::InputActivityFeedback;

InputActivityFeedback feedback(std::uint32_t duration_ms,
                               FeedbackEffectKind effect);

void emits_the_existing_cold_boot_timeline_by_absolute_deadline() {
  BootLedSequence sequence;
  BootLedFrame frame;
  std::uint32_t deadline_ms = 0;

  assert(!sequence.start(1'000));
  assert(sequence.active());
  assert(sequence.next_deadline_ms(&deadline_ms));
  assert(deadline_ms == 1'000);

  for (std::uint8_t index = 0; index < BootLedSequence::kPixelCount;
       ++index) {
    const auto now_ms =
        1'000U + index * BootLedSequence::kFrameHoldMs;
    assert(sequence.take_due_frame(now_ms, &frame));
    assert(frame.kind == BootLedFrameKind::Pixel);
    assert(frame.pixel_index == index);
  }

  assert(sequence.take_due_frame(2'100, &frame));
  assert(frame.kind == BootLedFrameKind::AllPixels);
  assert(sequence.take_due_frame(2'320, &frame));
  assert(frame.kind == BootLedFrameKind::Rainbow);
  assert(sequence.take_due_frame(2'770, &frame));
  assert(frame.kind == BootLedFrameKind::Complete);
  assert(!sequence.active());
  assert(!sequence.next_deadline_ms(&deadline_ms));
}

void a_late_owner_emits_one_frame_without_a_catch_up_burst() {
  BootLedSequence sequence;
  BootLedFrame frame;
  std::uint32_t deadline_ms = 0;

  sequence.start(10'000);
  assert(sequence.take_due_frame(10'000, &frame));
  assert(frame.pixel_index == 0);

  assert(sequence.take_due_frame(11'000, &frame));
  assert(frame.kind == BootLedFrameKind::Pixel);
  assert(frame.pixel_index == 1);
  assert(!sequence.take_due_frame(11'000, &frame));
  assert(sequence.next_deadline_ms(&deadline_ms));
  assert(deadline_ms == 11'000 + BootLedSequence::kFrameHoldMs);
}

void deadlines_are_uptime_wrap_safe() {
  constexpr std::uint32_t start_ms = 0xFFFF'FFF0U;
  BootLedSequence sequence;
  BootLedFrame frame;
  std::uint32_t deadline_ms = 0;

  sequence.start(start_ms);
  assert(sequence.take_due_frame(start_ms, &frame));
  assert(sequence.next_deadline_ms(&deadline_ms));
  assert(deadline_ms == start_ms + BootLedSequence::kFrameHoldMs);
  assert(!sequence.take_due_frame(deadline_ms - 1U, &frame));
  assert(sequence.take_due_frame(deadline_ms, &frame));
  assert(frame.pixel_index == 1);
}

void cancel_and_restart_have_one_clear_owner() {
  BootLedSequence sequence;
  BootLedFrame frame;

  sequence.start(5'000);
  sequence.cancel();
  assert(!sequence.active());
  assert(!sequence.take_due_frame(6'000, &frame));

  sequence.start(7'000);
  assert(sequence.take_due_frame(7'000, &frame));
  assert(frame.kind == BootLedFrameKind::Pixel);
  assert(frame.pixel_index == 0);
}

void reservation_owns_visuals_without_a_frame_or_deadline() {
  BootLedSequence sequence;
  BootLedFrame frame;
  std::uint32_t deadline_ms = 0;

  sequence.reserve();
  assert(sequence.active());
  assert(sequence.reserved());
  assert(!sequence.take_due_frame(8'000, &frame));
  assert(!sequence.next_deadline_ms(&deadline_ms));
}

void starting_a_reservation_emits_pixel_zero_and_preserves_feedback() {
  BootLedSequence sequence;
  BootLedDeferredFeedback deferred;
  BootLedDeferredPlayback playback;
  BootLedFrame frame;

  sequence.reserve();
  deferred.defer(feedback(900, FeedbackEffectKind::LightBarRipple),
                 9'000,
                 true);

  const bool started_from_reservation = sequence.start(9'500);
  if (!started_from_reservation) {
    deferred.clear();
  }

  assert(started_from_reservation);
  assert(!sequence.reserved());
  assert(deferred.pending());
  assert(sequence.take_due_frame(9'500, &frame));
  assert(frame.kind == BootLedFrameKind::Pixel);
  assert(frame.pixel_index == 0);
  assert(deferred.take(11'500, &playback));
  assert(playback.feedback.effect == FeedbackEffectKind::LightBarRipple);
  assert(playback.effect_until_ms == 12'400);
}

void direct_start_reports_no_reservation() {
  BootLedSequence sequence;
  BootLedFrame frame;

  assert(!sequence.start(12'000));
  assert(sequence.take_due_frame(12'000, &frame));
  assert(frame.kind == BootLedFrameKind::Pixel);
  assert(frame.pixel_index == 0);
}

InputActivityFeedback feedback(std::uint32_t duration_ms,
                               FeedbackEffectKind effect) {
  return {true,
          effect,
          ai_keyboard::FeedbackDirection::None,
          {1, 2, 3},
          duration_ms,
          40};
}

void transient_feedback_keeps_its_original_expiry() {
  BootLedDeferredFeedback deferred;
  BootLedDeferredPlayback playback;

  deferred.defer(feedback(160, FeedbackEffectKind::DirectionalFlow),
                 1'000,
                 false);
  assert(deferred.pending());
  assert(deferred.take(1'100, &playback));
  assert(playback.feedback.effect == FeedbackEffectKind::DirectionalFlow);
  assert(playback.effect_until_ms == 1'160);
  assert(!deferred.pending());
}

void expired_transient_feedback_is_not_replayed_after_boot() {
  BootLedDeferredFeedback deferred;
  BootLedDeferredPlayback playback;

  deferred.defer(feedback(160, FeedbackEffectKind::DirectionalFlow),
                 2'000,
                 false);
  assert(!deferred.take(2'160, &playback));
  assert(!deferred.pending());
}

void status_feedback_replays_its_full_duration_after_boot() {
  BootLedDeferredFeedback deferred;
  BootLedDeferredPlayback playback;

  deferred.defer(feedback(900, FeedbackEffectKind::LightBarRipple),
                 3'000,
                 true);
  assert(deferred.take(5'000, &playback));
  assert(playback.feedback.effect == FeedbackEffectKind::LightBarRipple);
  assert(playback.effect_until_ms == 5'900);
}

void newest_feedback_replaces_the_previous_deferred_event() {
  BootLedDeferredFeedback deferred;
  BootLedDeferredPlayback playback;

  deferred.defer(feedback(900, FeedbackEffectKind::LightBarRipple),
                 6'000,
                 true);
  deferred.defer(feedback(1'000, FeedbackEffectKind::ConfirmPulse),
                 6'100,
                 true);
  assert(deferred.take(7'000, &playback));
  assert(playback.feedback.effect == FeedbackEffectKind::ConfirmPulse);
  assert(playback.effect_until_ms == 8'000);
}

}  // namespace

int main() {
  emits_the_existing_cold_boot_timeline_by_absolute_deadline();
  a_late_owner_emits_one_frame_without_a_catch_up_burst();
  deadlines_are_uptime_wrap_safe();
  cancel_and_restart_have_one_clear_owner();
  reservation_owns_visuals_without_a_frame_or_deadline();
  starting_a_reservation_emits_pixel_zero_and_preserves_feedback();
  direct_start_reports_no_reservation();
  transient_feedback_keeps_its_original_expiry();
  expired_transient_feedback_is_not_replayed_after_boot();
  status_feedback_replays_its_full_duration_after_boot();
  newest_feedback_replaces_the_previous_deferred_event();
  return 0;
}
