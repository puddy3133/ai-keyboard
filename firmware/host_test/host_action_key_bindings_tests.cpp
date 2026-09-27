#include <array>
#include <cassert>
#include <cstddef>
#include <string>

#include "keyboard/config_payload.h"
#include "keyboard/keymap.h"

namespace {

struct KeyBinding {
  const char* name;
  ai_keyboard::InputId input;
};

constexpr std::array<KeyBinding, 8> kMainKeys{{
    {"KEY1", ai_keyboard::InputId::Key1},
    {"KEY2", ai_keyboard::InputId::Key2},
    {"KEY3", ai_keyboard::InputId::Key3},
    {"KEY4", ai_keyboard::InputId::Key4},
    {"KEY5", ai_keyboard::InputId::Key5},
    {"KEY6", ai_keyboard::InputId::Key6},
    {"KEY7", ai_keyboard::InputId::Key7},
    {"KEY8", ai_keyboard::InputId::Key8},
}};

std::string payload_with_host_action_at(std::size_t target,
                                        const std::string& host_action) {
  std::string payload =
      R"({"schema":"ai_keyboard.v1","profiles":[{"id":"default","keys":{)";
  for (std::size_t index = 0; index < kMainKeys.size(); ++index) {
    if (index != 0) {
      payload += ',';
    }
    payload += '"';
    payload += kMainKeys[index].name;
    payload += R"(":{"press":")";
    payload += index == target ? host_action : "disabled";
    payload += R"("})";
  }
  payload +=
      R"(},"encoder":{"left":"disabled","right":"disabled","press":"disabled"}}]})";
  return payload;
}

void every_main_key_preserves_and_emits_one_host_action_per_press_cycle() {
  const std::string configured_action =
      "host_action:123e4567-e89b-12d3-a456-426614174000";

  for (std::size_t target = 0; target < kMainKeys.size(); ++target) {
    const auto result = ai_keyboard::parse_config_payload(
        payload_with_host_action_at(target, configured_action));
    assert(result.status == ai_keyboard::ConfigParseStatus::Ok);

    for (std::size_t index = 0; index < kMainKeys.size(); ++index) {
      const auto& action =
          result.config.keymap.action_for(kMainKeys[index].input);
      if (index == target) {
        assert(action.kind == ai_keyboard::ActionKind::HostAction);
        assert(action.host_action == configured_action);
        assert(action.host_action.rfind("host_action:", 0) == 0);
      } else {
        assert(action.kind == ai_keyboard::ActionKind::Disabled);
        assert(action.host_action.empty());
      }
    }

    const auto& action =
        result.config.keymap.action_for(kMainKeys[target].input);
    const std::array<ai_keyboard::InputPhase, 2> cycle{{
        ai_keyboard::InputPhase::Pressed,
        ai_keyboard::InputPhase::Released,
    }};
    std::size_t host_action_events = 0;
    for (const auto phase : cycle) {
      const auto event = ai_keyboard::event_for_action(
          action, phase, "RightMeta", "RightOption");
      if (event.kind == ai_keyboard::FirmwareEventKind::HostAction) {
        ++host_action_events;
        assert(phase == ai_keyboard::InputPhase::Pressed);
        assert(event.value == configured_action);
      } else {
        assert(phase == ai_keyboard::InputPhase::Released);
        assert(event.kind == ai_keyboard::FirmwareEventKind::None);
        assert(event.value.empty());
      }
    }
    assert(host_action_events == 1);
  }
}

}  // namespace

int main() {
  every_main_key_preserves_and_emits_one_host_action_per_press_cycle();
  return 0;
}
