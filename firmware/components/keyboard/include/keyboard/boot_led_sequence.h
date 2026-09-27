#pragma once

#include <cstdint>

#include "keyboard/input_feedback.h"

namespace ai_keyboard {

enum class BootLedFrameKind : std::uint8_t {
  Pixel,
  AllPixels,
  Rainbow,
  Complete,
};

struct BootLedFrame {
  BootLedFrameKind kind = BootLedFrameKind::Complete;
  std::uint8_t pixel_index = 0;
};

struct BootLedDeferredPlayback {
  InputActivityFeedback feedback;
  std::uint32_t effect_until_ms = 0;
};

// Boot owns the physical strip while its self-test is visible. Runtime status
// events replay for their full duration afterwards; transient input feedback
// keeps its original expiry and is discarded when no longer useful.
class BootLedDeferredFeedback {
 public:
  void clear();
  void defer(const InputActivityFeedback& feedback,
             std::uint32_t now_ms,
             bool replay_full_duration_after_boot);
  bool pending() const;
  bool take(std::uint32_t now_ms, BootLedDeferredPlayback* playback);

 private:
  InputActivityFeedback feedback_;
  std::uint32_t effect_until_ms_ = 0;
  bool replay_full_duration_after_boot_ = false;
};

// Pure timing state for the V2 cold-boot LED self-test. The platform owner
// renders at most one due frame per pass and uses next_deadline_ms() to sleep
// until the next frame. Delayed owners never catch up by emitting a burst.
class BootLedSequence {
 public:
  static constexpr std::uint8_t kPixelCount = 5;
  static constexpr std::uint32_t kFrameHoldMs = 220;
  static constexpr std::uint32_t kRainbowOwnershipMs = 450;

  // Reserve visual ownership without starting the timeline. Reservation has
  // no frame and no deadline; it exists so runtime feedback can be deferred
  // until an authoritative Boot playback event starts the visible sequence.
  void reserve();
  bool reserved() const;

  // Starts at Pixel 0. The return value tells the platform whether this start
  // consumed an existing reservation, allowing feedback captured during that
  // reservation to survive the transition into the visible timeline.
  bool start(std::uint32_t now_ms);
  void cancel();
  bool active() const;
  bool take_due_frame(std::uint32_t now_ms, BootLedFrame* frame);
  bool next_deadline_ms(std::uint32_t* deadline_ms) const;

 private:
  enum class Phase : std::uint8_t {
    Idle,
    Reserved,
    Pixel,
    AllPixels,
    Rainbow,
    RainbowHold,
  };

  Phase phase_ = Phase::Idle;
  std::uint8_t pixel_index_ = 0;
  std::uint32_t deadline_ms_ = 0;
};

}  // namespace ai_keyboard
