#include <cassert>

#include "keyboard/speaker_playback.h"

namespace {

using ai_keyboard::SpeakerPlayback;
using ai_keyboard::SpeakerPlaybackPhase;
using ai_keyboard::SpeakerPlaybackResult;

void accepted_request_holds_power_and_blocks_sleep_until_drain_finishes() {
  SpeakerPlayback playback;
  assert(!playback.active());
  assert(!playback.power_required());
  assert(!playback.sleep_blocked());

  const auto ticket = playback.request();
  assert(ticket.accepted);
  assert(ticket.generation != 0);
  assert(playback.phase() == SpeakerPlaybackPhase::Starting);
  assert(playback.active());
  assert(playback.power_required());
  assert(playback.sleep_blocked());

  assert(playback.mark_started(ticket.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Playing);
  assert(playback.finish(ticket.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Draining);
  assert(playback.pending_result() == SpeakerPlaybackResult::Succeeded);
  assert(playback.power_required());
  assert(playback.sleep_blocked());

  assert(playback.mark_drained(ticket.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Idle);
  assert(!playback.active());
  assert(!playback.power_required());
  assert(!playback.sleep_blocked());
  assert(playback.last_result() == SpeakerPlaybackResult::Succeeded);
  assert(playback.last_completed_generation() == ticket.generation);
}

void capacity_one_rejects_a_second_request_until_fully_drained() {
  SpeakerPlayback playback;
  const auto first = playback.request();
  assert(first.accepted);

  const auto while_starting = playback.request();
  assert(!while_starting.accepted);
  assert(while_starting.generation == 0);

  assert(playback.mark_started(first.generation));
  const auto while_playing = playback.request();
  assert(!while_playing.accepted);

  assert(playback.finish(first.generation));
  const auto while_draining = playback.request();
  assert(!while_draining.accepted);

  assert(playback.mark_drained(first.generation));
  const auto second = playback.request();
  assert(second.accepted);
  assert(second.generation != first.generation);
}

void late_callbacks_cannot_mutate_a_new_generation() {
  SpeakerPlayback playback;
  const auto first = playback.request();
  assert(playback.fail(first.generation));
  assert(playback.mark_drained(first.generation));

  const auto second = playback.request();
  assert(second.accepted);
  assert(second.generation != first.generation);

  assert(!playback.mark_started(first.generation));
  assert(!playback.finish(first.generation));
  assert(!playback.fail(first.generation));
  assert(!playback.mark_drained(first.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Starting);
  assert(playback.active_generation() == second.generation);
  assert(playback.pending_result() == SpeakerPlaybackResult::None);
}

void failure_is_fail_safe_and_requires_a_hardware_drain() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();
  assert(ticket.accepted);

  assert(playback.fail(ticket.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Draining);
  assert(playback.pending_result() == SpeakerPlaybackResult::Failed);
  assert(playback.power_required());
  assert(playback.sleep_blocked());
  assert(!playback.request().accepted);

  // Repeated failure callbacks are harmless and preserve the failed outcome.
  assert(playback.fail(ticket.generation));
  assert(playback.pending_result() == SpeakerPlaybackResult::Failed);

  assert(playback.mark_drained(ticket.generation));
  assert(playback.last_result() == SpeakerPlaybackResult::Failed);
  assert(!playback.power_required());
  assert(playback.request().accepted);
}

void a_late_error_upgrades_an_in_progress_successful_drain() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();
  assert(playback.mark_started(ticket.generation));
  assert(playback.finish(ticket.generation));
  assert(playback.pending_result() == SpeakerPlaybackResult::Succeeded);

  assert(playback.fail(ticket.generation));
  assert(playback.pending_result() == SpeakerPlaybackResult::Failed);
  assert(playback.mark_drained(ticket.generation));
  assert(playback.last_result() == SpeakerPlaybackResult::Failed);
}

void invalid_transitions_do_not_release_the_active_generation() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();

  assert(!playback.mark_drained(ticket.generation));
  assert(playback.phase() == SpeakerPlaybackPhase::Starting);
  assert(!playback.finish(0));
  assert(!playback.cancel(0));
  assert(!playback.fail(0));
  assert(playback.active_generation() == ticket.generation);

  assert(playback.cancel(ticket.generation));
  assert(playback.pending_result() == SpeakerPlaybackResult::Cancelled);
  assert(playback.mark_drained(ticket.generation));
  assert(playback.last_result() == SpeakerPlaybackResult::Cancelled);
}

void first_pcm_event_is_generation_safe_and_one_shot() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();

  auto events = playback.consume_worker_events(
      ticket.generation + 1U, 0U, SpeakerPlaybackResult::None);
  assert(!events.first_pcm());
  assert(!events.terminal());
  assert(playback.phase() == SpeakerPlaybackPhase::Starting);

  events = playback.consume_worker_events(
      ticket.generation, 0U, SpeakerPlaybackResult::None);
  assert(events.first_pcm());
  assert(events.first_pcm_generation == ticket.generation);
  assert(!events.terminal());
  assert(playback.phase() == SpeakerPlaybackPhase::Playing);

  events = playback.consume_worker_events(
      ticket.generation, 0U, SpeakerPlaybackResult::None);
  assert(!events.first_pcm());
  assert(!events.terminal());
  assert(playback.phase() == SpeakerPlaybackPhase::Playing);
}

void first_pcm_and_terminal_are_both_retained_in_one_poll() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();

  const auto events = playback.consume_worker_events(
      ticket.generation,
      ticket.generation,
      SpeakerPlaybackResult::Succeeded);
  assert(events.first_pcm());
  assert(events.first_pcm_generation == ticket.generation);
  assert(events.terminal());
  assert(events.terminal_generation == ticket.generation);
  assert(events.terminal_result == SpeakerPlaybackResult::Succeeded);
  assert(playback.phase() == SpeakerPlaybackPhase::Idle);

  const auto replayed = playback.consume_worker_events(
      ticket.generation,
      ticket.generation,
      SpeakerPlaybackResult::Succeeded);
  assert(!replayed.first_pcm());
  assert(!replayed.terminal());
}

void terminal_without_first_pcm_reports_only_the_terminal_edge() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();

  const auto events = playback.consume_worker_events(
      0U, ticket.generation, SpeakerPlaybackResult::Failed);
  assert(!events.first_pcm());
  assert(events.terminal());
  assert(events.terminal_generation == ticket.generation);
  assert(events.terminal_result == SpeakerPlaybackResult::Failed);
  assert(playback.phase() == SpeakerPlaybackPhase::Idle);
}

void first_pcm_fact_survives_an_owner_cancellation_race() {
  SpeakerPlayback playback;
  const auto ticket = playback.request();
  assert(playback.cancel(ticket.generation));

  const auto events = playback.consume_worker_events(
      ticket.generation,
      ticket.generation,
      SpeakerPlaybackResult::Cancelled);
  assert(events.first_pcm());
  assert(events.terminal());
  assert(events.terminal_result == SpeakerPlaybackResult::Cancelled);
  assert(playback.phase() == SpeakerPlaybackPhase::Idle);
}

}  // namespace

int main() {
  accepted_request_holds_power_and_blocks_sleep_until_drain_finishes();
  capacity_one_rejects_a_second_request_until_fully_drained();
  late_callbacks_cannot_mutate_a_new_generation();
  failure_is_fail_safe_and_requires_a_hardware_drain();
  a_late_error_upgrades_an_in_progress_successful_drain();
  invalid_transitions_do_not_release_the_active_generation();
  first_pcm_event_is_generation_safe_and_one_shot();
  first_pcm_and_terminal_are_both_retained_in_one_poll();
  terminal_without_first_pcm_reports_only_the_terminal_edge();
  first_pcm_fact_survives_an_owner_cancellation_race();
  return 0;
}
