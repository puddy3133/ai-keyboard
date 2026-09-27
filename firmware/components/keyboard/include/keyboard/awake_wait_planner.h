#pragma once

#include <cstdint>

namespace ai_keyboard {

struct AwakeWaitDecision {
  bool immediate = false;
  bool has_deadline = false;
  std::uint32_t wait_ms = 0;
  const char* reason = "none";
};

struct OwnerServiceSchedule {
  // Outstanding work blocks a Deep Sleep commit, but it does not imply that
  // polling it again immediately can make progress.
  bool outstanding = false;
  bool runnable_now = false;
  bool deadline_armed = false;
  std::uint32_t deadline_ms = 0;
};

// Collects real work and absolute business deadlines for the single Awake
// state. Deadlines must be no farther than INT32_MAX milliseconds from now;
// every firmware deadline is currently at most 30 minutes.
class AwakeWaitPlanner {
 public:
  explicit AwakeWaitPlanner(std::uint32_t now_ms);

  void request_now(bool requested, const char* reason);
  void add_deadline(bool armed,
                    std::uint32_t absolute_deadline_ms,
                    const char* reason);
  void add_schedule(const OwnerServiceSchedule& schedule,
                    const char* reason);
  AwakeWaitDecision decision() const;

 private:
  std::uint32_t now_ms_ = 0;
  bool immediate_ = false;
  bool has_deadline_ = false;
  std::uint32_t deadline_ms_ = 0;
  const char* reason_ = "none";
};

}  // namespace ai_keyboard
