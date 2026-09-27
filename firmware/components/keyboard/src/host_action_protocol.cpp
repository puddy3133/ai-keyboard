#include "keyboard/host_action_protocol.h"

#include <algorithm>

namespace ai_keyboard {
namespace {

bool is_uuid_hyphen_position(std::size_t index) {
  return index == 8 || index == 13 || index == 18 || index == 23;
}

bool is_lowercase_hex(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f');
}

}  // namespace

bool is_canonical_host_action_value(std::string_view action) {
  if (action.size() != kHostActionPrefixLen + kHostActionV1UuidLen ||
      action.substr(0, kHostActionPrefixLen) != kHostActionPrefix) {
    return false;
  }

  const auto uuid = action.substr(kHostActionPrefixLen);
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (is_uuid_hyphen_position(index)) {
      if (uuid[index] != '-') {
        return false;
      }
    } else if (!is_lowercase_hex(uuid[index])) {
      return false;
    }
  }
  return true;
}

bool encode_host_action_v1(std::string_view action,
                           HostActionV1Report* out) {
  if (out == nullptr || !is_canonical_host_action_value(action)) {
    return false;
  }

  HostActionV1Report encoded;
  encoded.report_id = kHostActionV1ReportId;
  encoded.payload[0] = kHostActionV1CommandKind;
  encoded.payload[1] = kHostActionV1ChunkIndex;
  encoded.payload[2] = kHostActionV1TotalChunks;
  encoded.payload[3] = static_cast<std::uint8_t>(kHostActionV1UuidLen);
  const auto uuid = action.substr(kHostActionPrefixLen);
  std::copy(uuid.begin(), uuid.end(),
            encoded.payload.begin() + kHostActionV1HeaderLen);
  *out = encoded;
  return true;
}

}  // namespace ai_keyboard
