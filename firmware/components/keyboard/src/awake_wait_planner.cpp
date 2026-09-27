#include "keyboard/awake_wait_planner.h"

#include <cstdint>

namespace ai_keyboard {
namespace {

bool deadline_reached(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
}

bool deadline_precedes(std::uint32_t candidate_ms,
                       std::uint32_t current_ms) {
  return static_cast<std::int32_t>(candidate_ms - current_ms) < 0;
}

}  // namespace

AwakeWaitPlanner::AwakeWaitPlanner(std::uint32_t now_ms) : now_ms_(now_ms) {}

void AwakeWaitPlanner::request_now(bool requested, const char* reason) {
  if (!requested || immediate_) {
    return;
  }
  immediate_ = true;
  reason_ = reason == nullptr ? "work" : reason;
}

void AwakeWaitPlanner::add_deadline(bool armed,
                                    std::uint32_t absolute_deadline_ms,
                                    const char* reason) {
  if (!armed || immediate_) {
    return;
  }
  if (deadline_reached(now_ms_, absolute_deadline_ms)) {
    immediate_ = true;
    has_deadline_ = true;
    deadline_ms_ = absolute_deadline_ms;
    reason_ = reason == nullptr ? "deadline" : reason;
    return;
  }
  if (!has_deadline_ || deadline_precedes(absolute_deadline_ms, deadline_ms_)) {
    has_deadline_ = true;
    deadline_ms_ = absolute_deadline_ms;
    reason_ = reason == nullptr ? "deadline" : reason;
  }
}

void AwakeWaitPlanner::add_schedule(const OwnerServiceSchedule& schedule,
                                    const char* reason) {
  request_now(schedule.runnable_now, reason);
  add_deadline(schedule.deadline_armed, schedule.deadline_ms, reason);
}

AwakeWaitDecision AwakeWaitPlanner::decision() const {
  if (immediate_) {
    return {true, has_deadline_, 0, reason_};
  }
  if (!has_deadline_) {
    return {false, false, 0, "notification"};
  }
  return {false, true, deadline_ms_ - now_ms_, reason_};
}

}  // namespace ai_keyboard
