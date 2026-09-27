#include <cassert>
#include <cstdint>

#include "keyboard/encoder_press_gesture.h"

namespace {

void short_press_remains_a_click() {
  ai_keyboard::EncoderPressGesture gesture;
  gesture.press(10U);
  const auto released = gesture.release();
  assert(released.dispatch_click);
}

void no_rotation_preserves_the_three_second_config_entry() {
  ai_keyboard::EncoderPressGesture gesture;
  gesture.press(0xFFFFFF00U);
  assert(!gesture.trigger_config_if_due(0x00000050U, 1000U, true));
  assert(gesture.trigger_config_if_due(0x00000320U, 1000U, true));
  assert(gesture.config_triggered());

  const auto released = gesture.release();
  assert(!released.dispatch_click);
  assert(released.ignored_after_config);
}

void sampled_time_has_an_exact_three_second_boundary() {
  ai_keyboard::EncoderPressGesture before_deadline;
  before_deadline.press(100U);
  assert(!before_deadline.trigger_config_if_due(3099U, 3000U, true));
  assert(before_deadline.release().dispatch_click);

  ai_keyboard::EncoderPressGesture at_deadline;
  at_deadline.press(100U);
  assert(at_deadline.trigger_config_if_due(3100U, 3000U, true));

  ai_keyboard::EncoderPressGesture after_deadline;
  after_deadline.press(100U);
  assert(after_deadline.trigger_config_if_due(3101U, 3000U, true));
}

}  // namespace

int main() {
  short_press_remains_a_click();
  no_rotation_preserves_the_three_second_config_entry();
  sampled_time_has_an_exact_three_second_boundary();
  return 0;
}
