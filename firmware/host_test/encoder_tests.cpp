#include <cassert>

#include "keyboard/encoder.h"

void clockwise_detent_emits_positive_step() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void counterclockwise_detent_emits_negative_step() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b00) == -1);
}

void repeated_same_state_does_not_emit() {
  ai_keyboard::EncoderDecoder decoder;

  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b00) == 0);
}

void reset_to_nonzero_state_does_not_accumulate_a_partial_step() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b01);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void pause_mid_detent_resumes_without_losing_the_step() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b00);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void invalid_transition_is_counted_and_the_next_detent_recovers() {
  ai_keyboard::EncoderDecoder decoder;

  decoder.reset(0b00);
  assert(decoder.update(0b11) == 0);
  assert(decoder.invalid_transition_count() == 1);
  assert(decoder.update(0b00) == 0);
  assert(decoder.update(0b01) == 0);
  assert(decoder.update(0b11) == 0);
  assert(decoder.update(0b10) == 0);
  assert(decoder.update(0b00) == 1);
}

void ordered_step_queue_coalesces_runs_but_preserves_reversals() {
  ai_keyboard::EncoderStepQueue queue;
  assert(queue.push(1, 10, 100));
  assert(queue.push(2, 11, 101));
  assert(queue.push(-1, 12, 102));
  assert(queue.push(-3, 13, 103));
  assert(queue.push(1, 14, 104));
  assert(queue.size() == 3);

  ai_keyboard::EncoderStepRun run;
  assert(queue.pop(&run));
  assert(run.steps == 3);
  assert(run.first_timestamp_ms == 10);
  assert(run.last_timestamp_ms == 11);
  assert(run.first_order_sequence == 100);
  assert(run.last_order_sequence == 101);
  assert(queue.pop(&run));
  assert(run.steps == -4);
  assert(run.first_timestamp_ms == 12);
  assert(run.last_timestamp_ms == 13);
  assert(queue.pop(&run));
  assert(run.steps == 1);
  assert(run.first_timestamp_ms == 14);
  assert(queue.empty());
}

void gpio_edge_fence_splits_same_direction_runs_for_ordering() {
  ai_keyboard::EncoderStepQueue queue;
  assert(queue.push(1, 20));
  queue.break_coalescing();
  assert(queue.push(2, 22));
  assert(queue.size() == 2);

  ai_keyboard::EncoderStepRun run;
  assert(queue.peek(&run));
  assert(run.steps == 1);
  assert(run.first_timestamp_ms == 20);
  assert(queue.pop(&run));
  assert(queue.pop(&run));
  assert(run.steps == 2);
  assert(run.first_timestamp_ms == 22);
}

void rapid_detents_remain_a_single_ordered_run() {
  ai_keyboard::EncoderDecoder decoder;
  ai_keyboard::EncoderStepQueue queue;
  decoder.reset(0b00);

  for (int detent = 0; detent < 12; ++detent) {
    assert(decoder.update(0b01) == 0);
    assert(decoder.update(0b11) == 0);
    assert(decoder.update(0b10) == 0);
    assert(queue.push(decoder.update(0b00), 100 + detent));
  }

  ai_keyboard::EncoderStepRun run;
  assert(queue.pop(&run));
  assert(run.steps == 12);
  assert(run.first_timestamp_ms == 100);
  assert(run.last_timestamp_ms == 111);
  assert(queue.empty());
}

void very_large_same_direction_backlog_splits_before_distance_overflow() {
  ai_keyboard::EncoderStepQueue queue;
  for (int step = 0;
       step < ai_keyboard::kEncoderStepRunMagnitudeLimit + 1;
       ++step) {
    assert(queue.push(1, static_cast<std::uint32_t>(step)));
  }
  assert(queue.size() == 2);
  ai_keyboard::EncoderStepRun run;
  assert(queue.pop(&run));
  assert(run.steps == ai_keyboard::kEncoderStepRunMagnitudeLimit);
  assert(queue.pop(&run));
  assert(run.steps == 1);
}

void claimed_head_cannot_absorb_a_concurrent_same_direction_detent() {
  ai_keyboard::EncoderStepQueue queue;
  assert(queue.push(3, 100, 40));

  ai_keyboard::EncoderStepRun claimed;
  assert(queue.claim(&claimed));
  assert(claimed.steps == 3);
  assert(claimed.first_order_sequence == 40);

  // Models an ISR detent arriving between owner callback admission and pop.
  assert(queue.push(2, 101, 44));
  assert(queue.size() == 2);

  ai_keyboard::EncoderStepRun accepted;
  assert(queue.pop(&accepted));
  assert(accepted.steps == claimed.steps);
  assert(accepted.first_order_sequence == claimed.first_order_sequence);
  assert(queue.pop(&accepted));
  assert(accepted.steps == 2);
  assert(accepted.first_order_sequence == 44);
}

void reversal_spool_has_a_large_explicit_backpressure_boundary() {
  ai_keyboard::EncoderStepQueue queue;
  for (std::size_t index = 0;
       index < ai_keyboard::kEncoderStepQueueCapacity;
       ++index) {
    const int step = index % 2 == 0 ? 1 : -1;
    assert(queue.push(step,
                      static_cast<std::uint32_t>(index),
                      static_cast<std::uint32_t>(index + 1)));
  }
  assert(queue.size() == ai_keyboard::kEncoderStepQueueCapacity);
  assert(!queue.push(1, 1000, 1000));
}

void claimed_head_keeps_the_full_reversal_spool_available_to_isr() {
  ai_keyboard::EncoderStepQueue queue;
  assert(queue.push(7, 10, 1));
  ai_keyboard::EncoderStepRun claimed;
  assert(queue.claim(&claimed));

  for (std::size_t index = 0;
       index < ai_keyboard::kEncoderStepQueueCapacity;
       ++index) {
    const int step = index % 2 == 0 ? 1 : -1;
    assert(queue.push(step,
                      static_cast<std::uint32_t>(20 + index),
                      static_cast<std::uint32_t>(2 + index)));
  }
  assert(queue.size() == ai_keyboard::kEncoderStepQueueCapacity + 1);
  assert(!queue.push(1, 2000, 2000));

  ai_keyboard::EncoderStepRun accepted;
  assert(queue.pop(&accepted));
  assert(accepted.steps == claimed.steps);
  assert(accepted.first_order_sequence == claimed.first_order_sequence);
  assert(queue.size() == ai_keyboard::kEncoderStepQueueCapacity);
}

int main() {
  clockwise_detent_emits_positive_step();
  counterclockwise_detent_emits_negative_step();
  repeated_same_state_does_not_emit();
  reset_to_nonzero_state_does_not_accumulate_a_partial_step();
  pause_mid_detent_resumes_without_losing_the_step();
  invalid_transition_is_counted_and_the_next_detent_recovers();
  ordered_step_queue_coalesces_runs_but_preserves_reversals();
  gpio_edge_fence_splits_same_direction_runs_for_ordering();
  rapid_detents_remain_a_single_ordered_run();
  very_large_same_direction_backlog_splits_before_distance_overflow();
  claimed_head_cannot_absorb_a_concurrent_same_direction_detent();
  reversal_spool_has_a_large_explicit_backpressure_boundary();
  claimed_head_keeps_the_full_reversal_spool_available_to_isr();
  return 0;
}
