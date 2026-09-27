#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ai_keyboard {

inline constexpr std::string_view kHostActionPrefix = "host_action:";
inline constexpr std::size_t kHostActionPrefixLen = 12;
inline constexpr std::size_t kHostActionV1UuidLen = 36;
inline constexpr std::uint8_t kHostActionV1ReportId = 0x11;
inline constexpr std::uint8_t kHostActionV1CommandKind = 0x05;
inline constexpr std::uint8_t kHostActionV1ChunkIndex = 0;
inline constexpr std::uint8_t kHostActionV1TotalChunks = 1;
inline constexpr std::size_t kHostActionV1PayloadLen = 63;
inline constexpr std::size_t kHostActionV1HeaderLen = 4;

static_assert(kHostActionPrefix.size() == kHostActionPrefixLen);
static_assert(kHostActionV1HeaderLen + kHostActionV1UuidLen <=
              kHostActionV1PayloadLen);

struct HostActionV1Report {
  std::uint8_t report_id = 0;
  std::array<std::uint8_t, kHostActionV1PayloadLen> payload{};
};

bool is_canonical_host_action_value(std::string_view action);

// Encodes the existing 63-byte App Command container. The configuration
// prefix is validated but not copied into the 36-byte Host Action data field.
bool encode_host_action_v1(std::string_view action,
                           HostActionV1Report* out);

}  // namespace ai_keyboard
