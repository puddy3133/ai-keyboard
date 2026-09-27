#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "keyboard/ble_status_wire.h"
#include "keyboard/config_status.h"

namespace {

struct PublishedVariant {
  const char* name;
  std::string base;
  std::string wire;
};

std::size_t count_occurrences(const std::string& value,
                              const std::string& needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = value.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

void assert_host_action_v1_capability(const std::string& json) {
  constexpr char kCapability[] = R"("host_action_v1":true)";
  assert(count_occurrences(json, kCapability) == 1);
  assert(json.find(R"("host_action_v1":"true")") == std::string::npos);
  assert(json.find(R"("host_action_v1":false)") == std::string::npos);
  const auto capabilities_begin = json.find(R"("capabilities":{)");
  const auto capabilities_end = json.find('}', capabilities_begin);
  const auto capability = json.find(kCapability, capabilities_begin);
  assert(capabilities_begin != std::string::npos);
  assert(capabilities_end != std::string::npos);
  assert(capability != std::string::npos);
  assert(capability < capabilities_end);
}

ai_keyboard::BleStatusWireSnapshot worst_case_battery_wire() {
  return {
      true,
      true,
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::uint16_t>::max(),
  };
}

ai_keyboard::BleDetailedStatusWireSnapshot worst_case_detailed_wire() {
  const auto max = std::numeric_limits<std::uint32_t>::max();
  return {
      true,
      true,
      std::numeric_limits<std::uint16_t>::max(),
      true,
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::int32_t>::min(),
      max,
      max,
      max,
      max,
      max,
      max,
      max,
      max,
      max,
      max,
      max,
  };
}

ai_keyboard::ConfigStatusSnapshot full_snapshot() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.47";
  snapshot.phase = "status";
  snapshot.status = "cached";
  snapshot.bytes = 128;
  snapshot.crc16 = 4660;
  snapshot.ptt_hotkey = "F12";
  snapshot.edit_ptt_hotkey = "F14";
  snapshot.hotkey_mode = "toggle";
  snapshot.saved = true;
  snapshot.battery_mv = 3800;
  snapshot.battery_percent = 55;
  snapshot.target_platform = "macos";
  return snapshot;
}

ai_keyboard::ConfigStatusSnapshot compact_snapshot() {
  auto snapshot = full_snapshot();
  snapshot.firmware = "0.4.47-idf-v2-host-action-capability";
  snapshot.status = std::string(64, 's');
  snapshot.audio.enabled = true;
  snapshot.audio.source = "wifi_udp";
  snapshot.audio.microphone_source = "keyboard";
  snapshot.audio.capture = "mic_streaming";
  snapshot.audio.host = "192.168.255.255";
  snapshot.audio.port = 65535;
  snapshot.audio.control_state = "ready loops=4294967295 hb=4294967295";
  snapshot.audio.last_error = std::string(240, 'e');
  snapshot.audio.sent_packets = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.sent_bytes = std::numeric_limits<std::uint32_t>::max();
  snapshot.audio.session_id = std::numeric_limits<std::uint64_t>::max();
  snapshot.power.mode = "awake";
  snapshot.power.inactive_ms = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.deep_sleep_block = std::string(64, 'd');
  snapshot.power.last_wake = std::string(64, 'w');
  snapshot.power.cycle_seq = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_inactive_ms = std::numeric_limits<std::uint32_t>::max();
  snapshot.power.cycle_deep_sleep = true;
  snapshot.power.cycle_wake = std::string(64, 'c');
  return snapshot;
}

ai_keyboard::ConfigStatusSnapshot battery_snapshot() {
  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = "0.4.47";
  snapshot.phase = "battery";
  snapshot.status = "fresh";
  snapshot.bytes = 128;
  snapshot.crc16 = 4660;
  snapshot.saved = true;
  snapshot.battery_mv = 3800;
  snapshot.battery_percent = 55;
  snapshot.target_platform = "macos";
  snapshot.battery = {3790, "battery", 24, true};
  snapshot.power.mode = "awake";
  snapshot.power.inactive_ms = 600000;
  snapshot.power.deep_sleep_block = "keyboard_mic";
  snapshot.power.last_wake = "deep_key";
  snapshot.power.cycle_seq = 8;
  snapshot.power.cycle_inactive_ms = 420000;
  snapshot.power.cycle_deep_sleep = true;
  snapshot.power.cycle_wake = "deep_key";
  return snapshot;
}

ai_keyboard::ConfigStatusSnapshot speaker_probe_snapshot(
    ai_keyboard::SpeakerProbeSnapshot* speaker) {
  const auto max = std::numeric_limits<std::uint32_t>::max();
  speaker->present = true;
  speaker->version = ai_keyboard::kSpeakerProbeStatusVersion;
  speaker->generation = max;
  speaker->stage = ai_keyboard::SpeakerProbeStage::Done;
  speaker->result = ai_keyboard::SpeakerProbeResult::Cancelled;
  speaker->error = ai_keyboard::SpeakerProbeError::Unknown;
  speaker->raw_error = std::numeric_limits<std::int32_t>::min();
  speaker->microphone_generation = max;
  speaker->first_submit_us = max;
  speaker->decode_total_us = max;
  speaker->decode_max_us = max;
  speaker->decoded_frames = max;
  speaker->decoded_pcm_bytes = max;
  speaker->stack_high_water_bytes = max;
  speaker->heap_begin_free = max;
  speaker->heap_terminal_free = max;
  speaker->heap_largest_block = max;
  speaker->heap_minimum_free = max;
  speaker->decoded_abs_peak = max;
  speaker->decoded_rms_permille = max;

  ai_keyboard::ConfigStatusSnapshot snapshot;
  snapshot.firmware = std::string(40, 'f');
  snapshot.phase = "spk_probe";
  snapshot.status = "cancelled";
  snapshot.bytes = std::numeric_limits<std::uint16_t>::max();
  snapshot.crc16 = std::numeric_limits<std::uint16_t>::max();
  snapshot.saved = true;
  snapshot.target_platform = "windows";
  snapshot.speaker = speaker;
  return snapshot;
}

void every_published_variant_has_one_boolean_capability_and_is_bounded() {
  const auto detailed = worst_case_detailed_wire();
  const auto full = ai_keyboard::build_config_status_json(full_snapshot());
  assert(full.find(R"("compact":true)") == std::string::npos);

  const auto compact = ai_keyboard::build_config_status_json(compact_snapshot());
  assert(compact.find(R"("compact":true)") != std::string::npos);

  ai_keyboard::SpeakerProbeSnapshot speaker;
  const auto speaker_probe = ai_keyboard::build_config_status_json(
      speaker_probe_snapshot(&speaker));
  assert(speaker_probe.find(R"("phase":"spk_probe")") != std::string::npos);

  const auto confirmation = ai_keyboard::build_config_confirmation_status_json(
      compact_snapshot());
  assert(confirmation.find(R"("bytes":128)") != std::string::npos);
  assert(confirmation.find(R"("audio")") == std::string::npos);

  const auto battery = ai_keyboard::build_config_status_json(battery_snapshot());
  assert(battery.find(R"("phase":"battery")") != std::string::npos);
  const auto battery_wire = ai_keyboard::append_ble_status_wire_json(
      battery, worst_case_battery_wire());
  assert(battery_wire.find(R"("ble":{)") != std::string::npos);

  const std::string fallback = ai_keyboard::kConfigStatusFallbackJson;
  std::vector<PublishedVariant> variants{
      {"full", full,
       ai_keyboard::append_ble_detailed_status_wire_json(full, detailed)},
      {"compact", compact,
       ai_keyboard::append_ble_detailed_status_wire_json(compact, detailed)},
      {"speaker_probe", speaker_probe,
       ai_keyboard::append_ble_detailed_status_wire_json(speaker_probe, detailed)},
      {"confirmation", confirmation,
       ai_keyboard::append_ble_detailed_status_wire_json(confirmation, detailed)},
      {"battery", battery, battery_wire},
      {"ble_fallback", fallback,
       ai_keyboard::append_ble_detailed_status_wire_json(fallback, detailed)},
  };

  std::size_t maximum = 0;
  for (const auto& variant : variants) {
    assert_host_action_v1_capability(variant.base);
    assert_host_action_v1_capability(variant.wire);
    assert(variant.wire.size() <= ai_keyboard::kConfigStatusGattSafeLen);
    maximum = std::max(maximum, variant.wire.size());
    std::cout << "STATUS_BYTES " << variant.name
              << " base=" << variant.base.size()
              << " final=" << variant.wire.size()
              << " remaining="
              << (ai_keyboard::kConfigStatusGattSafeLen - variant.wire.size())
              << '\n';
  }
  std::cout << "STATUS_MAX final=" << maximum
            << " limit=" << ai_keyboard::kConfigStatusGattSafeLen
            << " remaining="
            << (ai_keyboard::kConfigStatusGattSafeLen - maximum) << '\n';
}

}  // namespace

int main() {
  every_published_variant_has_one_boolean_capability_and_is_bounded();
  return 0;
}
