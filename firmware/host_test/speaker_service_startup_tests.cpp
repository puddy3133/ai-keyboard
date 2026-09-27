#include <cassert>

#include "keyboard/speaker_service_startup.h"

int main() {
  using ai_keyboard::SpeakerServiceStartupInputs;
  using ai_keyboard::SpeakerWifiAdmissionState;
  using ai_keyboard::evaluate_speaker_service_startup;

  const SpeakerServiceStartupInputs local_missing{
      false,
      SpeakerWifiAdmissionState::NotAttempted,
      false,
      true,
  };
  const auto local_missing_decision =
      evaluate_speaker_service_startup(local_missing);
  assert(local_missing_decision.wait_for_local);
  assert(!local_missing_decision.attempt_wifi);
  assert(!local_missing_decision.boot_allowed);

  const SpeakerServiceStartupInputs wifi_not_attempted{
      true,
      SpeakerWifiAdmissionState::NotAttempted,
      false,
      false,
  };
  const auto wifi_not_attempted_decision =
      evaluate_speaker_service_startup(wifi_not_attempted);
  assert(!wifi_not_attempted_decision.wait_for_local);
  assert(wifi_not_attempted_decision.attempt_wifi);
  // The owner performs the attempt first, then may enter ResolveBoot in this
  // same pass regardless of whether allocation starts or fully rolls back.
  assert(wifi_not_attempted_decision.boot_allowed);

  const SpeakerServiceStartupInputs wifi_started{
      true,
      SpeakerWifiAdmissionState::Started,
      false,
      false,
  };
  const auto wifi_started_decision =
      evaluate_speaker_service_startup(wifi_started);
  assert(!wifi_started_decision.attempt_wifi);
  assert(wifi_started_decision.boot_allowed);

  const SpeakerServiceStartupInputs audio_unavailable{
      true,
      SpeakerWifiAdmissionState::Unavailable,
      false,
      false,
  };
  // A failed microphone/Wi-Fi audio owner must not suppress the independent
  // local boot sound or USB sound management service.
  const auto audio_unavailable_decision =
      evaluate_speaker_service_startup(audio_unavailable);
  assert(!audio_unavailable_decision.attempt_wifi);
  assert(audio_unavailable_decision.boot_allowed);

  const SpeakerServiceStartupInputs wifi_deferred_during_boot{
      true,
      SpeakerWifiAdmissionState::Deferred,
      false,
      true,
  };
  // The persistent Wi-Fi management carrier receives one priority admission
  // attempt. A fully rolled-back transient failure must not cancel the
  // independent local Boot sound transaction.
  const auto deferred_during_boot_decision =
      evaluate_speaker_service_startup(wifi_deferred_during_boot);
  assert(!deferred_during_boot_decision.attempt_wifi);
  assert(deferred_during_boot_decision.boot_allowed);

  const SpeakerServiceStartupInputs wifi_deferred_retry_not_due{
      true,
      SpeakerWifiAdmissionState::Deferred,
      true,
      false,
  };
  assert(!evaluate_speaker_service_startup(
              wifi_deferred_retry_not_due)
              .attempt_wifi);

  const SpeakerServiceStartupInputs wifi_deferred_retry_due{
      true,
      SpeakerWifiAdmissionState::Deferred,
      true,
      true,
  };
  const auto deferred_retry_due_decision =
      evaluate_speaker_service_startup(wifi_deferred_retry_due);
  assert(deferred_retry_due_decision.attempt_wifi);
  assert(deferred_retry_due_decision.boot_allowed);

  return 0;
}
