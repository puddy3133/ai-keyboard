#include "keyboard/speaker_service_startup.h"

namespace ai_keyboard {

SpeakerServiceStartupDecision evaluate_speaker_service_startup(
    const SpeakerServiceStartupInputs& inputs) {
  if (!inputs.local_ready) {
    return {true, false, false};
  }

  switch (inputs.wifi_admission) {
    case SpeakerWifiAdmissionState::NotAttempted:
      // The owner executes this attempt before consuming boot_allowed in the
      // same pass, preserving the persistent service's allocation priority.
      return {false, true, true};
    case SpeakerWifiAdmissionState::Started:
    case SpeakerWifiAdmissionState::Unavailable:
      return {false, false, true};
    case SpeakerWifiAdmissionState::Deferred:
      return {
          false,
          inputs.boot_resources_released && inputs.wifi_retry_due,
          true,
      };
  }
  return {false, false, false};
}

}  // namespace ai_keyboard
