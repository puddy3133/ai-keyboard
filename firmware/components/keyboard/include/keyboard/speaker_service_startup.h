#pragma once

#include <cstdint>

namespace ai_keyboard {

// Wi-Fi sound synchronization and local Boot playback share internal heap but
// have independent outcomes. The carrier receives one allocation attempt
// before Boot; a fully rolled-back failure is deferred until Boot has released
// its resources instead of being misreported as a failed speaker self-test.
enum class SpeakerWifiAdmissionState : std::uint8_t {
  NotAttempted,
  Started,
  Deferred,
  Unavailable,
};

struct SpeakerServiceStartupInputs {
  bool local_ready = false;
  SpeakerWifiAdmissionState wifi_admission =
      SpeakerWifiAdmissionState::NotAttempted;
  bool boot_resources_released = false;
  bool wifi_retry_due = false;
};

struct SpeakerServiceStartupDecision {
  bool wait_for_local = false;
  bool attempt_wifi = false;
  bool boot_allowed = false;
};

SpeakerServiceStartupDecision evaluate_speaker_service_startup(
    const SpeakerServiceStartupInputs& inputs);

}  // namespace ai_keyboard
