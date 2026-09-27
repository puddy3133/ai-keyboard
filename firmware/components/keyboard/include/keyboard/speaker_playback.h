#pragma once

#include <cstddef>
#include <cstdint>

namespace ai_keyboard {

enum class SpeakerPlaybackPhase : std::uint8_t {
  Idle,
  Starting,
  Playing,
  Draining,
};

enum class SpeakerPlaybackResult : std::uint8_t {
  None,
  Succeeded,
  Cancelled,
  Failed,
};

struct SpeakerPlaybackTicket {
  bool accepted = false;
  std::uint32_t generation = 0;
};

// One owner-loop observation may contain both edges. Keeping them independent
// is important for very short sounds: the worker can publish first PCM and
// terminal completion before the platform owner next runs.
struct SpeakerPlaybackEvents {
  std::uint32_t first_pcm_generation = 0;
  std::uint32_t terminal_generation = 0;
  SpeakerPlaybackResult terminal_result = SpeakerPlaybackResult::None;

  bool first_pcm() const { return first_pcm_generation != 0; }
  bool terminal() const { return terminal_generation != 0; }
};

class SpeakerPlayback {
 public:
  static constexpr std::size_t kCapacity = 1;

  SpeakerPlaybackTicket request();
  bool mark_started(std::uint32_t generation);
  bool finish(std::uint32_t generation);
  bool cancel(std::uint32_t generation);
  bool fail(std::uint32_t generation);
  bool mark_drained(std::uint32_t generation);

  // Folds the worker's generation-tagged publications into the owner state
  // machine and returns one-shot observable edges. Zero means "not
  // published". A matching first-PCM publication is reported exactly once,
  // independently of the terminal edge, even if both arrive in one poll.
  SpeakerPlaybackEvents consume_worker_events(
      std::uint32_t first_pcm_generation,
      std::uint32_t terminal_generation,
      SpeakerPlaybackResult terminal_result);

  SpeakerPlaybackPhase phase() const;
  std::uint32_t active_generation() const;
  bool active() const;
  bool sleep_blocked() const;
  bool power_required() const;
  SpeakerPlaybackResult pending_result() const;
  SpeakerPlaybackResult last_result() const;
  std::uint32_t last_completed_generation() const;

 private:
  bool begin_drain(std::uint32_t generation, SpeakerPlaybackResult result);
  bool matches(std::uint32_t generation) const;
  std::uint32_t next_generation();

  SpeakerPlaybackPhase phase_ = SpeakerPlaybackPhase::Idle;
  SpeakerPlaybackResult pending_result_ = SpeakerPlaybackResult::None;
  SpeakerPlaybackResult last_result_ = SpeakerPlaybackResult::None;
  std::uint32_t generation_counter_ = 0;
  std::uint32_t active_generation_ = 0;
  std::uint32_t last_completed_generation_ = 0;
  std::uint32_t consumed_first_pcm_generation_ = 0;
};

}  // namespace ai_keyboard
