#include <cassert>
#include <cstdint>
#include <string_view>

#include "keyboard/awake_wait_planner.h"

namespace {

void blocks_for_notification_without_work_or_deadline() {
  const ai_keyboard::AwakeWaitPlanner planner(1'000);
  const auto decision = planner.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
  assert(decision.wait_ms == 0);
  assert(std::string_view(decision.reason) == "notification");
}

void real_work_preempts_every_deadline() {
  ai_keyboard::AwakeWaitPlanner planner(1'000);
  planner.add_deadline(true, 1'005, "debounce");
  planner.request_now(true, "transport");
  planner.add_deadline(true, 1'001, "late_add");
  const auto decision = planner.decision();
  assert(decision.immediate);
  assert(decision.wait_ms == 0);
  assert(std::string_view(decision.reason) == "transport");
}

void selects_the_nearest_business_deadline() {
  ai_keyboard::AwakeWaitPlanner planner(10'000);
  planner.add_deadline(true, 10'500, "audit");
  planner.add_deadline(true, 10'012, "wheel");
  planner.add_deadline(true, 10'080, "led");
  const auto decision = planner.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == 12);
  assert(std::string_view(decision.reason) == "wheel");
}

void due_deadline_requests_an_immediate_owner_pass() {
  ai_keyboard::AwakeWaitPlanner planner(500);
  planner.add_deadline(true, 499, "battery");
  const auto decision = planner.decision();
  assert(decision.immediate);
  assert(decision.has_deadline);
  assert(std::string_view(decision.reason) == "battery");
}

void deadline_ordering_is_uptime_wrap_safe() {
  constexpr std::uint32_t now = 0xFFFF'FFF0U;
  ai_keyboard::AwakeWaitPlanner planner(now);
  planner.add_deadline(true, now + 40U, "later");
  planner.add_deadline(true, now + 5U, "soon");
  const auto decision = planner.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == 5U);
  assert(std::string_view(decision.reason) == "soon");
}

void outstanding_work_does_not_mean_busy_polling() {
  ai_keyboard::AwakeWaitPlanner planner(20'000);
  planner.add_schedule(
      {true, false, true, 20'030}, "ble_hid_credit");
  auto decision = planner.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == 30);

  ai_keyboard::AwakeWaitPlanner callback_driven(20'000);
  callback_driven.add_schedule(
      {true, false, false, 0}, "usb_endpoint_callback");
  decision = callback_driven.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
}

void speaker_boot_preparing_waits_for_worker_notification_without_spin() {
  ai_keyboard::AwakeWaitPlanner planner(30'000);
  planner.add_schedule(
      {true, false, false, 0}, "speaker_boot");

  const auto decision = planner.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
  assert(decision.wait_ms == 0);
  assert(std::string_view(decision.reason) == "notification");
}

void speaker_boot_terminal_result_requests_owner_continuation() {
  ai_keyboard::AwakeWaitPlanner planner(30'000);
  planner.add_schedule(
      {true, true, false, 0}, "speaker_boot");

  const auto decision = planner.decision();
  assert(decision.immediate);
  assert(!decision.has_deadline);
  assert(decision.wait_ms == 0);
  assert(std::string_view(decision.reason) == "speaker_boot");
}

void speaker_boot_release_idle_requests_exactly_one_owner_continuation() {
  ai_keyboard::AwakeWaitPlanner release_completed(40'000);
  release_completed.request_now(true, "speaker_boot_idle");
  auto decision = release_completed.decision();
  assert(decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "speaker_boot_idle");

  // Once the owner consumes Idle and moves its startup phase to Ready, Idle
  // alone must not remain runnable or the Awake owner would busy-spin forever.
  const ai_keyboard::AwakeWaitPlanner owner_consumed_idle(40'000);
  decision = owner_consumed_idle.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "notification");
}

void speaker_boot_preparing_observes_the_input_quiet_deadline() {
  constexpr std::uint32_t quiet_deadline_ms = 50'030;
  ai_keyboard::AwakeWaitPlanner before_quiet(50'000);
  before_quiet.add_schedule(
      {true, false, false, 0}, "speaker_boot");
  before_quiet.add_deadline(
      true, quiet_deadline_ms, "speaker_assets_input_quiet");

  auto decision = before_quiet.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == 30);
  assert(std::string_view(decision.reason) ==
         "speaker_assets_input_quiet");

  ai_keyboard::AwakeWaitPlanner quiet_elapsed(quiet_deadline_ms);
  quiet_elapsed.add_schedule(
      {true, false, false, 0}, "speaker_boot");
  quiet_elapsed.add_deadline(
      true, quiet_deadline_ms, "speaker_assets_input_quiet");
  decision = quiet_elapsed.decision();
  assert(decision.immediate);
  assert(decision.has_deadline);
  assert(std::string_view(decision.reason) ==
         "speaker_assets_input_quiet");
}

void speaker_shutdown_settle_retries_are_bounded_without_busy_spinning() {
  constexpr std::uint32_t retry_ms = 2;
  ai_keyboard::AwakeWaitPlanner waiting_for_suspend(60'000);
  waiting_for_suspend.add_deadline(
      true, 60'000 + retry_ms, "speaker_shutdown_settle");
  auto decision = waiting_for_suspend.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == retry_ms);
  assert(std::string_view(decision.reason) == "speaker_shutdown_settle");

  // The deadline wakes one owner pass. If the task has not suspended yet, or
  // I2S disable/delete reports another transient error, re-arm a future retry
  // instead of returning to an unbounded wait or spinning at wait_ms == 0.
  ai_keyboard::AwakeWaitPlanner transient_retry(60'002);
  transient_retry.add_deadline(
      true, 60'002 + retry_ms, "speaker_shutdown_settle");
  decision = transient_retry.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == retry_ms);
}

void speaker_boot_waiting_for_shared_store_reenters_when_the_slot_frees() {
  ai_keyboard::AwakeWaitPlanner store_busy(70'000);
  store_busy.add_schedule(
      {true, false, false, 0}, "speaker_boot");
  auto decision = store_busy.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "notification");

  // The shared Store completion notification wakes assets.poll(). Once that
  // poll consumes the protocol job and frees the slot, ResolveBoot is locally
  // runnable even though no BLE/USB/LED notification remains outstanding.
  ai_keyboard::AwakeWaitPlanner store_slot_free(70'000);
  store_slot_free.add_schedule(
      {true, true, false, 0}, "speaker_boot");
  decision = store_slot_free.decision();
  assert(decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "speaker_boot");
}

void speaker_assets_runtime_schedule_distinguishes_progress_from_waits() {
  // A mailbox record or Core lifecycle/ingress unit is durable local state.
  // After one callback notification is consumed, that state itself must keep
  // the owner running until the bounded synchronous work is drained.
  ai_keyboard::AwakeWaitPlanner mailbox_residual(80'000);
  mailbox_residual.add_schedule(
      {true, true, false, 0}, "speaker_assets");
  auto decision = mailbox_residual.decision();
  assert(decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "speaker_assets");

  // Priority-denied Store work cannot progress synchronously. It remains a
  // lifecycle/deep-sleep blocker but must wait for an input-owner event or the
  // explicit quiet-window deadline instead of busy polling.
  ai_keyboard::AwakeWaitPlanner priority_blocked(80'000);
  priority_blocked.add_schedule(
      {true, false, false, 0}, "speaker_assets");
  decision = priority_blocked.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
  assert(std::string_view(decision.reason) == "notification");

  // With priority restored and no Store job owning the shared slot, the same
  // retained Core action is immediately executable.
  ai_keyboard::AwakeWaitPlanner store_slot_free(80'000);
  store_slot_free.add_schedule(
      {true, true, false, 0}, "speaker_assets");
  decision = store_slot_free.decision();
  assert(decision.immediate);
  assert(std::string_view(decision.reason) == "speaker_assets");

  // An active Store worker or an endpoint-owned reply is callback-driven.
  // Neither is a reason to spin the platform owner.
  ai_keyboard::AwakeWaitPlanner callback_backpressure(80'000);
  callback_backpressure.add_schedule(
      {true, false, false, 0}, "speaker_assets");
  decision = callback_backpressure.decision();
  assert(!decision.immediate);
  assert(!decision.has_deadline);
}

void cold_boot_led_deadlines_do_not_serialize_speaker_progress() {
  constexpr std::uint32_t now_ms = 90'000;

  // A callback-driven speaker Store read may sleep while the LED owns the
  // nearest visual deadline. This is one owner wait, not a dependency edge.
  ai_keyboard::AwakeWaitPlanner waiting_for_both(now_ms);
  waiting_for_both.add_deadline(true, now_ms + 220U, "status_led");
  waiting_for_both.add_schedule(
      {true, false, false, 0}, "speaker_boot");
  auto decision = waiting_for_both.decision();
  assert(!decision.immediate);
  assert(decision.has_deadline);
  assert(decision.wait_ms == 220U);
  assert(std::string_view(decision.reason) == "status_led");

  // When the speaker callback publishes a terminal result, it immediately
  // preempts the later LED deadline. The next owner pass can start I2S while
  // the LED timeline remains armed for a future frame.
  ai_keyboard::AwakeWaitPlanner speaker_ready(now_ms);
  speaker_ready.add_deadline(true, now_ms + 220U, "status_led");
  speaker_ready.add_schedule(
      {true, true, false, 0}, "speaker_boot");
  decision = speaker_ready.decision();
  assert(decision.immediate);
  assert(std::string_view(decision.reason) == "speaker_boot");
}

}  // namespace

int main() {
  blocks_for_notification_without_work_or_deadline();
  real_work_preempts_every_deadline();
  selects_the_nearest_business_deadline();
  due_deadline_requests_an_immediate_owner_pass();
  deadline_ordering_is_uptime_wrap_safe();
  outstanding_work_does_not_mean_busy_polling();
  speaker_boot_preparing_waits_for_worker_notification_without_spin();
  speaker_boot_terminal_result_requests_owner_continuation();
  speaker_boot_release_idle_requests_exactly_one_owner_continuation();
  speaker_boot_preparing_observes_the_input_quiet_deadline();
  speaker_shutdown_settle_retries_are_bounded_without_busy_spinning();
  speaker_boot_waiting_for_shared_store_reenters_when_the_slot_frees();
  speaker_assets_runtime_schedule_distinguishes_progress_from_waits();
  cold_boot_led_deadlines_do_not_serialize_speaker_progress();
  return 0;
}
