#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

// Dedicated ISR-side direction spool. It absorbs rapid reversals while native
// keyboard selection chords are backpressured; overflow remains observable via
// diag.enc_drop instead of being mistaken for successful delivery.
constexpr std::size_t kEncoderStepQueueCapacity = 128;
// One run remains within the signed 32-bit pending-selection queue and below
// its 32767 accumulation cap.
constexpr int kEncoderStepRunMagnitudeLimit = 4095;

struct EncoderStepRun {
  int steps = 0;
  std::uint32_t first_timestamp_ms = 0;
  std::uint32_t last_timestamp_ms = 0;
  std::uint32_t first_order_sequence = 0;
  std::uint32_t last_order_sequence = 0;
};

class EncoderStepQueue {
 public:
  bool push(int step,
            std::uint32_t timestamp_ms = 0,
            std::uint32_t order_sequence = 0);
  bool peek(EncoderStepRun* run) const;
  // Moves the current head into a dedicated in-flight slot before an owner
  // callback attempts durable admission. A failed callback retries that exact
  // run, while ISR producers retain the full ring capacity for later detents.
  bool claim(EncoderStepRun* run);
  bool pop(EncoderStepRun* run);
  // A non-encoder GPIO edge is an ordering fence. The next detent must begin
  // a new run even when its direction matches the preceding run.
  void break_coalescing();
  void clear();
  bool empty() const;
  std::size_t size() const;

 private:
  std::array<EncoderStepRun, kEncoderStepQueueCapacity> runs_{};
  EncoderStepRun claimed_run_{};
  bool claimed_run_valid_ = false;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool coalescing_allowed_ = true;
};

class EncoderDecoder {
 public:
  void reset(std::uint8_t state);
  int update(std::uint8_t state);
  std::uint32_t invalid_transition_count() const;
  std::uint32_t partial_reset_count() const;

 private:
  std::uint8_t previous_state_ = 0;
  int accumulator_ = 0;
  bool armed_ = true;
  std::uint32_t invalid_transition_count_ = 0;
  std::uint32_t partial_reset_count_ = 0;
};

}  // namespace ai_keyboard
