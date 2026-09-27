#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "keyboard/host_action_protocol.h"
#include "keyboard/status_hid_protocol.h"

namespace {

void accepts_canonical_lowercase_uuid_without_semantic_uuid_restrictions() {
  const std::vector<std::string> accepted{
      "host_action:123e4567-e89b-12d3-a456-426614174000",
      "host_action:00000000-0000-0000-0000-000000000000",
      "host_action:ffffffff-ffff-ffff-ffff-ffffffffffff",
  };

  for (const auto& action : accepted) {
    assert(ai_keyboard::is_canonical_host_action_value(action));
  }
}

void rejects_noncanonical_uuid_and_leaves_output_untouched() {
  const std::vector<std::string> rejected{
      "host_action:123E4567-e89b-12d3-a456-426614174000",
      "host_action:123e4567-e89b-12d3-a456-42661417400",
      "host_action:123e456-7e89b-12d3-a456-426614174000",
      "host_action:123e4567-e89b-12d3-a456-42661417400g",
      "host-action:123e4567-e89b-12d3-a456-426614174000",
  };

  for (const auto& action : rejected) {
    assert(!ai_keyboard::is_canonical_host_action_value(action));

    ai_keyboard::HostActionV1Report report;
    report.report_id = 0xAA;
    report.payload.fill(0xAA);
    assert(!ai_keyboard::encode_host_action_v1(action, &report));
    assert(report.report_id == 0xAA);
    for (const auto byte : report.payload) {
      assert(byte == 0xAA);
    }
  }
}

void encodes_the_frozen_v1_report_without_the_config_prefix() {
  static_assert(ai_keyboard::kHostActionV1ReportId == 0x11);
  static_assert(ai_keyboard::kHostActionV1CommandKind == 0x05);
  static_assert(ai_keyboard::kHostActionV1ChunkIndex == 0);
  static_assert(ai_keyboard::kHostActionV1TotalChunks == 1);
  static_assert(ai_keyboard::kHostActionV1UuidLen == 36);
  static_assert(ai_keyboard::kHostActionV1PayloadLen == 63);
  static_assert(ai_keyboard::kStatusResponseCommandKind == 0x04);
  static_assert(ai_keyboard::kHostActionV1CommandKind !=
                ai_keyboard::kStatusResponseCommandKind);

  const std::string action =
      "host_action:123e4567-e89b-12d3-a456-426614174000";
  const std::string uuid = "123e4567-e89b-12d3-a456-426614174000";
  ai_keyboard::HostActionV1Report report;

  assert(ai_keyboard::encode_host_action_v1(action, &report));
  assert(report.report_id == 0x11);
  assert(report.payload[0] == 0x05);
  assert(report.payload[1] == 0);
  assert(report.payload[2] == 1);
  assert(report.payload[3] == 36);
  const std::string encoded_uuid(
      reinterpret_cast<const char*>(report.payload.data() + 4),
      ai_keyboard::kHostActionV1UuidLen);
  assert(encoded_uuid == uuid);
  assert(encoded_uuid.find("host_action:") == std::string::npos);
  for (std::size_t index = 40; index < report.payload.size(); ++index) {
    assert(report.payload[index] == 0);
  }
}

}  // namespace

int main() {
  accepts_canonical_lowercase_uuid_without_semantic_uuid_restrictions();
  rejects_noncanonical_uuid_and_leaves_output_untouched();
  encodes_the_frozen_v1_report_without_the_config_prefix();
  return 0;
}
