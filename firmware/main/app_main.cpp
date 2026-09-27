#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "keyboard/agent_status.h"
#include "keyboard/audio_stub.h"
#include "keyboard/awake_wait_planner.h"
#include "keyboard/board_pins.h"
#include "keyboard/battery_estimator.h"
#include "keyboard/config_payload.h"
#include "keyboard/config_receiver.h"
#include "keyboard/config_status.h"
#include "keyboard/config_state.h"
#include "keyboard/cold_boot_feedback.h"
#include "keyboard/encoder_press_gesture.h"
#include "keyboard/held_keyboard_state.h"
#include "keyboard/hid_report_queue.h"
#include "keyboard/keyboard_snapshot_delivery.h"
#include "keyboard/keymap.h"
#include "keyboard/power_cycle.h"
#include "keyboard/power_policy.h"
#include "keyboard/platform_selection.h"
#include "keyboard/transport_routing.h"
#include "platform/battery_adc.h"
#include "platform/ble_hid.h"
#include "platform/cdc_light_control.h"
#include "platform/gpio_keys.h"
#include "platform/keyboard_audio.h"
#include "platform/led_strip_status.h"
#include "platform/nvs_store.h"
#include "platform/peripheral_power.h"
#include "platform/usb_hid.h"
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "platform/speaker_output.h"
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
#include "keyboard/speaker_service_startup.h"
#include "platform/speaker_assets_supervisor.h"
#include "speaker_assets/factory_boot_sound.h"
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)
#include "speaker_assets/diagnostic_link_anchor.h"
#endif
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "easy_input";
constexpr const char* kFirmwareName = "EasyInput AI";
#if defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-ima-probe";
#elif defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-opus-observe";
#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
constexpr const char* kFirmwareVersion =
    "0.4.40-idf-v2-spk-boot-probe";
#elif defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
constexpr const char* kFirmwareVersion =
    "0.4.53-idf-v2-ble-store-v1";
#else
constexpr const char* kFirmwareVersion = "0.4.40-idf-v2-audio-pool";
#endif
constexpr std::uint32_t kUsbDisconnectConfirmMs = 25;
// 电池供电且满足全部安全门槛时,超长空闲进入 deep sleep(微安级待机);
// 任意主键/旋钮按压经 KEY_WAKE 唤醒,代价是唤醒后需重启+蓝牙重连(~2-4s)。
constexpr std::uint32_t kDeepSleepAfterMs =
    ai_keyboard::kDefaultPowerPolicy.deep_sleep_after_ms;
constexpr std::uint32_t kAwakePowerSampleIntervalMs = 300000;
constexpr std::uint32_t kAwakeHeartbeatLogIntervalMs = 600000;
constexpr std::uint32_t kEncoderConfigModeHoldMs = 3000;
constexpr std::uint32_t kPlatformSelectionModeTimeoutMs = 10000;
constexpr int kEncoderScrollMaxReportMagnitude = 96;
constexpr int kEncoderWheelMaxChunkMagnitude = 24;
constexpr std::uint32_t kEncoderWheelFlushIntervalMs = 12;
constexpr int kEncoderCursorMaxTapsPerEvent = 4;
constexpr std::uint32_t kEncoderCursorTapHoldMs = 10;
constexpr std::uint32_t kEncoderCursorTapGapMs = 4;
constexpr std::uint32_t kEncoderLedFeedbackMinIntervalMs = 45;
constexpr std::uint16_t kBleStableConnIntervalMin = 12;
constexpr std::uint16_t kBleStableConnIntervalMax = 36;
constexpr std::uint16_t kBleStableConnLatency = 0;
constexpr std::uint16_t kBleStableConnSupervisionTimeout = 400;
constexpr std::uint32_t kSpeakerAssetsInputQuietMs = 30;
// A cold boot must always produce visible feedback. This bounds admission into
// the speaker worker, not the time at which sound should start: normal playback
// still starts the LEDs from its first submitted PCM frame. Once the worker has
// accepted the request, its bounded first-PCM/terminal result owns the outcome
// and input/deadline can no longer misclassify an unconsumed event as silence.
constexpr std::uint32_t kColdBootFeedbackMaxAdmissionWaitMs = 5000;
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
constexpr std::uint32_t kSpeakerAssetsRetryMs = 1000;
constexpr std::uint32_t kSpeakerShutdownSettleRetryMs = 10;
#endif
constexpr std::uint8_t kHidKeyArrowRight = 0x4F;
constexpr std::uint8_t kHidKeyArrowLeft = 0x50;
constexpr std::uint8_t kHidKeyArrowDown = 0x51;
constexpr std::uint8_t kHidKeyArrowUp = 0x52;
constexpr std::size_t kEncoderSelectionChordsPerFlush = 4;
constexpr std::uint32_t kRetainedPowerCycleMagic = 0x50435943;
constexpr std::uint16_t kRetainedPowerCycleVersion = 2;

struct RetainedPowerCycle {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t sequence;
  std::uint32_t inactive_ms;
  std::uint8_t reached_deep_sleep;
  std::uint8_t wake_reason;
  std::uint16_t reserved_tail = 0;
};

RTC_DATA_ATTR RetainedPowerCycle g_retained_power_cycle;

int encoder_step_count(int encoder_step) {
  const int magnitude = encoder_step < 0 ? -encoder_step : encoder_step;
  return magnitude == 0 ? 1 : magnitude;
}

std::int8_t hid_axis_value(int value) {
  return static_cast<std::int8_t>(
      std::clamp(value, -kEncoderScrollMaxReportMagnitude, kEncoderScrollMaxReportMagnitude));
}

using ChargeState = ai_keyboard::BatteryPowerState;

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
enum class SpeakerStartupPhase : std::uint8_t {
  StartLocal,
  ResolveBoot,
  BeginOutput,
  WaitPlayback,
  ShutdownOutput,
  ReleaseLease,
  WaitLeaseIdle,
  Ready,
};
#endif

struct AppContext {
  ai_keyboard::ConfigState config_state;
  easy_input::GpioInputScanner inputs;
  easy_input::PeripheralPowerController peripheral_power;
  easy_input::StatusLedStrip leds;
  ai_keyboard::ColdBootFeedbackCoordinator cold_boot_feedback;
  easy_input::NvsConfigStore config_store;
  easy_input::BatteryAdc battery;
  ai_keyboard::BatteryEstimator battery_estimator;
  easy_input::BleHidTransport ble;
  easy_input::UsbHidTransport usb;
  easy_input::CdcLightControl cdc_light;
  ai_keyboard::UsbPhysicalPresenceMonitor usb_physical_presence{
      kUsbDisconnectConfirmMs};
  ai_keyboard::AudioIoArbiter audio_io_arbiter;
  easy_input::KeyboardAudioLink audio;
  bool audio_ready = false;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  easy_input::SpeakerOutput speaker;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  bool speaker_probe_pending = false;
#endif
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  easy_input::SpeakerAssetsSupervisor speaker_assets;
  easy_input::speaker_assets::SoundReadLease boot_sound_lease;
  easy_input::speaker_assets::SoundResolvedAsset boot_sound_asset;
  SpeakerStartupPhase speaker_startup_phase =
      SpeakerStartupPhase::StartLocal;
  bool speaker_boot_resolution_started = false;
  bool speaker_boot_skip_requested = false;
  bool speaker_factory_boot_sound = false;
  bool speaker_skip_boot_after_deep_sleep = false;
  bool speaker_wifi_unavailable_logged = false;
  ai_keyboard::SpeakerWifiAdmissionState speaker_wifi_admission =
      ai_keyboard::SpeakerWifiAdmissionState::NotAttempted;
  std::uint32_t speaker_local_retry_after_ms = 0U;
  std::uint32_t speaker_wifi_retry_after_ms = 0U;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  bool speaker_power_hold_active = false;
#endif
  ai_keyboard::EncoderScrollAxis encoder_scroll_axis = ai_keyboard::EncoderScrollAxis::Vertical;
  std::uint16_t battery_mv = 0;
  std::uint16_t battery_raw_mv = 0;
  std::uint8_t battery_percent = 0;
  std::uint32_t battery_sample_ms = 0;
  bool battery_sample_valid = false;
  std::uint32_t last_power_log_ms = 0;
  std::uint32_t last_heartbeat_log_ms = 0;
  // Only debounced physical HMI events may advance this clock. Management,
  // transport, LED and audio work can wake the owner task but cannot defer the
  // 30-minute whole-device Deep Sleep deadline.
  std::uint32_t last_user_activity_ms = 0;
  bool last_usb_mounted = false;
  bool last_ble_connected = false;
  bool led_status_initialized = false;
  const char* last_wake_reason = "boot";
  ai_keyboard::PowerCycleSnapshot latest_power_cycle;
  bool key_wake_verified = false;
  bool deep_sleep_wakeup_configured = false;
  bool audio_power_hold_active = false;
  ai_keyboard::EncoderPressGesture encoder_press_gesture;
  ai_keyboard::MouseWheelQueue pending_encoder_text_selection_steps;
  bool encoder_text_selection_active = false;
  bool encoder_text_selection_exit_pending = false;
  bool encoder_text_selection_chord_pending = false;
  ai_keyboard::PlatformSelectionController platform_selection;
  std::uint32_t last_encoder_led_feedback_ms = 0;
  ai_keyboard::MouseWheelQueue pending_wheel_reports;
  std::uint32_t last_wheel_flush_ms = 0;
  std::uint32_t wheel_send_failures = 0;
  std::uint32_t wheel_transport_drops = 0;
  std::uint32_t wheel_coalesced_reports = 0;
  std::uint32_t hid_event_sequence = 0;
  ai_keyboard::HeldKeyboardState held_keyboard;
  ai_keyboard::KeyboardSnapshotDelivery keyboard_delivery;
  ai_keyboard::KeyboardTransportLatch keyboard_transport;
  std::array<ai_keyboard::KeyboardTransportLatch,
             ai_keyboard::kKeyboardStateSourceCount>
      bridged_hotkey_transports;
  std::array<ai_keyboard::BridgedHotkeyDelivery,
             ai_keyboard::kKeyboardStateSourceCount>
      bridged_hotkey_deliveries;
  bool transport_usb_mounted = false;
  std::uint32_t usb_transport_epoch = 0;
  bool transport_ble_connected = false;
  std::uint32_t ble_transport_epoch = 0;
  bool input_led_feedback_pending = false;
  ai_keyboard::InputId pending_input_led = ai_keyboard::InputId::Key1;
  ai_keyboard::InputPhase pending_input_led_phase = ai_keyboard::InputPhase::Released;
  std::uint32_t pending_input_led_ms = 0;
  ai_keyboard::AgentStatusCommand last_agent_status_command{};
  bool last_agent_status_valid = false;
  ai_keyboard::AwakeWaitDecision next_awake_wait{};
  std::string last_input = "none";
  std::uint32_t last_input_ms = 0;
  TaskHandle_t platform_task = nullptr;
  std::atomic<bool> status_refresh_pending{false};
};

bool handle_input_event(const easy_input::InputEvent& event, void* context);

std::uint32_t millis() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

TickType_t delay_ticks(std::uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

const char* input_name(ai_keyboard::InputId input) {
  switch (input) {
    case ai_keyboard::InputId::Key1:
      return "KEY1";
    case ai_keyboard::InputId::Key2:
      return "KEY2";
    case ai_keyboard::InputId::Key3:
      return "KEY3";
    case ai_keyboard::InputId::Key4:
      return "KEY4";
    case ai_keyboard::InputId::Key5:
      return "KEY5";
    case ai_keyboard::InputId::Key6:
      return "KEY6";
    case ai_keyboard::InputId::Key7:
      return "KEY7";
    case ai_keyboard::InputId::Key8:
      return "KEY8";
    case ai_keyboard::InputId::EncoderLeft:
      return "ENC_LEFT";
    case ai_keyboard::InputId::EncoderRight:
      return "ENC_RIGHT";
    case ai_keyboard::InputId::EncoderPress:
      return "ENC_PRESS";
    case ai_keyboard::InputId::Count:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* phase_name(ai_keyboard::InputPhase phase) {
  return phase == ai_keyboard::InputPhase::Pressed ? "pressed" : "released";
}

const char* ptt_mode_name(ai_keyboard::PttMode mode) {
  switch (mode) {
    case ai_keyboard::PttMode::Hold:
      return "hold";
    case ai_keyboard::PttMode::Toggle:
      return "toggle";
  }
  return "toggle";
}

int input_gpio(ai_keyboard::InputId input) {
  switch (input) {
    case ai_keyboard::InputId::Key1:
    case ai_keyboard::InputId::Key2:
    case ai_keyboard::InputId::Key3:
    case ai_keyboard::InputId::Key4:
    case ai_keyboard::InputId::Key5:
    case ai_keyboard::InputId::Key6:
    case ai_keyboard::InputId::Key7:
    case ai_keyboard::InputId::Key8: {
      const auto index = static_cast<std::size_t>(input);
      return index < ai_keyboard::kKeyPins.size()
                 ? static_cast<int>(ai_keyboard::kKeyPins[index].gpio)
                 : -1;
    }
    case ai_keyboard::InputId::EncoderLeft:
      return static_cast<int>(ai_keyboard::kEncoderPinA);
    case ai_keyboard::InputId::EncoderRight:
      return static_cast<int>(ai_keyboard::kEncoderPinB);
    case ai_keyboard::InputId::EncoderPress:
      return static_cast<int>(ai_keyboard::kEncoderPressPin);
    case ai_keyboard::InputId::Count:
      return -1;
  }
  return -1;
}

const char* encoder_scroll_axis_name(ai_keyboard::EncoderScrollAxis axis) {
  switch (axis) {
    case ai_keyboard::EncoderScrollAxis::Vertical:
      return "vertical";
    case ai_keyboard::EncoderScrollAxis::Horizontal:
      return "horizontal";
    case ai_keyboard::EncoderScrollAxis::Toggle:
      return "toggle";
  }
  return "unknown";
}

const char* encoder_rotation_mode_name(ai_keyboard::EncoderRotationMode mode) {
  switch (mode) {
    case ai_keyboard::EncoderRotationMode::Scroll:
      return "scroll";
    case ai_keyboard::EncoderRotationMode::Cursor:
      return "cursor";
  }
  return "unknown";
}

const char* parse_status_name(ai_keyboard::ConfigParseStatus status) {
  switch (status) {
    case ai_keyboard::ConfigParseStatus::Ok:
      return "ok";
    case ai_keyboard::ConfigParseStatus::InvalidJson:
      return "invalid_json";
    case ai_keyboard::ConfigParseStatus::InvalidSchema:
      return "invalid_schema";
    case ai_keyboard::ConfigParseStatus::UnsupportedAudio:
      return "unsupported_audio";
    case ai_keyboard::ConfigParseStatus::MissingProfile:
      return "missing_profile";
    case ai_keyboard::ConfigParseStatus::MissingBinding:
      return "missing_binding";
    case ai_keyboard::ConfigParseStatus::UnknownAction:
      return "unknown_action";
    case ai_keyboard::ConfigParseStatus::FixedTextTooLarge:
      return "fixed_text_too_large";
  }
  return "unknown";
}

void configure_power_management() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t config = {};
  config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  config.min_freq_mhz = CONFIG_XTAL_FREQ;
  config.light_sleep_enable = false;

  const esp_err_t err = esp_pm_configure(&config);
  if (err == ESP_OK) {
    ESP_LOGI(kTag,
             "power management enabled max=%dMHz min=%dMHz light_sleep=%s",
             config.max_freq_mhz,
             config.min_freq_mhz,
             config.light_sleep_enable ? "true" : "false");
  } else {
    ESP_LOGW(kTag, "power management unavailable: %s", esp_err_to_name(err));
  }
#else
  ESP_LOGW(kTag, "power management disabled in sdkconfig");
#endif
}

std::uint32_t next_power_cycle_sequence(const AppContext* app) {
  const auto next = app->latest_power_cycle.sequence + 1;
  return next == 0 ? 1 : next;
}

void record_completed_power_cycle(AppContext* app,
                                  std::uint32_t now_ms,
                                  const char* reason,
                                  bool reached_deep_sleep = false) {
  const auto snapshot = ai_keyboard::build_power_cycle_snapshot(
      next_power_cycle_sequence(app),
      now_ms - app->last_user_activity_ms,
      ai_keyboard::power_cycle_wake_reason(reason),
      reached_deep_sleep);
  if (!snapshot.valid()) {
    return;
  }
  app->latest_power_cycle = snapshot;
  ESP_LOGI(kTag,
           "power cycle seq=%lu inactive_ms=%lu deep_sleep=%u wake=%s",
           static_cast<unsigned long>(snapshot.sequence),
           static_cast<unsigned long>(snapshot.inactive_ms),
           snapshot.reached_deep_sleep ? 1U : 0U,
           ai_keyboard::power_cycle_wake_reason_name(snapshot.wake_reason));
}

void retain_power_cycle_for_deep_sleep(const ai_keyboard::PowerCycleSnapshot& snapshot) {
  g_retained_power_cycle = {
      kRetainedPowerCycleMagic,
      kRetainedPowerCycleVersion,
      0,
      snapshot.sequence,
      snapshot.inactive_ms,
      static_cast<std::uint8_t>(snapshot.reached_deep_sleep ? 1U : 0U),
      static_cast<std::uint8_t>(snapshot.wake_reason),
      0,
  };
}

void restore_retained_power_cycle(AppContext* app, esp_sleep_wakeup_cause_t wake_cause) {
  const bool retained = g_retained_power_cycle.magic == kRetainedPowerCycleMagic &&
                        g_retained_power_cycle.version == kRetainedPowerCycleVersion &&
                        g_retained_power_cycle.sequence != 0;
  if (!retained || wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    g_retained_power_cycle.magic = 0;
    return;
  }

  auto wake_reason = ai_keyboard::PowerCycleWakeReason::Other;
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    wake_reason = ai_keyboard::PowerCycleWakeReason::DeepSleepKey;
  } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
    wake_reason = ai_keyboard::PowerCycleWakeReason::Timer;
  }
  app->latest_power_cycle = {
      g_retained_power_cycle.sequence,
      g_retained_power_cycle.inactive_ms,
      g_retained_power_cycle.reached_deep_sleep != 0,
      wake_reason,
  };
  g_retained_power_cycle.magic = 0;
}

void mark_user_activity(AppContext* app,
                        std::uint32_t now_ms,
                        const char* reason = "input") {
  app->last_user_activity_ms = now_ms;
  app->audio.cancel_wifi_release_for_device_activity();
  ESP_LOGD(kTag, "user activity reason=%s", reason == nullptr ? "input" : reason);
}

void signal_async_work(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return;
  }
  if (app->platform_task != nullptr) {
    xTaskNotifyGive(app->platform_task);
  }
}

void signal_status_read(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return;
  }
  app->status_refresh_pending.store(true, std::memory_order_release);
  if (app->platform_task != nullptr) {
    xTaskNotifyGive(app->platform_task);
  }
}

bool external_power_status_active(const AppContext* app);
bool usb_vbus_status_present();

ai_keyboard::PowerPolicyInputs base_power_policy_inputs(const AppContext* app,
                                                        std::uint32_t now_ms) {
  ai_keyboard::PowerPolicyInputs inputs;
  inputs.now_ms = now_ms;
  inputs.last_user_activity_ms = app->last_user_activity_ms;
  inputs.external_power = external_power_status_active(app);
  const bool usb_disconnect_sample_pending =
      app->usb_physical_presence.disconnect_pending() ||
      (app->usb_physical_presence.present() && !inputs.external_power);
  inputs.input_active =
      app->inputs.any_input_active() || app->inputs.activity_pending() ||
      usb_disconnect_sample_pending;
  inputs.usb_mounted = app->usb.mounted();
  inputs.key_wake_verified = app->key_wake_verified;
  return inputs;
}

ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,
                                                   std::uint32_t now_ms);
bool bridged_hotkey_work_pending(const AppContext* app);

bool external_power_status_active(const AppContext* app) {
  (void)app;
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    return usb_vbus_status_present();
  }

  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    return gpio_get_level(static_cast<gpio_num_t>(ai_keyboard::kChargeStatusPin)) ==
           ai_keyboard::kChargeStatusChargingLevel;
  }

  return false;
}

bool usb_vbus_status_present() {
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    return gpio_get_level(
               static_cast<gpio_num_t>(
                   ai_keyboard::kExternalPowerSensePin)) ==
           ai_keyboard::kExternalPowerSenseActiveLevel;
  }
  return false;
}

void sync_usb_physical_presence(AppContext* app, std::uint32_t now_ms) {
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    if (!app->usb_physical_presence.update(
            usb_vbus_status_present(), now_ms)) {
      return;
    }
    const bool present = app->usb_physical_presence.present();
    app->usb.observe_physical_presence(present);
    ESP_LOGI(kTag,
             "USB physical VBUS stable present=%u",
             present ? 1U : 0U);
  }
}

const char* charge_state_name(ChargeState state) {
  return ai_keyboard::battery_power_state_name(state);
}

ChargeState charge_state_for(const AppContext* app) {
  if (!external_power_status_active(app)) {
    return ChargeState::Battery;
  }

  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    const int chrg_level =
        gpio_get_level(static_cast<gpio_num_t>(ai_keyboard::kChargeStatusPin));
    return ai_keyboard::battery_power_state_from_signals(
        true, chrg_level, ai_keyboard::kChargeStatusChargingLevel);
  }

  return ChargeState::UsbUnknown;
}

void configure_board_status_inputs() {
  std::uint64_t mask = 0;
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    mask |= 1ULL << ai_keyboard::kExternalPowerSensePin;
  }
  if constexpr (ai_keyboard::kChargeStatusPin >= 0) {
    mask |= 1ULL << ai_keyboard::kChargeStatusPin;
  }
  if constexpr (ai_keyboard::kKeyWakePin >= 0) {
    mask |= 1ULL << ai_keyboard::kKeyWakePin;
  }
  if (mask == 0) {
    return;
  }

  gpio_config_t config = {};
  config.pin_bit_mask = mask;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "board status input config failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(kTag,
           "configured board status inputs pullup=on external_power=GPIO%d charge=GPIO%d key_wake=GPIO%d",
           static_cast<int>(ai_keyboard::kExternalPowerSensePin),
           static_cast<int>(ai_keyboard::kChargeStatusPin),
           static_cast<int>(ai_keyboard::kKeyWakePin));
}

void IRAM_ATTR board_status_gpio_isr(void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr || app->platform_task == nullptr) {
    return;
  }
  BaseType_t higher_priority_woken = pdFALSE;
  vTaskNotifyGiveFromISR(app->platform_task, &higher_priority_woken);
  if (higher_priority_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

void configure_board_status_notifications(AppContext* app) {
  if (app == nullptr) {
    return;
  }
  constexpr std::array<std::int8_t, 2> kStatusPins{
      ai_keyboard::kExternalPowerSensePin,
      ai_keyboard::kChargeStatusPin,
  };
  for (const auto pin : kStatusPins) {
    if (pin < 0) {
      continue;
    }
    const auto gpio = static_cast<gpio_num_t>(pin);
    ESP_ERROR_CHECK(gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(gpio, board_status_gpio_isr, app));
  }
}

bool configure_deep_sleep_wakeup() {
  if constexpr (ai_keyboard::kKeyWakePin < 0) {
    ESP_LOGW(kTag, "deep sleep disabled: board has no KEY_WAKE");
    return false;
  } else {
    const std::uint64_t wake_mask = 1ULL << ai_keyboard::kKeyWakePin;
    const esp_err_t err =
        esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
      ESP_LOGW(kTag,
               "deep sleep wake config failed key_wake=GPIO%d: %s",
               static_cast<int>(ai_keyboard::kKeyWakePin),
               esp_err_to_name(err));
      return false;
    }
    // EXT1 remains armed while Awake and is consumed only by the terminal
    // Deep Sleep transition. Do not use gpio_wakeup_enable: it overwrites the
    // edge interrupt owned by gpio_keys and can create an interrupt storm.
    ESP_LOGI(kTag,
             "deep sleep wake configured key_wake=GPIO%d",
             static_cast<int>(ai_keyboard::kKeyWakePin));
    return true;
  }
}

int read_optional_gpio(std::int8_t gpio) {
  if (gpio < 0) {
    return -1;
  }
  return gpio_get_level(static_cast<gpio_num_t>(gpio));
}

bool key_wake_line_asserted() {
  if constexpr (ai_keyboard::kKeyWakePin < 0) {
    return false;
  } else {
    return read_optional_gpio(ai_keyboard::kKeyWakePin) == 0;
  }
}

int read_gpio(std::uint8_t gpio) {
  return gpio_get_level(static_cast<gpio_num_t>(gpio));
}

std::string raw_key_levels() {
  std::string levels;
  levels.reserve(ai_keyboard::kKeyPins.size());
  for (const auto& key : ai_keyboard::kKeyPins) {
    const int level = read_gpio(key.gpio);
    levels.push_back(level == 0 ? '0' : '1');
  }
  return levels;
}

std::string raw_encoder_levels() {
  std::string levels;
  levels.reserve(3);
  levels.push_back(read_gpio(ai_keyboard::kEncoderPinA) == 0 ? '0' : '1');
  levels.push_back(read_gpio(ai_keyboard::kEncoderPinB) == 0 ? '0' : '1');
  levels.push_back(read_gpio(ai_keyboard::kEncoderPressPin) == 0 ? '0' : '1');
  return levels;
}

std::string pinmap_summary() {
  std::string summary;
  summary.reserve(96);
  for (std::size_t index = 0; index < ai_keyboard::kKeyPins.size(); ++index) {
    if (!summary.empty()) {
      summary += ",";
    }
    summary += "K" + std::to_string(index + 1) + "=" +
               std::to_string(ai_keyboard::kKeyPins[index].gpio);
  }
  summary += ",E=" + std::to_string(ai_keyboard::kEncoderPinA) + "/" +
             std::to_string(ai_keyboard::kEncoderPinB) + "/" +
             std::to_string(ai_keyboard::kEncoderPressPin);
  summary += ",W=" + std::to_string(static_cast<int>(ai_keyboard::kKeyWakePin));
  summary += ",P=" + std::to_string(static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin));
  summary += ",L=" + std::to_string(ai_keyboard::kWs2812Pin);
  summary += ",MIC=" + std::to_string(static_cast<int>(ai_keyboard::kMicI2sBclkPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kMicI2sWsPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kMicI2sDataInPin));
  summary += ",SPK=" + std::to_string(static_cast<int>(ai_keyboard::kSpkI2sBclkPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kSpkI2sWsPin)) + "/" +
             std::to_string(static_cast<int>(ai_keyboard::kSpkI2sDataOutPin));
  return summary;
}

std::string raw_diagnostic_gpio_levels() {
  std::string levels;
  levels.reserve(64);
  for (const auto& key : ai_keyboard::kKeyPins) {
    if (!levels.empty()) {
      levels += ",";
    }
    const auto gpio = key.gpio;
    levels += std::to_string(gpio) + ":" +
              std::to_string(gpio_get_level(static_cast<gpio_num_t>(gpio)));
  }
  return levels;
}

ai_keyboard::BoardDiagnosticsSnapshot board_diagnostics(const AppContext* app,
                                                         std::uint32_t now_ms) {
  const auto last_input_age =
      app->last_input_ms == 0 ? 0 : static_cast<std::uint32_t>(now_ms - app->last_input_ms);
  const auto input = app->inputs.diagnostics();
  return {
      ai_keyboard::kBoardName,
      raw_key_levels(),
      raw_encoder_levels(),
      pinmap_summary(),
      raw_diagnostic_gpio_levels(),
      app->last_input,
      last_input_age,
      read_optional_gpio(ai_keyboard::kKeyWakePin),
      read_optional_gpio(ai_keyboard::kExternalPowerSensePin),
      read_optional_gpio(ai_keyboard::kChargeStatusPin),
      static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
      read_optional_gpio(ai_keyboard::kPeripheralPowerEnablePin),
      static_cast<int>(ai_keyboard::kPeripheralPowerEnableActiveLevel),
      static_cast<int>(ai_keyboard::kWs2812Pin),
      input.raw_edges,
      input.edge_queue_drops,
      input.emitted_events,
      input.filtered_transitions,
      input.encoder_edges,
      input.encoder_steps,
      input.encoder_invalid_transitions,
      input.encoder_partial_resets,
      input.encoder_queue_drops,
  };
}

ai_keyboard::PowerDiagnosticsSnapshot power_diagnostics(AppContext* app,
                                                         std::uint32_t now_ms) {
  const auto decision = ai_keyboard::evaluate_deep_sleep_policy(
      power_policy_inputs(app, now_ms));
  return {
      "awake",
      now_ms - app->last_user_activity_ms,
      decision.deep_sleep_block,
      app->last_wake_reason,
      app->inputs.wake_edge_count(),
      app->usb.mounted(),
      app->latest_power_cycle.sequence,
      app->latest_power_cycle.inactive_ms,
      app->latest_power_cycle.reached_deep_sleep,
      ai_keyboard::power_cycle_wake_reason_name(app->latest_power_cycle.wake_reason),
  };
}

std::uint16_t config_json_crc16(const std::string& json) {
  return ai_keyboard::crc16_ccitt(reinterpret_cast<const std::uint8_t*>(json.data()),
                                  json.size());
}

std::string publish_config_status(AppContext* app,
                                  const char* phase,
                                  const char* status,
                                  std::size_t bytes,
                                  std::uint16_t crc16,
                                  bool saved,
                                  bool force_confirmation_view = false,
                                  const ai_keyboard::SpeakerProbeSnapshot*
                                      speaker_probe = nullptr) {
  const bool diagnostic_status = phase != nullptr && std::string_view(phase) == "diag";
  const bool battery_status = phase != nullptr && std::string_view(phase) == "battery";
  const bool speaker_status =
      phase != nullptr && std::string_view(phase) == "spk_probe";
  const bool config_confirmation = force_confirmation_view ||
      (phase != nullptr &&
       (std::string_view(phase) == "push" || std::string_view(phase) == "platform"));
  const auto now_ms = millis();
  ai_keyboard::AudioStatusSnapshot audio_status;
  if (!diagnostic_status && !battery_status && !speaker_status &&
      !config_confirmation) {
    const auto audio_diagnostics = app->audio.diagnostics();
    audio_status = {
        app->config_state.audio_enabled(),
        ai_keyboard::kAudioTransport,
        "keyboard",
        ai_keyboard::kAudioTransport,
        app->audio.capture_status(),
        app->config_state.audio_host(),
        app->config_state.audio_port(),
        ai_keyboard::kAudioMicrophoneChannel,
        ai_keyboard::kAudioSpeakerChannel,
        audio_diagnostics.sent_packets,
        audio_diagnostics.sent_bytes,
        audio_diagnostics.last_rms_milli,
        audio_diagnostics.peak_rms_milli,
        audio_diagnostics.send_errors,
        audio_diagnostics.read_errors,
        audio_diagnostics.recovery_count,
        audio_diagnostics.session_generation,
        audio_diagnostics.session_id,
        audio_diagnostics.stream_phase,
        audio_diagnostics.stop_reason,
        audio_diagnostics.control_state,
        audio_diagnostics.last_error,
        audio_diagnostics.stream_host,
        audio_diagnostics.stream_port,
    };
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  // Product playback already records a fixed, allocation-free boot probe.
  // Include its compact terminal core in ordinary battery/GATT refreshes so a
  // silent boot can be diagnosed later without serial or diagnostic firmware.
  const auto product_speaker_probe = app->speaker.probe_snapshot();
  if (speaker_probe == nullptr && product_speaker_probe.present) {
    speaker_probe = &product_speaker_probe;
  }
#endif
  ai_keyboard::ConfigStatusSnapshot snapshot{
      kFirmwareVersion,
      phase == nullptr ? "" : phase,
      status == nullptr ? "unknown" : status,
      bytes,
      crc16,
      app->config_state.ptt_hotkey(),
      app->config_state.edit_ptt_hotkey(),
      ptt_mode_name(app->config_state.ptt_mode()),
      saved,
      app->battery_mv,
      app->battery_percent,
      audio_status,
      diagnostic_status || battery_status ? power_diagnostics(app, now_ms)
                                          : ai_keyboard::PowerDiagnosticsSnapshot{},
      diagnostic_status ? board_diagnostics(app, now_ms)
                        : ai_keyboard::BoardDiagnosticsSnapshot{},
      battery_status && app->battery_sample_valid
          ? ai_keyboard::BatteryStatusSnapshot{
                app->battery_raw_mv,
                charge_state_name(charge_state_for(app)),
                now_ms - app->battery_sample_ms,
                app->battery_estimator.full_anchor_mv() > 0,
            }
          : ai_keyboard::BatteryStatusSnapshot{},
      ai_keyboard::host_platform_name(app->config_state.target_platform()),
      true,
      true,
      true,
  };
  snapshot.speaker = speaker_probe;
  auto status_json = config_confirmation
      ? ai_keyboard::build_config_confirmation_status_json(snapshot)
      : ai_keyboard::build_config_status_json(snapshot);
  if (status_json.size() > ai_keyboard::kConfigStatusGattSafeLen) {
    ESP_LOGE(kTag,
             "CONFIG status builder exceeded GATT budget phase=%s bytes=%u; using core confirmation view",
             phase == nullptr ? "" : phase,
             static_cast<unsigned>(status_json.size()));
    status_json = ai_keyboard::build_config_confirmation_status_json(snapshot);
  }
  if (status_json.size() > ai_keyboard::kConfigStatusGattSafeLen) {
    ESP_LOGE(kTag,
             "CONFIG status suppressed after bounded fallback phase=%s bytes=%u",
             phase == nullptr ? "" : phase,
             static_cast<unsigned>(status_json.size()));
    return {};
  }
  app->ble.publish_status_json(status_json);
  return status_json;
}

void publish_config_status_for_json(AppContext* app,
                                    const char* phase,
                                    const char* status,
                                    const std::string& json,
                                    bool saved) {
  publish_config_status(app, phase, status, json.size(), config_json_crc16(json), saved);
}

void publish_config_status_without_payload(AppContext* app,
                                           const char* phase,
                                           const char* status,
                                           bool saved) {
  publish_config_status(app, phase, status, 0, 0, saved);
}

bool decode_speaker_sync_key(
    const std::string& encoded,
    std::array<std::uint8_t, 32>* decoded) {
  if (decoded == nullptr || encoded.size() != 64U) {
    return false;
  }
  auto hex_nibble = [](char value, std::uint8_t* nibble) {
    if (nibble == nullptr) {
      return false;
    }
    if (value >= '0' && value <= '9') {
      *nibble = static_cast<std::uint8_t>(value - '0');
      return true;
    }
    if (value >= 'a' && value <= 'f') {
      *nibble = static_cast<std::uint8_t>(
          value - 'a' + 10);
      return true;
    }
    if (value >= 'A' && value <= 'F') {
      *nibble = static_cast<std::uint8_t>(
          value - 'A' + 10);
      return true;
    }
    return false;
  };
  decoded->fill(0U);
  for (std::size_t index = 0U; index < decoded->size();
       ++index) {
    std::uint8_t high = 0U;
    std::uint8_t low = 0U;
    if (!hex_nibble(encoded[index * 2U], &high) ||
        !hex_nibble(encoded[index * 2U + 1U], &low)) {
      decoded->fill(0U);
      return false;
    }
    (*decoded)[index] = static_cast<std::uint8_t>(
        (high << 4U) | low);
  }
  return std::any_of(
      decoded->begin(),
      decoded->end(),
      [](std::uint8_t value) { return value != 0U; });
}

void sync_keyboard_audio_config(AppContext* app, const char* reason) {
  easy_input::KeyboardAudioConfig config;
  const auto port = app->config_state.audio_port();
  // 麦克风来源是 App 的本地会话策略。固件只根据已配置的 Wi-Fi 音频
  // 端点决定硬件能力，避免旧 audio_enabled 状态把控制面永久关闭。
  config.enabled = app->config_state.audio_enabled();
  config.wifi_ssid = app->config_state.wifi_ssid();
  config.wifi_password = app->config_state.wifi_password();
  config.host = app->config_state.audio_host();
  config.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
  config.speaker_sync_key_epoch =
      app->config_state.speaker_sync_key_epoch();
  config.speaker_sync_key_valid =
      config.speaker_sync_key_epoch != 0U &&
      decode_speaker_sync_key(
          app->config_state.speaker_sync_key(),
          &config.speaker_sync_key);
  app->audio.configure(config);
  ESP_LOGI(kTag,
           "audio sync reason=%s enabled=%d transport=%s mic=keyboard endpoint_configured=%d capture=%s",
           reason == nullptr ? "" : reason,
           config.enabled ? 1 : 0,
           ai_keyboard::kAudioTransport,
           !config.wifi_ssid.empty() && !config.host.empty() && config.port != 0 ? 1 : 0,
           app->audio.capture_status().c_str());
}

enum class PttAudioTrigger {
  None,
  Voice,
  Edit,
};

bool hotkey_matches_configured_ptt(const std::string& configured_hotkey,
                                   const std::string& action_hotkey) {
  return !configured_hotkey.empty() && configured_hotkey == action_hotkey;
}

PttAudioTrigger ptt_audio_trigger_for_action(const AppContext* app,
                                             const ai_keyboard::Action& action) {
  if (action.kind == ai_keyboard::ActionKind::VoicePttHold) {
    return PttAudioTrigger::Voice;
  }
  if (action.kind == ai_keyboard::ActionKind::EditPttHold) {
    return PttAudioTrigger::Edit;
  }

  if (app != nullptr && action.kind == ai_keyboard::ActionKind::Hotkey) {
    if (hotkey_matches_configured_ptt(app->config_state.ptt_hotkey(), action.hotkey)) {
      return PttAudioTrigger::Voice;
    }
    if (hotkey_matches_configured_ptt(app->config_state.edit_ptt_hotkey(), action.hotkey)) {
      return PttAudioTrigger::Edit;
    }
  }

  return PttAudioTrigger::None;
}

void handle_ptt_keyboard_audio(AppContext* app,
                               const ai_keyboard::Action& action,
                               ai_keyboard::InputPhase phase,
                               const char* source) {
  const auto trigger = ptt_audio_trigger_for_action(app, action);
  if (trigger == PttAudioTrigger::None) {
    return;
  }

  ESP_LOGD(kTag,
           "audio ptt event forwarded source=%s phase=%s trigger=%s capture=%s",
           source == nullptr ? "" : source,
           phase_name(phase),
           trigger == PttAudioTrigger::Edit ? "edit" : "voice",
           app->audio.capture_status().c_str());
  if (phase == ai_keyboard::InputPhase::Pressed) {
    app->audio.prepare_for_audio_trigger();
  }
}

void toggle_encoder_scroll_axis(AppContext* app);
bool flush_encoder_text_selection(AppContext* app);

bool encoder_text_selection_owns_keyboard_transport(
    const AppContext* app) {
  return app != nullptr &&
         (app->encoder_text_selection_active ||
          app->encoder_text_selection_exit_pending ||
          app->encoder_text_selection_chord_pending ||
          !app->pending_encoder_text_selection_steps.empty());
}

const char* keyboard_transport_name(ai_keyboard::KeyboardTransportOwner owner) {
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::None:
      return "none";
    case ai_keyboard::KeyboardTransportOwner::Usb:
      return "usb";
    case ai_keyboard::KeyboardTransportOwner::Ble:
      return "ble";
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      return "suppressed";
  }
  return "unknown";
}

void reconcile_keyboard_transport_lifetimes(AppContext* app) {
  app->ble.refresh_connection_identity();

  // TinyUSB callbacks own the USB lifetime generation. Reading the generation
  // here detects an unmount/remount pair even when both callbacks happened
  // between two main-loop iterations.
  const auto usb_epoch = app->usb.connection_epoch();
  app->transport_usb_mounted = usb_epoch != 0;
  app->usb_transport_epoch = usb_epoch;

  const bool previous_ble_connected = app->transport_ble_connected;
  const auto previous_ble_epoch = app->ble_transport_epoch;
  const auto ble_epoch = app->ble.connection_epoch();
  app->transport_ble_connected =
      app->ble.connected() && ble_epoch != 0;
  app->ble_transport_epoch =
      app->transport_ble_connected ? ble_epoch : 0;

  if (previous_ble_connected != app->transport_ble_connected ||
      previous_ble_epoch != app->ble_transport_epoch) {
    // Relative movement belongs to one concrete host lifetime. It has no
    // release state to reconcile, so stale displacement is discarded exactly
    // at the lifetime boundary and never replayed after reconnect.
    app->pending_wheel_reports.clear();
  }

  const bool keyboard_invalidated =
      app->keyboard_transport.observe_transport_state(
          app->transport_usb_mounted,
          app->usb_transport_epoch,
          app->transport_ble_connected,
          app->ble_transport_epoch);
  if (keyboard_invalidated) {
    if (encoder_text_selection_owns_keyboard_transport(app)) {
      // A selection session that lost its exact host lifetime must not replay
      // retained Shift+Arrow distance into a replacement host. Disconnect
      // releases the old host's keyboard state, so leave the mode as well.
      app->pending_encoder_text_selection_steps.clear();
      app->encoder_text_selection_active = false;
      app->encoder_text_selection_exit_pending = false;
      app->encoder_text_selection_chord_pending = false;
    }
    const auto held = app->held_keyboard.current();
    // The old endpoint can no longer consume a pending transition. Establish
    // the current physical state as the delivery baseline so no stale key-down
    // is replayed into the fresh endpoint. If everything is already released,
    // the next chord may select the fresh endpoint immediately.
    app->keyboard_delivery.reset(held);
    if (held.empty()) {
      app->keyboard_transport.commit_snapshot(true);
    }
    ESP_LOGW(kTag,
             "keyboard transport lifetime changed; held=%u next chord gated=%u",
             static_cast<unsigned>(app->held_keyboard.active_source_count()),
             held.empty() ? 0U : 1U);
  }

  for (std::size_t index = 0;
       index < app->bridged_hotkey_transports.size();
       ++index) {
    auto& transport = app->bridged_hotkey_transports[index];
    const bool invalidated = transport.observe_transport_state(
        app->transport_usb_mounted,
        app->usb_transport_epoch,
        app->transport_ble_connected,
        app->ble_transport_epoch);
    if (!invalidated) {
      continue;
    }
    auto& delivery = app->bridged_hotkey_deliveries[index];
    delivery.reset_to_desired();
    if (!delivery.desired_pressed()) {
      transport.commit_snapshot(true);
    }
  }
}

bool send_keyboard_snapshot(AppContext* app,
                            const ai_keyboard::HidKeyboardSnapshot& snapshot,
                            ai_keyboard::HidReportClass report_class) {
  const auto owner = app->keyboard_transport.select_for_snapshot(
      snapshot.empty(),
      app->transport_usb_mounted,
      app->usb_transport_epoch,
      app->transport_ble_connected,
      app->ble_transport_epoch);
  bool accepted = false;
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::Usb: {
      accepted = app->usb.queue_keyboard_report_for_epoch(
          snapshot.modifier,
          snapshot.keycodes,
          snapshot.apple_fn,
          report_class,
          app->keyboard_transport.owner_epoch());
      app->usb.poll_pending_reports();
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::Ble: {
      const auto expected_owner = app->ble.connection_identity();
      if (!expected_owner.valid() ||
          expected_owner.generation != app->keyboard_transport.owner_epoch()) {
        return false;
      }
      accepted = app->ble.send_keyboard_report_for_owner(
          snapshot.modifier,
          snapshot.keycodes,
          snapshot.apple_fn,
          report_class,
          expected_owner);
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::None:
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      // A chord that started without a host, or whose owner disconnected, is
      // deliberately suppressed until physical release. Treat suppression as
      // handled so it cannot block a later chord.
      accepted = true;
      break;
  }
  return accepted;
}

bool flush_pending_keyboard_snapshot(AppContext* app) {
  // Sample endpoint lifetimes before taking a pending snapshot. Reconciliation
  // may deliberately cancel a state queued for a disconnected host.
  reconcile_keyboard_transport_lifetimes(app);
  const auto pending = app->keyboard_delivery.pending_snapshot();
  if (!pending.valid()) {
    // select_for_snapshot() may have provisionally latched an endpoint before
    // a queue accepted the first down edge. If that edge and its matching up
    // coalesced under bounded overload, release the empty chord latch here so
    // the next physical press can select a live endpoint.
    if (app->keyboard_delivery.desired().empty()) {
      app->keyboard_transport.commit_snapshot(true);
    }
    return true;
  }
  if (!send_keyboard_snapshot(app, pending.snapshot, pending.report_class)) {
    return false;
  }
  if (!app->keyboard_delivery.mark_accepted(pending.generation)) {
    ESP_LOGE(kTag,
             "HID snapshot acceptance raced with desired state generation=%lu",
             static_cast<unsigned long>(pending.generation));
    return false;
  }
  app->keyboard_transport.commit_snapshot(
      pending.snapshot.empty() &&
      !encoder_text_selection_owns_keyboard_transport(app));
  return true;
}

bool dispatch_held_keyboard_event(AppContext* app,
                                  ai_keyboard::InputId source,
                                  const ai_keyboard::FirmwareEvent& event) {
  ai_keyboard::HeldKeyboardUpdate update;
  if (event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown) {
    const auto report = ai_keyboard::hid_report_for_hotkey(event.value);
    update = app->held_keyboard.press(source, report);
  } else {
    update = app->held_keyboard.release(source);
  }

  if (!update.accepted()) {
    ESP_LOGW(kTag,
             "HID state rejected source=%s kind=%s status=%u",
             input_name(source),
             event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ? "down" : "up",
             static_cast<unsigned>(update.status));
    return false;
  }
  if (!update.state_changed || !update.report_changed) {
    return true;
  }

  app->keyboard_delivery.set_desired(update.snapshot);
  const bool accepted = flush_pending_keyboard_snapshot(app);
  if (!accepted) {
    ESP_LOGD(kTag,
             "HID snapshot not accepted source=%s owner=%s empty=%u held=%u",
             input_name(source),
             keyboard_transport_name(app->keyboard_transport.owner()),
             update.snapshot.empty() ? 1U : 0U,
             static_cast<unsigned>(app->held_keyboard.active_source_count()));
  }
  return accepted;
}

bool flush_pending_bridged_hotkey_event(AppContext* app,
                                        ai_keyboard::InputId source) {
  const auto source_index = static_cast<std::size_t>(source);
  if (source_index >= app->bridged_hotkey_transports.size()) {
    return false;
  }
  auto& transport = app->bridged_hotkey_transports[source_index];
  auto& delivery = app->bridged_hotkey_deliveries[source_index];
  const auto pending = delivery.pending_transition();
  if (!pending.valid) {
    // A complete short press may occur while the transport queue is full. If
    // its down edge never entered the queue, down+up safely coalesce to a
    // no-op and the provisional endpoint latch can be released immediately.
    if (!delivery.desired_pressed()) {
      transport.commit_snapshot(true);
    }
    return true;
  }

  const bool released = !pending.pressed;
  const auto owner = transport.select_for_snapshot(
      released,
      app->transport_usb_mounted,
      app->usb_transport_epoch,
      app->transport_ble_connected,
      app->ble_transport_epoch);
  const ai_keyboard::FirmwareEvent event{
      pending.pressed ? ai_keyboard::FirmwareEventKind::HidKeyDown
                      : ai_keyboard::FirmwareEventKind::HidKeyUp,
      pending.hotkey,
      true,
  };
  bool accepted = false;
  switch (owner) {
    case ai_keyboard::KeyboardTransportOwner::Usb:
      accepted = app->usb.send_firmware_event_for_epoch(
          input_name(source), event, transport.owner_epoch());
      break;
    case ai_keyboard::KeyboardTransportOwner::Ble: {
      const auto expected_owner = app->ble.connection_identity();
      if (!expected_owner.valid() ||
          expected_owner.generation != transport.owner_epoch()) {
        return false;
      }
      accepted = app->ble.send_firmware_event_for_owner(
          input_name(source), event, expected_owner);
      break;
    }
    case ai_keyboard::KeyboardTransportOwner::None:
    case ai_keyboard::KeyboardTransportOwner::Suppressed:
      accepted = true;
      break;
  }
  if (accepted && delivery.mark_accepted(pending)) {
    transport.commit_snapshot(released);
    return true;
  }
  return false;
}

void flush_pending_bridged_hotkey_events(AppContext* app) {
  for (std::size_t index = 0;
       index < app->bridged_hotkey_deliveries.size();
       ++index) {
    flush_pending_bridged_hotkey_event(
        app, static_cast<ai_keyboard::InputId>(index));
  }
}

bool dispatch_bridged_hotkey_event(AppContext* app,
                                   ai_keyboard::InputId source,
                                   const ai_keyboard::FirmwareEvent& event) {
  reconcile_keyboard_transport_lifetimes(app);
  const auto source_index = static_cast<std::size_t>(source);
  if (source_index >= app->bridged_hotkey_deliveries.size()) {
    return false;
  }
  const bool pressed =
      event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown;
  app->bridged_hotkey_deliveries[source_index].set_desired(
      pressed, event.value);
  return flush_pending_bridged_hotkey_event(app, source);
}

void dispatch_firmware_event(AppContext* app,
                             ai_keyboard::InputId source_input,
                             const ai_keyboard::FirmwareEvent& event) {
  if (event.kind == ai_keyboard::FirmwareEventKind::None) {
    return;
  }

  const char* source = input_name(source_input);
  const auto sequence = ++app->hid_event_sequence;
  if (event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ||
      event.kind == ai_keyboard::FirmwareEventKind::HidKeyUp) {
    const auto report = ai_keyboard::hid_report_for_hotkey(event.value);
    const bool app_bridge =
        event.bridge_app_hotkey && !report.valid;
    const bool accepted =
        app_bridge
            ? dispatch_bridged_hotkey_event(app, source_input, event)
            : dispatch_held_keyboard_event(app, source_input, event);
    if (!accepted) {
      ESP_LOGD(kTag,
               "HID dispatch seq=%lu source=%s kind=%s delivery rejected",
               static_cast<unsigned long>(sequence),
               source,
               event.kind == ai_keyboard::FirmwareEventKind::HidKeyDown ? "down" : "up");
    }
    return;
  }

  const auto route = ai_keyboard::route_for_firmware_event(event, app->ble.connected());
  const char* kind = "unknown";
  switch (event.kind) {
    case ai_keyboard::FirmwareEventKind::None:
      kind = "none";
      break;
    case ai_keyboard::FirmwareEventKind::HidKeyDown:
      kind = "down";
      break;
    case ai_keyboard::FirmwareEventKind::HidKeyUp:
      kind = "up";
      break;
    case ai_keyboard::FirmwareEventKind::HidTap:
      kind = "tap";
      break;
    case ai_keyboard::FirmwareEventKind::FixedText:
      kind = "text";
      break;
    case ai_keyboard::FirmwareEventKind::HostAction:
      kind = "host_action";
      break;
    case ai_keyboard::FirmwareEventKind::AppCommand:
      kind = "app";
      break;
  }
  ESP_LOGD(kTag,
           "HID dispatch seq=%lu source=%s kind=%s route=%s usb=%u ble=%u",
           static_cast<unsigned long>(sequence),
           source,
           kind,
           route == ai_keyboard::FirmwareTransportRoute::BleFirst ? "ble_first" : "usb_first",
           app->usb.ready() ? 1U : 0U,
           app->ble.connected() ? 1U : 0U);
  if (route == ai_keyboard::FirmwareTransportRoute::BleFirst) {
    if (!app->ble.send_firmware_event(source, event)) {
      ESP_LOGW(kTag, "HID dispatch seq=%lu BLE delivery failed", static_cast<unsigned long>(sequence));
    }
    return;
  }

  const auto usb_epoch = app->usb.connection_epoch();
  if (usb_epoch != 0) {
    if (!app->usb.send_firmware_event_for_epoch(source, event, usb_epoch)) {
      // USB ownership is fixed at physical-event arrival. Queue pressure or a
      // concurrent remount must not migrate the same event to BLE.
      ESP_LOGW(kTag,
               "HID dispatch seq=%lu USB owner rejected report; "
               "BLE fallback suppressed",
               static_cast<unsigned long>(sequence));
    }
    return;
  }
  if (!app->ble.send_firmware_event(source, event)) {
    ESP_LOGW(kTag,
             "HID dispatch seq=%lu no transport accepted report",
             static_cast<unsigned long>(sequence));
  }
}

void dispatch_encoder_press_click(AppContext* app) {
  const auto& action = app->config_state.keymap().action_for(ai_keyboard::InputId::EncoderPress);
  if (action.kind == ai_keyboard::ActionKind::ScrollAxisToggle) {
    toggle_encoder_scroll_axis(app);
    return;
  }
  if (action.kind == ai_keyboard::ActionKind::TextCaretSelect) {
    if (app->encoder_text_selection_exit_pending) {
      flush_encoder_text_selection(app);
      if (app->encoder_text_selection_exit_pending) {
        ESP_LOGD(kTag, "ENC_TEXT_SELECT new entry waits for prior chords");
        return;
      }
    }
    if (app->encoder_text_selection_active) {
      app->encoder_text_selection_active = false;
      app->encoder_text_selection_exit_pending = true;
      flush_encoder_text_selection(app);
      return;
    }
    app->encoder_text_selection_active = true;
    app->encoder_text_selection_exit_pending = false;
    app->encoder_text_selection_chord_pending = false;
    app->pending_encoder_text_selection_steps.clear();
    ESP_LOGI(kTag, "ENC_TEXT_SELECT entered native Shift+Arrow mode");
    return;
  }

  const auto press_event =
      ai_keyboard::event_for_action(action,
                                    ai_keyboard::InputPhase::Pressed,
                                    app->config_state.ptt_hotkey(),
                                    app->config_state.edit_ptt_hotkey(),
                                    app->config_state.target_platform());
  dispatch_firmware_event(app, ai_keyboard::InputId::EncoderPress, press_event);

  const auto release_event =
      ai_keyboard::event_for_action(action,
                                    ai_keyboard::InputPhase::Released,
                                    app->config_state.ptt_hotkey(),
                                    app->config_state.edit_ptt_hotkey(),
                                    app->config_state.target_platform());
  if (release_event.kind != ai_keyboard::FirmwareEventKind::None) {
    vTaskDelay(delay_ticks(15));
    dispatch_firmware_event(app, ai_keyboard::InputId::EncoderPress, release_event);
  }
}

ai_keyboard::EncoderScrollAxis active_encoder_axis(AppContext* app,
                                                   const ai_keyboard::EncoderScrollConfig& config) {
  const auto& press_action =
      app->config_state.keymap().action_for(ai_keyboard::InputId::EncoderPress);
  const bool press_toggles_axis = press_action.kind == ai_keyboard::ActionKind::ScrollAxisToggle;
  if (config.axis == ai_keyboard::EncoderScrollAxis::Toggle || press_toggles_axis) {
    return app->encoder_scroll_axis;
  }
  return config.axis;
}

bool queue_encoder_text_selection_chord(AppContext* app, bool move_right) {
  // A pending physical snapshot must establish the exact baseline before a
  // synthetic tap can be appended. Returning false keeps the encoder distance
  // in its bounded direction queue for a later retry.
  if (!flush_pending_keyboard_snapshot(app) ||
      app->keyboard_delivery.pending()) {
    return false;
  }
  // Reconciliation above may have discovered that the exact selection owner
  // disappeared and cleared the whole session. Do not let the already-copied
  // queue head select a fresh host after that cancellation.
  if (!encoder_text_selection_owns_keyboard_transport(app)) {
    return false;
  }

  const auto source = move_right ? ai_keyboard::InputId::EncoderRight
                                 : ai_keyboard::InputId::EncoderLeft;
  const auto report = ai_keyboard::hid_report_for_hotkey(
      move_right ? "Shift+ArrowRight" : "Shift+ArrowLeft");
  const auto pressed = app->held_keyboard.press(source, report);
  if (!pressed.accepted()) {
    ESP_LOGD(kTag,
             "ENC_TEXT_SELECT chord waits for keyboard capacity status=%u",
             static_cast<unsigned>(pressed.status));
    return false;
  }
  const auto restored = app->held_keyboard.release(source);
  if (!restored.accepted()) {
    ESP_LOGE(kTag, "ENC_TEXT_SELECT failed to restore logical keyboard state");
    return false;
  }

  // If the exact Shift+Arrow chord is already physically held, adding and
  // removing the encoder source cannot create a new HID transition. Consume
  // the detent as a safe no-op instead of perturbing the user's held keys.
  if (!pressed.report_changed) {
    return true;
  }

  app->encoder_text_selection_chord_pending = true;
  if (!send_keyboard_snapshot(
          app, pressed.snapshot, ai_keyboard::HidReportClass::KeyboardPress)) {
    return false;
  }
  app->keyboard_transport.commit_snapshot(false);

  const auto release_class = restored.snapshot.empty()
                                 ? ai_keyboard::HidReportClass::KeyboardAllReleased
                                 : ai_keyboard::HidReportClass::KeyboardRelease;
  if (!send_keyboard_snapshot(app, restored.snapshot, release_class)) {
    // KeyboardPress admission reserves room for its matching release. The only
    // expected failure here is an owner lifetime loss; reconciliation will
    // discard this chord before any fresh host can receive it.
    ESP_LOGW(kTag, "ENC_TEXT_SELECT release rejected after accepted press");
    return false;
  }
  // Keep the exact host latched across detents and across unrelated physical
  // key taps until the selection mode has drained and exited. Otherwise a USB
  // mount between two detents could split one logical selection across hosts.
  app->keyboard_transport.commit_snapshot(false);
  app->encoder_text_selection_chord_pending = false;
  return true;
}

bool flush_encoder_text_selection(AppContext* app) {
  for (std::size_t sent = 0;
       sent < kEncoderSelectionChordsPerFlush;
       ++sent) {
    ai_keyboard::QueuedMouseWheel steps;
    if (!app->pending_encoder_text_selection_steps.front(&steps)) {
      if (app->encoder_text_selection_exit_pending) {
        app->encoder_text_selection_exit_pending = false;
        if (app->held_keyboard.empty() &&
            !app->keyboard_delivery.pending()) {
          app->keyboard_transport.commit_snapshot(true);
        }
        ESP_LOGI(kTag, "ENC_TEXT_SELECT exited after draining native chords");
      }
      return true;
    }

    const int direction = (steps.horizontal > 0) - (steps.horizontal < 0);
    if (direction == 0) {
      app->pending_encoder_text_selection_steps.pop_if_sequence(steps.sequence);
      continue;
    }
    if (!queue_encoder_text_selection_chord(app, direction > 0)) {
      return false;
    }
    if (!app->pending_encoder_text_selection_steps.consume_if_sequence(
            steps.sequence, 0, direction)) {
      ESP_LOGE(kTag, "ENC_TEXT_SELECT pending chord consume failed");
      return false;
    }
  }
  return app->pending_encoder_text_selection_steps.empty();
}

void release_keyboard_reports(AppContext* app) {
  app->held_keyboard.clear();
  // Preserve the delivery state that the transport has already accepted. A
  // zero snapshot remains pending until USB/BLE accepts it; queue pressure
  // must never turn platform/config switching into a one-shot best effort.
  app->keyboard_delivery.set_desired({});
  flush_pending_keyboard_snapshot(app);
  for (std::size_t index = 0;
       index < app->bridged_hotkey_deliveries.size();
       ++index) {
    app->bridged_hotkey_deliveries[index].set_desired(false, {});
    flush_pending_bridged_hotkey_event(
        app, static_cast<ai_keyboard::InputId>(index));
  }
}

bool save_selected_host_platform(AppContext* app,
                                 ai_keyboard::HostPlatform platform,
                                 std::uint32_t now_ms) {
  release_keyboard_reports(app);
  esp_err_t save_err = ESP_OK;
  const bool saved = app->config_store.save_host_platform(platform, &save_err);
  if (saved) {
    app->config_state.set_target_platform(platform);
  }
  app->leds.show_status_event(saved
      ? (platform == ai_keyboard::HostPlatform::Windows
             ? easy_input::StatusLedEvent::PlatformWindows
             : easy_input::StatusLedEvent::PlatformMacOS)
      : easy_input::StatusLedEvent::SaveFailed, now_ms);
  ESP_LOGI(kTag, "HOST platform=%s saved=%u", ai_keyboard::host_platform_name(platform), saved ? 1U : 0U);
  return saved;
}

void handle_platform_selection_result(
    AppContext* app,
    const ai_keyboard::PlatformSelectionResult& result,
    std::uint32_t now_ms) {
  using Outcome = ai_keyboard::PlatformSelectionOutcome;
  if (result.outcome == Outcome::MacOS || result.outcome == Outcome::Windows) {
    const auto platform = result.outcome == Outcome::Windows
                              ? ai_keyboard::HostPlatform::Windows
                              : ai_keyboard::HostPlatform::MacOS;
    const bool saved = save_selected_host_platform(app, platform, now_ms);
    publish_config_status_without_payload(
        app, "platform", saved ? "ok" : "save_failed", saved);
    ESP_LOGI(kTag,
             "HOST platform selected from config mode platform=%s saved=%u",
             ai_keyboard::host_platform_name(platform),
             saved ? 1U : 0U);
    return;
  }
  if (result.outcome == Outcome::Conflict) {
    app->leds.show_status_event(easy_input::StatusLedEvent::SaveFailed, now_ms);
    publish_config_status_without_payload(app, "platform", "conflict", false);
    ESP_LOGW(kTag, "HOST platform selection rejected: KEY1 and KEY2 conflict");
    return;
  }
  if (result.outcome == Outcome::TimedOut) {
    ESP_LOGI(kTag, "HOST platform selection mode timed out");
  } else if (result.outcome == Outcome::Cancelled) {
    ESP_LOGI(kTag, "HOST platform selection mode cancelled by another key");
  }
}

void tap_keyboard_keycode(AppContext* app,
                          ai_keyboard::InputId source,
                          std::uint8_t keycode,
                          int repeat_count = 1) {
  const int taps = std::clamp(repeat_count, 1, kEncoderCursorMaxTapsPerEvent);
  ai_keyboard::HidKeyboardReport report;
  report.valid = keycode != 0;
  report.keycode = keycode;
  report.keycodes[0] = keycode;
  for (int index = 0; index < taps; ++index) {
    const auto pressed = app->held_keyboard.press(source, report);
    if (pressed.accepted() && pressed.report_changed) {
      app->keyboard_delivery.set_desired(pressed.snapshot);
      flush_pending_keyboard_snapshot(app);
    }
    vTaskDelay(delay_ticks(kEncoderCursorTapHoldMs));
    const auto released = app->held_keyboard.release(source);
    if (released.accepted() && released.report_changed) {
      app->keyboard_delivery.set_desired(released.snapshot);
      flush_pending_keyboard_snapshot(app);
    }
    if (index + 1 < taps) {
      vTaskDelay(delay_ticks(kEncoderCursorTapGapMs));
    }
  }
}

bool send_ble_mouse_wheel_report(AppContext* app,
                                 std::int8_t vertical,
                                 std::int8_t horizontal,
                                 ai_keyboard::BleOwnerToken expected_owner) {
  return app->ble.send_mouse_wheel_for_owner(
      vertical, horizontal, expected_owner);
}

bool ble_mouse_wheel_transport_available(const AppContext* app) {
  return app->ble.connected();
}

void queue_mouse_wheel_report(AppContext* app,
                              std::int8_t vertical,
                              std::int8_t horizontal,
                              std::uint32_t now_ms) {
  if (vertical == 0 && horizontal == 0) {
    return;
  }

  // Transport ownership is selected when the physical movement arrives, not
  // when a HID endpoint later becomes ready. A mounted USB interface keeps
  // ownership through endpoint backpressure; the USB transport's bounded
  // queue consumes displacement only after TinyUSB accepts the report.
  bool coalesced = false;
  const auto usb_epoch = app->usb.connection_epoch();
  if (usb_epoch != 0) {
    if (!app->usb.queue_mouse_wheel_for_epoch(
            vertical, horizontal, usb_epoch, &coalesced)) {
      ++app->wheel_transport_drops;
      ESP_LOGD(kTag,
               "wheel USB queue full; dropped vertical=%d horizontal=%d drops=%lu",
               static_cast<int>(vertical),
               static_cast<int>(horizontal),
               static_cast<unsigned long>(app->wheel_transport_drops));
      return;
    }
    if (coalesced) {
      ++app->wheel_coalesced_reports;
    }
    return;
  }

  // With USB absent this queue is owned by one exact BLE connection lifetime.
  // Movement observed without an HID owner is discarded instead of replayed
  // into a future host.
  const auto ble_owner = app->ble.connection_identity();
  if (!ble_owner.valid()) {
    ++app->wheel_transport_drops;
    return;
  }
  if (!app->pending_wheel_reports.push(
          vertical,
          horizontal,
          now_ms,
          nullptr,
          &coalesced,
          nullptr,
          ble_owner)) {
    ++app->wheel_transport_drops;
    ESP_LOGD(kTag,
             "wheel BLE queue full; dropped vertical=%d horizontal=%d drops=%lu",
             static_cast<int>(vertical),
             static_cast<int>(horizontal),
             static_cast<unsigned long>(app->wheel_transport_drops));
    return;
  }
  if (coalesced) {
    ++app->wheel_coalesced_reports;
  }
}

bool has_pending_wheel_report(const AppContext* app) {
  return !app->pending_wheel_reports.empty() ||
         app->usb.mouse_wheel_report_pending();
}

ai_keyboard::PowerPolicyInputs power_policy_inputs(AppContext* app,
                                                   std::uint32_t now_ms) {
  auto inputs = base_power_policy_inputs(app, now_ms);
  inputs.config_window_active = app->ble.config_window_active();
  inputs.encoder_press_pending = app->encoder_press_gesture.pending() ||
                                 app->encoder_text_selection_exit_pending ||
                                 app->encoder_text_selection_chord_pending ||
                                 !app->pending_encoder_text_selection_steps.empty();
  inputs.wheel_report_pending = has_pending_wheel_report(app);
  inputs.management_work_pending =
      app->status_refresh_pending.load(std::memory_order_acquire) ||
      app->audio.management_work_pending() ||
      app->usb.work_schedule(now_ms).outstanding ||
      app->ble.management_work_pending() ||
      app->audio_io_arbiter.deep_sleep_quiesce_interrupted();
  inputs.hid_work_pending =
      app->keyboard_delivery.pending() || app->ble.input_delivery_pending() ||
      bridged_hotkey_work_pending(app);
  inputs.audio_streaming =
      app->audio.streaming() ||
      app->audio_io_arbiter.microphone_generation() != 0;
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  inputs.speaker_playback_active = app->speaker.sleep_blocked();
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  inputs.speaker_playback_active =
      inputs.speaker_playback_active ||
      app->speaker_assets.transfer_active();
#endif
#endif
  inputs.wifi_active = app->audio.wifi_active_or_streaming();
  inputs.wake_source_configured = app->deep_sleep_wakeup_configured;
  inputs.key_wake_asserted = key_wake_line_asserted();
  return inputs;
}

// Whole-device sleep admission is based on 30 minutes without confirmed user
// input. A BLE connection is teardown-able and therefore not a permanent
// blocker; only concrete in-flight management/HID work blocks the commit.
bool deep_sleep_allowed(AppContext* app, std::uint32_t now_ms, const char** reason) {
  const auto decision =
      ai_keyboard::evaluate_deep_sleep_policy(power_policy_inputs(app, now_ms));
  if (reason != nullptr) {
    *reason = decision.deep_sleep_block;
  }
  return decision.deep_sleep_allowed;
}

bool try_begin_audio_deep_sleep_quiesce(AppContext* app) {
  return app->audio_io_arbiter.try_begin_deep_sleep_quiesce();
}

void cancel_audio_deep_sleep_quiesce(AppContext* app) {
  if (!app->audio_io_arbiter.cancel_deep_sleep_quiesce()) {
    ESP_LOGE(kTag, "deep sleep audio admission gate cancel failed");
  }
}

bool begin_audio_deep_sleep_terminal(AppContext* app) {
  return app->audio_io_arbiter.begin_deep_sleep_terminal();
}

void maybe_enter_deep_sleep(AppContext* app, std::uint32_t now_ms) {
  const auto decision =
      ai_keyboard::evaluate_deep_sleep_policy(power_policy_inputs(app, now_ms));
  if (decision.wifi_release_required) {
    app->audio.request_wifi_release_for_deep_sleep();
    return;
  }
  if (!decision.deep_sleep_allowed) {
    // 深睡门槛已到但出现 USB、配置窗口或输入等新阻塞条件时,
    // 取消此前可能已经发出的释放请求并恢复控制通道。
    if (now_ms - app->last_user_activity_ms >= kDeepSleepAfterMs) {
      app->audio.cancel_wifi_release_for_device_activity();
    }
    return;
  }

  ESP_LOGI(kTag,
           "entering deep sleep idle_ms=%lu battery=%umV wake=KEY_WAKE(GPIO%d) any key",
           static_cast<unsigned long>(now_ms - app->last_user_activity_ms),
           static_cast<unsigned>(app->battery_mv),
           static_cast<int>(ai_keyboard::kKeyWakePin));

  if constexpr (ai_keyboard::kKeyWakePin >= 0) {
    // 深睡时数字域上拉断电,显式启用 RTC 域上拉,防 KEY_WAKE 悬空误唤醒。
    const esp_err_t pullup_err =
        rtc_gpio_pullup_en(static_cast<gpio_num_t>(ai_keyboard::kKeyWakePin));
    if (pullup_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE pull-up failed: %s",
               esp_err_to_name(pullup_err));
      return;
    }
    const esp_err_t pulldown_err =
        rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(ai_keyboard::kKeyWakePin));
    if (pulldown_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE pull-down disable failed: %s",
               esp_err_to_name(pulldown_err));
      return;
    }
    // Clear a stale timer source left by an older firmware revision before
    // the irreversible commit. The Awake architecture never arms this timer.
    const esp_err_t timer_err =
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (timer_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep timer wake disable failed: %s",
               esp_err_to_name(timer_err));
      return;
    }
    const esp_err_t wake_err = esp_sleep_enable_ext1_wakeup_io(
        1ULL << ai_keyboard::kKeyWakePin,
        ESP_EXT1_WAKEUP_ANY_LOW);
    if (wake_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep KEY_WAKE config failed: %s",
               esp_err_to_name(wake_err));
      return;
    }

    // Atomically close new microphone/speaker admission before the final
    // policy snapshots. An asynchronous Wi-Fi start may otherwise arrive
    // after a snapshot and race the GPIO8 shutdown sequence.
    if (!try_begin_audio_deep_sleep_quiesce(app)) {
      ESP_LOGI(kTag, "deep sleep cancelled: audio transition/owner active");
      return;
    }
    if (!app->ble.try_begin_deep_sleep_quiesce()) {
      ESP_LOGI(kTag, "deep sleep cancelled: BLE management/HID work active");
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    // Wake-source setup and logging may take time. Re-evaluate every blocker
    // only after the audio admission gate is closed. Also honor GPIO/encoder
    // edges latched by an ISR after this main-loop iteration polled input.
    const auto final_decision = ai_keyboard::evaluate_deep_sleep_policy(
        power_policy_inputs(app, millis()));
    if (!final_decision.deep_sleep_allowed || app->inputs.activity_pending()) {
      ESP_LOGI(kTag,
               "deep sleep cancelled during final gate reason=%s input_pending=%u",
               final_decision.deep_sleep_block,
               app->inputs.activity_pending() ? 1U : 0U);
      app->ble.cancel_deep_sleep_quiesce();
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    const esp_err_t led_err = app->leds.prepare_for_deep_sleep();
    if (led_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep LED quiesce failed: %s",
               esp_err_to_name(led_err));
      app->ble.cancel_deep_sleep_quiesce();
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    // The black frame takes a bounded RMT transaction. Recheck every mutable
    // blocker once more immediately before command pins are destructively
    // detached and the rail is driven low.
    const auto commit_decision = ai_keyboard::evaluate_deep_sleep_policy(
        power_policy_inputs(app, millis()));
    if (!commit_decision.deep_sleep_allowed ||
        app->inputs.activity_pending()) {
      ESP_LOGI(kTag,
               "deep sleep cancelled at commit gate reason=%s input_pending=%u",
               commit_decision.deep_sleep_block,
               app->inputs.activity_pending() ? 1U : 0U);
      app->ble.cancel_deep_sleep_quiesce();
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    // This CAS is the irreversible audio admission cutoff. A MIC/SPK request
    // that arrived after Quiesce was accepted, retained and latched as an
    // interruption, so terminalization fails and Awake is restored. Only
    // requests arriving after this exact point may be permanently rejected.
    if (!begin_audio_deep_sleep_terminal(app)) {
      ESP_LOGI(kTag,
               "deep sleep cancelled at audio terminal gate; admission reopened");
      app->ble.cancel_deep_sleep_quiesce();
      cancel_audio_deep_sleep_quiesce(app);
      return;
    }

    // BLE performs one last atomic terminal admission check. NOT_FINISHED is
    // no longer reversible after the audio terminal cutoff above; every BLE
    // failure from this point requires a clean firmware restart.
    const esp_err_t ble_shutdown_err = app->ble.shutdown_for_deep_sleep();
    if (ble_shutdown_err == ESP_ERR_NOT_FINISHED) {
      ESP_LOGE(kTag,
               "deep sleep BLE terminal gate interrupted after audio commit; restarting");
      esp_restart();
      return;
    }
    if (ble_shutdown_err != ESP_OK) {
      ESP_LOGE(kTag,
               "deep sleep BLE shutdown failed: %s; restarting",
               esp_err_to_name(ble_shutdown_err));
      esp_restart();
      return;
    }
    record_completed_power_cycle(app, now_ms, "unknown", true);
    retain_power_cycle_for_deep_sleep(app->latest_power_cycle);
    if (!app->audio.shutdown_wifi_for_deep_sleep()) {
      g_retained_power_cycle.magic = 0;
      ESP_LOGE(kTag,
               "deep sleep Wi-Fi shutdown failed after BLE commit; restarting");
      esp_restart();
      return;
    }
    const esp_err_t power_err =
        app->peripheral_power.prepare_for_deep_sleep();
    if (power_err != ESP_OK) {
      g_retained_power_cycle.magic = 0;
      ESP_LOGE(kTag,
               "deep sleep shared rail shutdown failed: %s",
               esp_err_to_name(power_err));
      // Once command-pin reconfiguration starts there is no safe generic
      // rollback for live RMT/I2S routing. Cold restart restores the complete
      // awake initialization contract instead of continuing half-configured.
      ESP_LOGE(kTag, "restarting after partial peripheral shutdown failure");
      esp_restart();
      return;
    }
    esp_deep_sleep_start();
  }
}

std::int8_t wheel_chunk(int pending) {
  return static_cast<std::int8_t>(
      std::clamp(pending, -kEncoderWheelMaxChunkMagnitude, kEncoderWheelMaxChunkMagnitude));
}

void flush_pending_wheel_report(AppContext* app, std::uint32_t now_ms, bool force = false) {
  if (!has_pending_wheel_report(app)) {
    return;
  }
  if (!force && app->last_wheel_flush_ms != 0 &&
      now_ms - app->last_wheel_flush_ms < kEncoderWheelFlushIntervalMs) {
    return;
  }

  ai_keyboard::QueuedMouseWheel report;
  if (!app->pending_wheel_reports.front(&report)) {
    return;
  }

  const auto current_owner = app->ble.connection_identity();
  if (!report.ble_owner.valid() || current_owner != report.ble_owner) {
    app->pending_wheel_reports.pop_if_sequence(report.sequence);
    ++app->wheel_transport_drops;
    ESP_LOGD(kTag,
             "BLE wheel owner changed; discarded seq=%lu vertical=%d "
             "horizontal=%d drops=%lu",
             static_cast<unsigned long>(report.sequence),
             report.vertical,
             report.horizontal,
             static_cast<unsigned long>(app->wheel_transport_drops));
    return;
  }

  if (!ble_mouse_wheel_transport_available(app)) {
    return;
  }

  const std::int8_t vertical = wheel_chunk(report.vertical);
  const std::int8_t horizontal = wheel_chunk(report.horizontal);
  app->last_wheel_flush_ms = now_ms;

  if (vertical == 0 && horizontal == 0) {
    return;
  }

  if (!send_ble_mouse_wheel_report(
          app, vertical, horizontal, report.ble_owner)) {
    ++app->wheel_send_failures;
    ESP_LOGD(kTag,
             "BLE wheel HID busy; seq=%lu vertical=%d horizontal=%d failures=%lu",
             static_cast<unsigned long>(report.sequence),
             report.vertical,
             report.horizontal,
             static_cast<unsigned long>(app->wheel_send_failures));
    return;
  }
  app->pending_wheel_reports.consume_if_sequence(report.sequence, vertical, horizontal);
}

void show_encoder_scroll_feedback(AppContext* app,
                                  std::int8_t vertical,
                                  std::int8_t horizontal,
                                  std::uint32_t now_ms) {
  if ((vertical == 0 && horizontal == 0) || app == nullptr) {
    return;
  }
  if (app->last_encoder_led_feedback_ms != 0 &&
      now_ms - app->last_encoder_led_feedback_ms < kEncoderLedFeedbackMinIntervalMs) {
    return;
  }
  app->last_encoder_led_feedback_ms = now_ms;
  app->leds.show_scroll_event(vertical, horizontal, now_ms);
}

void dispatch_encoder_scroll(AppContext* app,
                             const easy_input::InputEvent& event,
                             ai_keyboard::EncoderScrollAxis axis) {
  const auto& config = app->config_state.encoder_scroll();
  const auto direction =
      (event.encoder_step > 0 ? config.speed : -config.speed) * encoder_step_count(event.encoder_step);
  std::int8_t vertical = 0;
  std::int8_t horizontal = 0;
  if (axis == ai_keyboard::EncoderScrollAxis::Vertical) {
    const auto value = config.reverse_vertical ? direction : -direction;
    vertical = hid_axis_value(value);
  } else {
    const auto value = config.reverse_horizontal ? -direction : direction;
    horizontal = hid_axis_value(value);
  }

  ESP_LOGD(kTag,
           "ENC_SCROLL axis=%s speed=%d vertical=%d horizontal=%d",
           encoder_scroll_axis_name(axis),
           config.speed,
           static_cast<int>(vertical),
           static_cast<int>(horizontal));
  const std::uint32_t now_ms = millis();
  show_encoder_scroll_feedback(app, vertical, horizontal, now_ms);
  queue_mouse_wheel_report(app, vertical, horizontal, now_ms);
  flush_pending_wheel_report(app, now_ms, app->last_wheel_flush_ms == 0);
}

void dispatch_encoder_cursor(AppContext* app,
                             const easy_input::InputEvent& event,
                             ai_keyboard::EncoderScrollAxis axis) {
  // The encoder's logical step is inverted from the physical cursor direction:
  // clockwise should move right/down, counter-clockwise should move left/up.
  const bool clockwise = event.encoder_step < 0;
  std::uint8_t keycode = 0;
  std::int8_t vertical_feedback = 0;
  std::int8_t horizontal_feedback = 0;

  if (axis == ai_keyboard::EncoderScrollAxis::Vertical) {
    const bool down = clockwise;
    keycode = down ? kHidKeyArrowDown : kHidKeyArrowUp;
    vertical_feedback = down ? 1 : -1;
  } else {
    const bool right = clockwise;
    keycode = right ? kHidKeyArrowRight : kHidKeyArrowLeft;
    horizontal_feedback = right ? 1 : -1;
  }

  ESP_LOGD(kTag,
           "ENC_CURSOR axis=%s direction=%s keycode=0x%02X",
           encoder_scroll_axis_name(axis),
           clockwise ? "clockwise" : "counter_clockwise",
           static_cast<unsigned>(keycode));
  const int taps = std::clamp(encoder_step_count(event.encoder_step),
                              1,
                              kEncoderCursorMaxTapsPerEvent);
  show_encoder_scroll_feedback(app, vertical_feedback, horizontal_feedback, millis());
  tap_keyboard_keycode(app, event.input, keycode, taps);
}

bool dispatch_encoder_text_selection_step(
    AppContext* app,
    const easy_input::InputEvent& event,
    ai_keyboard::EncoderScrollAxis axis) {
  const bool clockwise = event.encoder_step < 0;
  const int delta = (clockwise ? 1 : -1) *
                    encoder_step_count(event.encoder_step);
  flush_encoder_text_selection(app);
  if (!app->encoder_text_selection_active) {
    // Owner loss cancels this sampled detent as part of the old selection
    // session. Consume the source event without reinterpreting it as a normal
    // cursor move or retaining it for a replacement host.
    return true;
  }
  bool saturated = false;
  constexpr bool split_on_saturation = true;
  const auto retain_steps = [&]() {
    return app->pending_encoder_text_selection_steps.push(
        0,
        delta,
        event.timestamp_ms,
        nullptr,
        nullptr,
        &saturated,
        {},
        0,
        split_on_saturation);
  };
  bool retained = retain_steps();
  if (!retained) {
    flush_encoder_text_selection(app);
    retained = retain_steps();
  }
  if (!retained) {
    ESP_LOGD(kTag,
             "ENC_TEXT_SELECT source retained for retry axis=%s delta=%d",
             encoder_scroll_axis_name(axis),
             delta);
    return false;
  }
  flush_encoder_text_selection(app);
  const auto feedback_y = static_cast<std::int8_t>(
      axis == ai_keyboard::EncoderScrollAxis::Vertical
          ? (delta > 0) - (delta < 0)
          : 0);
  const auto feedback_x = static_cast<std::int8_t>(
      axis == ai_keyboard::EncoderScrollAxis::Horizontal
          ? (delta > 0) - (delta < 0)
          : 0);
  show_encoder_scroll_feedback(app, feedback_y, feedback_x, millis());
  ESP_LOGD(kTag,
           "ENC_TEXT_SELECT axis=%s delta=%d transport=%s retained=%u saturated=%u",
           encoder_scroll_axis_name(axis),
           delta,
           keyboard_transport_name(app->keyboard_transport.owner()),
           retained ? 1U : 0U,
           saturated ? 1U : 0U);
  return true;
}

bool dispatch_encoder_rotation(AppContext* app, const easy_input::InputEvent& event) {
  if (!app->encoder_text_selection_active &&
      encoder_text_selection_owns_keyboard_transport(app)) {
    flush_encoder_text_selection(app);
    if (encoder_text_selection_owns_keyboard_transport(app)) {
      // Preserve source order: a post-exit cursor/scroll detent must not pass
      // pre-exit Shift+Arrow distance that is still transport-backpressured.
      return false;
    }
  }
  const auto& config = app->config_state.encoder_scroll();
  if (!config.enabled) {
    return true;
  }

  const auto axis = active_encoder_axis(app, config);
  if (config.mode == ai_keyboard::EncoderRotationMode::Cursor) {
    const auto& press_action = app->config_state.keymap().action_for(
        ai_keyboard::InputId::EncoderPress);
    if (press_action.kind == ai_keyboard::ActionKind::TextCaretSelect &&
        app->encoder_text_selection_active) {
      return dispatch_encoder_text_selection_step(app, event, axis);
    }
    dispatch_encoder_cursor(app, event, axis);
    return true;
  }
  dispatch_encoder_scroll(app, event, axis);
  return true;
}

void toggle_encoder_scroll_axis(AppContext* app) {
  app->encoder_scroll_axis =
      app->encoder_scroll_axis == ai_keyboard::EncoderScrollAxis::Vertical
          ? ai_keyboard::EncoderScrollAxis::Horizontal
          : ai_keyboard::EncoderScrollAxis::Vertical;
  ESP_LOGI(kTag,
           "ENC_%s axis=%s",
           encoder_rotation_mode_name(app->config_state.encoder_scroll().mode),
           encoder_scroll_axis_name(app->encoder_scroll_axis));
}

void sync_encoder_scroll_axis(AppContext* app) {
  const auto axis = app->config_state.encoder_scroll().axis;
  app->encoder_scroll_axis = axis == ai_keyboard::EncoderScrollAxis::Horizontal
                                 ? ai_keyboard::EncoderScrollAxis::Horizontal
                                 : ai_keyboard::EncoderScrollAxis::Vertical;
  const auto& press_action = app->config_state.keymap().action_for(
      ai_keyboard::InputId::EncoderPress);
  if ((press_action.kind != ai_keyboard::ActionKind::TextCaretSelect ||
       app->config_state.encoder_scroll().mode !=
           ai_keyboard::EncoderRotationMode::Cursor) &&
      (app->encoder_text_selection_active ||
       !app->pending_encoder_text_selection_steps.empty())) {
    app->encoder_text_selection_active = false;
    app->encoder_text_selection_exit_pending = true;
    flush_encoder_text_selection(app);
  }
}

void check_encoder_press_config_hold(AppContext* app,
                                     std::uint32_t observed_ms,
                                     std::uint32_t effect_now_ms,
                                     bool encoder_press_observed) {
  if (!app->encoder_press_gesture.trigger_config_if_due(
          observed_ms,
          kEncoderConfigModeHoldMs,
          encoder_press_observed)) {
    return;
  }

  if (app->encoder_text_selection_active ||
      !app->pending_encoder_text_selection_steps.empty()) {
    app->encoder_text_selection_active = false;
    app->encoder_text_selection_exit_pending = true;
    flush_encoder_text_selection(app);
  }

  app->platform_selection.arm(effect_now_ms, kPlatformSelectionModeTimeoutMs);
  app->ble.open_config_window("encoder_long_press");
  app->leds.show_status_event(
      easy_input::StatusLedEvent::ConfigMode, effect_now_ms);
  ESP_LOGI(kTag,
           "CONFIG mode opened by encoder long press hold_ms=%lu",
           static_cast<unsigned long>(kEncoderConfigModeHoldMs));
  // 状态特征只回吐最后一次发布的缓存;长按进配置模式时发布一份实时快照,
  // 让 App 在无串口环境下读到音频/控制通道与省电门控的完整诊断。
  const char* deep_sleep_block = "unknown";
  deep_sleep_allowed(app, effect_now_ms, &deep_sleep_block);
  std::array<char, 120> diag_status{};
  std::snprintf(diag_status.data(),
                diag_status.size(),
                "%s ds=%s state=awake up=%lus vin=%d chrg=%d",
                app->audio.capture_status().c_str(),
                deep_sleep_block,
                static_cast<unsigned long>(effect_now_ms / 1000U),
                read_optional_gpio(ai_keyboard::kExternalPowerSensePin),
                read_optional_gpio(ai_keyboard::kChargeStatusPin));
  publish_config_status(app, "diag", diag_status.data(), 0, 0, true);
}

void sync_led_status(AppContext* app, std::uint32_t now_ms) {
  const bool usb_mounted = app->usb.mounted();
  if (!app->led_status_initialized || usb_mounted != app->last_usb_mounted) {
    if (app->led_status_initialized || usb_mounted) {
      app->leds.show_status_event(usb_mounted ? easy_input::StatusLedEvent::UsbConnected
                                              : easy_input::StatusLedEvent::UsbDisconnected,
                                  now_ms);
    }
    app->last_usb_mounted = usb_mounted;
  }

  const bool ble_connected = app->ble.connected();
  if (!app->led_status_initialized || ble_connected != app->last_ble_connected) {
    if (app->led_status_initialized || ble_connected) {
      app->leds.show_status_event(ble_connected ? easy_input::StatusLedEvent::BleConnected
                                                : easy_input::StatusLedEvent::BleDisconnected,
                                  now_ms);
    }
    app->last_ble_connected = ble_connected;
  }
  app->led_status_initialized = true;
}

void apply_cold_boot_feedback_action(
    AppContext* app,
    ai_keyboard::ColdBootFeedbackAction action,
    const char* outcome,
    std::uint32_t generation = 0U) {
  if (app == nullptr) {
    return;
  }
  switch (action) {
    case ai_keyboard::ColdBootFeedbackAction::None:
      return;
    case ai_keyboard::ColdBootFeedbackAction::ReserveVisual:
      app->leds.reserve_cold_boot_sequence();
      ESP_LOGI(kTag,
               "cold boot feedback reserved awaiting audio outcome");
      return;
    case ai_keyboard::ColdBootFeedbackAction::StartVisual:
      app->leds.start_cold_boot_sequence(millis());
      ESP_LOGI(kTag,
               "cold boot visual started outcome=%s generation=%lu",
               outcome == nullptr ? "unknown" : outcome,
               static_cast<unsigned long>(generation));
      return;
  }
}

void observe_cold_boot_first_pcm(AppContext* app,
                                 std::uint32_t generation) {
  apply_cold_boot_feedback_action(
      app,
      app->cold_boot_feedback.on_first_pcm_submitted(generation),
      "first_pcm_submitted",
      generation);
}

void settle_cold_boot_silent(AppContext* app, const char* reason) {
  if (app == nullptr ||
      !app->cold_boot_feedback.awaiting_audio_outcome()) {
    return;
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  app->speaker_boot_skip_requested = true;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  app->speaker_probe_pending = false;
#endif
  apply_cold_boot_feedback_action(
      app,
      app->cold_boot_feedback.on_silent_terminal(),
      reason,
      0U);
}

void preempt_cold_boot_audio(AppContext* app, const char* reason) {
  if (app == nullptr ||
      !app->cold_boot_feedback.awaiting_audio_outcome()) {
    return;
  }
  const auto action =
      app->cold_boot_feedback.on_priority_preempted();
  if (action != ai_keyboard::ColdBootFeedbackAction::StartVisual) {
    return;
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  app->speaker_boot_skip_requested = true;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  app->speaker_probe_pending = false;
#endif
  apply_cold_boot_feedback_action(
      app,
      action,
      reason,
      0U);
}

void service_cold_boot_feedback_liveness(
    AppContext* app,
    std::uint32_t now_ms) {
  if (app == nullptr) {
    return;
  }
  const auto action =
      app->cold_boot_feedback.on_liveness_deadline(now_ms);
  if (action != ai_keyboard::ColdBootFeedbackAction::StartVisual) {
    return;
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  ESP_LOGW(
      kTag,
      "cold boot audio admission deadline phase=%u local=%d wifi_admission=%u wifi_carrier=%d store_units=%lu",
      static_cast<unsigned>(app->speaker_startup_phase),
      app->speaker_assets.ready() ? 1 : 0,
      static_cast<unsigned>(app->speaker_wifi_admission),
      app->speaker_assets.wifi_carrier_started() ? 1 : 0,
      static_cast<unsigned long>(
          app->speaker_assets.boot_progress_generation()));
  app->speaker_boot_skip_requested = true;
#endif
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  app->speaker_probe_pending = false;
#endif
  apply_cold_boot_feedback_action(
      app, action, "audio_liveness_deadline", 0U);
}

void sync_audio_power_hold(AppContext* app) {
  // GPIO8 now stays high for the whole awake lifecycle. The per-feature hold
  // remains a readiness/diagnostic fact for the audio arbiter; releasing it
  // cannot switch off the shared rail before whole-device deep sleep.
  const auto microphone_generation =
      app->audio_io_arbiter.microphone_generation();
  const bool audio_active =
      app->audio.streaming() || microphone_generation != 0;
  if (app->audio_power_hold_active != audio_active) {
    const esp_err_t power_err =
        app->peripheral_power.set_audio_power_hold(audio_active);
    if (power_err != ESP_OK) {
      ESP_LOGE(kTag,
               "audio power activity update failed: %s",
               esp_err_to_name(power_err));
      return;
    }
    app->audio_power_hold_active = audio_active;
  }
  if (audio_active && microphone_generation != 0 &&
      app->audio_power_hold_active) {
    app->audio_io_arbiter.mark_microphone_power_ready(
        microphone_generation);
  }
}

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
void service_speaker(AppContext* app) {
  const bool cold_boot_audio_allowed =
      app->cold_boot_feedback.awaiting_audio_outcome() ||
      app->cold_boot_feedback.started_generation() != 0U;
  const bool playback_allowed =
      !app->audio.streaming() &&
      !app->audio_io_arbiter.microphone_requested() &&
      cold_boot_audio_allowed;
  const auto speaker_events = app->speaker.poll(playback_allowed);
  if (speaker_events.first_pcm()) {
    observe_cold_boot_first_pcm(
        app, speaker_events.first_pcm_generation);
  }
  if (speaker_events.terminal() &&
      app->cold_boot_feedback.awaiting_audio_outcome()) {
    // A terminal publication with no preceding first-PCM edge is the exact
    // fail-safe boundary for this one Boot attempt. Start the visual by itself
    // and never wait for a later, unrelated playback generation.
    settle_cold_boot_silent(
        app, "speaker_terminal_before_first_pcm");
  }

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::StartLocal) {
    const auto now_ms = millis();
    const bool retry_due =
        app->speaker_local_retry_after_ms == 0U ||
        static_cast<std::int32_t>(
            now_ms - app->speaker_local_retry_after_ms) >= 0;
    if (retry_due) {
      const auto local_result =
          app->speaker_assets.begin_local(
              &app->usb, app->platform_task);
      if (local_result == ESP_OK) {
        app->speaker_local_retry_after_ms = 0U;
        app->speaker_startup_phase =
            app->speaker_skip_boot_after_deep_sleep
                ? SpeakerStartupPhase::WaitLeaseIdle
                : SpeakerStartupPhase::ResolveBoot;
        ESP_LOGI(kTag, "local speaker asset service started");
      } else {
        if (local_result == ESP_ERR_NO_MEM) {
          // begin_local() fully rolls back synchronization allocation failure
          // and explicitly retains a successfully opened Store while retrying
          // only its task allocation. Preserve this Boot attempt until the
          // admission watchdog, rather than converting a retryable condition
          // into a permanent silent result on the first owner pass.
          app->speaker_local_retry_after_ms =
              now_ms + kSpeakerAssetsRetryMs;
          ESP_LOGW(
              kTag,
              "local speaker asset service allocation deferred: %s",
              esp_err_to_name(local_result));
        } else {
          app->speaker_local_retry_after_ms = 0U;
          app->speaker_startup_phase = SpeakerStartupPhase::Ready;
          app->speaker_boot_skip_requested = true;
          settle_cold_boot_silent(
              app, "local_service_terminal_failure");
          ESP_LOGE(
              kTag,
              "local speaker asset service unavailable: %s",
              esp_err_to_name(local_result));
        }
      }
    }
  }

  if (app->speaker_assets.ready() && !app->audio_ready &&
      app->speaker_wifi_admission ==
          ai_keyboard::SpeakerWifiAdmissionState::NotAttempted) {
    app->speaker_wifi_admission =
        ai_keyboard::SpeakerWifiAdmissionState::Unavailable;
  }
  if (app->speaker_wifi_admission ==
          ai_keyboard::SpeakerWifiAdmissionState::Unavailable &&
      !app->speaker_wifi_unavailable_logged) {
    app->speaker_wifi_unavailable_logged = true;
    ESP_LOGW(
        kTag,
        "Wi-Fi sound sync unavailable because keyboard audio did not initialize; USB sound sync and local Boot playback remain available");
  }

  const auto startup_now_ms = millis();
  const bool wifi_retry_due =
      app->speaker_wifi_retry_after_ms == 0U ||
      static_cast<std::int32_t>(
          startup_now_ms - app->speaker_wifi_retry_after_ms) >= 0;
  const bool boot_resources_released =
      app->speaker_startup_phase == SpeakerStartupPhase::Ready &&
      app->speaker.shutdown_complete() &&
      app->speaker_assets.boot_idle() && !app->speaker.busy() &&
      !app->audio.streaming() &&
      !app->audio_io_arbiter.microphone_requested();
  const auto startup_decision =
      ai_keyboard::evaluate_speaker_service_startup({
      app->speaker_assets.ready(),
      app->speaker_wifi_admission,
      boot_resources_released,
      wifi_retry_due,
  });
  if (startup_decision.attempt_wifi) {
    const bool initial_admission =
        app->speaker_wifi_admission ==
        ai_keyboard::SpeakerWifiAdmissionState::NotAttempted;
    const auto wifi_result =
        app->speaker_assets.start_wifi(&app->audio);
    if (wifi_result == ESP_OK) {
      app->speaker_wifi_admission =
          ai_keyboard::SpeakerWifiAdmissionState::Started;
      app->speaker_wifi_retry_after_ms = 0U;
      app->audio.request_heartbeat_refresh();
      if (initial_admission) {
        ESP_LOGI(
            kTag,
            "speaker asset Wi-Fi carrier admitted before Boot playback");
      } else {
        ESP_LOGI(
            kTag,
            "deferred speaker asset Wi-Fi carrier recovered after Boot release");
      }
    } else if (wifi_result == ESP_ERR_NO_MEM) {
      // SpeakerAssetsWifiCarrier::begin() removes every partial allocation on
      // error. Record the independent management service as deferred, allow
      // local Boot playback to continue, and retry only after SpeakerOutput
      // and the exact Store lease are fully released.
      app->speaker_wifi_admission =
          ai_keyboard::SpeakerWifiAdmissionState::Deferred;
      app->speaker_wifi_retry_after_ms =
          startup_now_ms + kSpeakerAssetsRetryMs;
      ESP_LOGW(
          kTag,
          "speaker asset Wi-Fi carrier allocation deferred; local Boot remains eligible: %s",
          esp_err_to_name(wifi_result));
    } else {
      app->speaker_wifi_admission =
          ai_keyboard::SpeakerWifiAdmissionState::Unavailable;
      app->speaker_wifi_retry_after_ms = 0U;
      app->speaker_wifi_unavailable_logged = true;
      ESP_LOGE(
          kTag,
          "speaker asset Wi-Fi carrier unavailable; local Boot remains eligible: %s",
          esp_err_to_name(wifi_result));
    }
  }
  if (startup_decision.wait_for_local ||
      !startup_decision.boot_allowed) {
    return;
  }

  if (!playback_allowed &&
      app->speaker_startup_phase ==
          SpeakerStartupPhase::ResolveBoot) {
    app->speaker_boot_skip_requested = true;
    settle_cold_boot_silent(app, "microphone_priority_before_boot");
  }

  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::ResolveBoot) {
    // A microphone request that arrives before Boot resolution starts wins
    // immediately. Do not start a Store lease merely to play a late startup
    // sound after the user's recording has finished.
    if (app->speaker_boot_skip_requested &&
        !app->speaker_boot_resolution_started) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
      settle_cold_boot_silent(app, "boot_skipped_before_resolution");
      ESP_LOGI(kTag, "Boot sound skipped before asset resolution");
    } else {
      const auto boot_result =
          app->speaker_assets.take_boot_playback(
              &app->boot_sound_lease,
              &app->boot_sound_asset);
      app->speaker_boot_resolution_started = true;
      if (boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::Ready) {
        app->speaker_factory_boot_sound = false;
        if (app->speaker_boot_skip_requested || !playback_allowed) {
          settle_cold_boot_silent(
              app, "boot_cancelled_after_resolution");
        }
        app->speaker_startup_phase =
            app->speaker_boot_skip_requested || !playback_allowed
                ? SpeakerStartupPhase::ReleaseLease
                : SpeakerStartupPhase::BeginOutput;
      } else if (
          boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::FactoryDefault) {
        app->speaker_factory_boot_sound = true;
        if (app->speaker_boot_skip_requested || !playback_allowed) {
          settle_cold_boot_silent(
              app, "factory_boot_cancelled_after_resolution");
        }
        app->speaker_startup_phase =
            app->speaker_boot_skip_requested || !playback_allowed
                ? SpeakerStartupPhase::ReleaseLease
                : SpeakerStartupPhase::BeginOutput;
      } else if (
          boot_result ==
          easy_input::SpeakerAssetsBootPlaybackResult::Unavailable) {
        app->speaker_startup_phase =
            SpeakerStartupPhase::WaitLeaseIdle;
        settle_cold_boot_silent(app, "boot_sound_unavailable");
        ESP_LOGI(
            kTag,
            "Boot sound is disabled or unavailable; startup stays silent");
      }
    }
  }

  if (app->speaker_startup_phase ==
      SpeakerStartupPhase::BeginOutput) {
    if (!playback_allowed) {
      app->speaker_boot_skip_requested = true;
      app->speaker_startup_phase =
          SpeakerStartupPhase::ReleaseLease;
      settle_cold_boot_silent(app, "microphone_priority_before_output");
    } else {
      app->speaker.mark_boot_pending(
          app->audio_io_arbiter.microphone_generation());
      const auto begin_result =
          app->speaker.begin(
              app->platform_task, &app->audio_io_arbiter);
      if (begin_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Boot speaker unavailable: %s",
            esp_err_to_name(begin_result));
        app->speaker_startup_phase =
            SpeakerStartupPhase::ReleaseLease;
        settle_cold_boot_silent(app, "speaker_begin_failed");
      } else {
        bool requested = false;
        if (app->speaker_factory_boot_sound) {
          const auto factory_sound =
              easy_input::speaker_assets::factory_boot_sound();
          requested = app->speaker.request_embedded_asset(
              factory_sound.encoded,
              factory_sound.encoded_bytes);
        } else {
          requested = app->speaker.request_asset(
              app->speaker_assets.playback_storage(),
              app->boot_sound_lease,
              app->boot_sound_asset);
        }
        if (!requested) {
          ESP_LOGW(kTag, "boot asset playback request rejected");
          app->speaker_startup_phase =
              SpeakerStartupPhase::ShutdownOutput;
          settle_cold_boot_silent(app, "playback_request_rejected");
        } else {
          // This synchronous acceptance is the arbitration boundary. From now
          // on only the generation-tagged first-PCM or terminal worker event
          // may resolve the reserved visual; input/deadline cannot cancel a
          // first frame that was published but not yet consumed by this owner.
          app->cold_boot_feedback.on_audio_attempt_committed();
          app->speaker_startup_phase =
              SpeakerStartupPhase::WaitPlayback;
        }
      }
    }
  }
#endif

#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  if (app->speaker_probe_pending && !playback_allowed) {
    app->speaker_probe_pending = false;
    settle_cold_boot_silent(
        app, "diagnostic_microphone_priority_before_boot");
  } else if (app->speaker_probe_pending && playback_allowed) {
    app->speaker_probe_pending = false;
    if (!app->speaker.request_diagnostic_tone()) {
      ESP_LOGW(kTag, "speaker diagnostic request rejected");
      settle_cold_boot_silent(
          app, "diagnostic_playback_request_rejected");
    } else {
      app->cold_boot_feedback.on_audio_attempt_committed();
    }
  }
#endif

  // Establish the microphone lease before releasing the speaker lease. The
  // audio worker also waits for this power-ready generation, so a concurrent
  // mic start can never overlap or race a GPIO8 rail handoff.
  sync_audio_power_hold(app);
  const bool power_required = app->speaker.power_lease_required();
  if (app->speaker_power_hold_active != power_required) {
    const esp_err_t power_err =
        app->peripheral_power.set_speaker_power_hold(power_required);
    if (power_err != ESP_OK) {
      ESP_LOGE(kTag,
               "speaker power activity update failed: %s",
               esp_err_to_name(power_err));
      return;
    }
    app->speaker_power_hold_active = power_required;
  }
  if (power_required && app->peripheral_power.ready()) {
    app->speaker.notify_power_ready();
  }
  const bool handoff_complete =
      app->speaker.complete_power_handoff();
  if (!handoff_complete) {
    ESP_LOGW(kTag, "speaker audio ownership handoff deferred");
  }
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::WaitPlayback &&
      handoff_complete && !app->speaker.busy()) {
    app->speaker_startup_phase =
        SpeakerStartupPhase::ShutdownOutput;
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::ShutdownOutput &&
      handoff_complete && !app->speaker.busy()) {
    if (!app->speaker.request_shutdown()) {
      ESP_LOGW(kTag, "speaker startup resource shutdown deferred");
    } else if (app->speaker.shutdown_complete()) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::ReleaseLease;
    }
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::ReleaseLease &&
      app->speaker.shutdown_complete()) {
    if (!app->boot_sound_lease.valid) {
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
    } else if (
        app->speaker_assets.queue_playback_lease_release(
            app->boot_sound_lease)) {
      app->boot_sound_lease = {};
      app->boot_sound_asset = {};
      app->speaker_startup_phase =
          SpeakerStartupPhase::WaitLeaseIdle;
    } else {
      ESP_LOGW(kTag, "boot sound read lease release was not queued");
    }
  }

  if (app->speaker_startup_phase ==
          SpeakerStartupPhase::WaitLeaseIdle &&
      app->speaker.shutdown_complete() &&
      app->speaker_assets.boot_idle()) {
    app->speaker_startup_phase =
        SpeakerStartupPhase::Ready;
  }
#endif
}
#endif

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
bool speaker_asset_resource_steps_allowed(
    const AppContext* app,
    std::uint32_t now_ms) {
  if (app == nullptr || app->audio.streaming() ||
      app->audio_io_arbiter.microphone_requested() ||
      app->speaker.busy() || app->inputs.any_input_active() ||
      !app->held_keyboard.empty() ||
      app->keyboard_delivery.pending() ||
      app->encoder_text_selection_chord_pending ||
      !app->pending_encoder_text_selection_steps.empty() ||
      app->ble.input_reports_pending() ||
      has_pending_wheel_report(app) ||
      (app->last_input_ms != 0U &&
       now_ms - app->last_input_ms <
           kSpeakerAssetsInputQuietMs)) {
    return false;
  }
  for (const auto& delivery : app->bridged_hotkey_deliveries) {
    if (delivery.pending()) {
      return false;
    }
  }
  return true;
}
#endif

void flush_input_led_feedback(AppContext* app) {
  if (!app->input_led_feedback_pending) {
    return;
  }
  const auto input = app->pending_input_led;
  const auto phase = app->pending_input_led_phase;
  const auto queued_ms = app->pending_input_led_ms;
  app->input_led_feedback_pending = false;
  app->leds.show_input_event(input, phase, queued_ms);
}

bool handle_input_event(const easy_input::InputEvent& event, void* context) {
  auto* app = static_cast<AppContext*>(context);
  if (app == nullptr) {
    return true;
  }

  const auto now = millis();
  mark_user_activity(app, now, "debounced_input");
  // User input owns startup responsiveness. If Boot audio has not produced a
  // first PCM frame yet, abandon that one-shot attempt and make the reserved
  // visual visible immediately. Any already-audible sound continues normally.
  preempt_cold_boot_audio(app, "input_priority_preempted");
  const auto* name = input_name(event.input);
  app->last_input = std::string(name) + ":" + phase_name(event.phase);
  const int gpio = input_gpio(event.input);
  if (gpio >= 0) {
    app->last_input += ":gpio=" + std::to_string(gpio);
  }
  if (event.encoder_step != 0) {
    app->last_input += ":step=" + std::to_string(event.encoder_step);
  }
  app->last_input_ms = now;
  const bool encoder_turn = event.input == ai_keyboard::InputId::EncoderLeft ||
                            event.input == ai_keyboard::InputId::EncoderRight;
  if (!encoder_turn &&
      ai_keyboard::feedback_for_input_event(event.input, event.phase).active) {
    // Coalesce visual feedback until after HID state has been queued. RMT
    // rendering is intentionally outside the input edge hot path. An inactive
    // release feedback must not erase a press queued in the same poll cycle.
    app->pending_input_led = event.input;
    app->pending_input_led_phase = event.phase;
    app->pending_input_led_ms = now;
    app->input_led_feedback_pending = true;
  }

  if (event.encoder_step != 0) {
    ESP_LOGD(kTag, "%s step=%d", name, event.encoder_step);
  } else {
    ESP_LOGD(kTag, "%s %s", name, phase_name(event.phase));
  }

  if (encoder_turn) {
    // Resolve the 3-second boundary before interpreting this detent, but only
    // while the physical switch is still down. A release that is settling must
    // never be converted into system configuration merely because the user
    // started turning immediately afterwards. Text selection itself is a
    // short-press toggle and never depends on holding while turning.
    check_encoder_press_config_hold(
        app,
        event.timestamp_ms,
        now,
        app->inputs.low_active_pressed(ai_keyboard::kEncoderPressPin));
    if (app->platform_selection.active()) {
      ESP_LOGI(kTag, "%s consumed while awaiting platform selection", name);
      return true;
    }
    return dispatch_encoder_rotation(app, event);
  }

  if (event.input == ai_keyboard::InputId::EncoderPress) {
    if (event.phase == ai_keyboard::InputPhase::Pressed) {
      app->encoder_press_gesture.press(event.timestamp_ms);
      return true;
    }

    const auto release = app->encoder_press_gesture.release();
    if (release.dispatch_click) {
      dispatch_encoder_press_click(app);
    } else if (release.ignored_after_config) {
      ESP_LOGI(kTag, "ENC_PRESS release ignored after config mode long press");
    }
    return true;
  }
  const auto platform_result =
      app->platform_selection.handle_event(event.input, event.phase, now);
  handle_platform_selection_result(app, platform_result, now);
  if (platform_result.consumed) {
    app->input_led_feedback_pending = false;
    ESP_LOGI(kTag, "%s %s consumed by platform selection",
             name,
             phase_name(event.phase));
    return true;
  }

  const auto& action = app->config_state.keymap().action_for(event.input);
  const auto firmware_event =
      ai_keyboard::event_for_action(action,
                                    event.phase,
                                    app->config_state.ptt_hotkey(),
                                    app->config_state.edit_ptt_hotkey(),
                                    app->config_state.target_platform());
  handle_ptt_keyboard_audio(app, action, event.phase, name);
  dispatch_firmware_event(app, event.input, firmware_event);
  return true;
}

void load_stored_config(AppContext* app) {
  ai_keyboard::HostPlatform stored_platform = ai_keyboard::HostPlatform::MacOS;
  if (!app->config_store.load_host_platform(&stored_platform)) {
    stored_platform = ai_keyboard::HostPlatform::MacOS;
  }
  app->config_state.set_target_platform(stored_platform);
  std::string stored_json;
  esp_err_t err = ESP_OK;
  if (!app->config_store.load_config(&stored_json, &err)) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(kTag, "CONFIG load skipped: no stored config");
      publish_config_status_without_payload(app, "boot", "no_stored_config", false);
    } else {
      ESP_LOGW(kTag, "CONFIG load skipped: %s", esp_err_to_name(err));
      publish_config_status_without_payload(app, "boot", "nvs_load_failed", false);
    }
    return;
  }

  const auto status = app->config_state.apply_json(stored_json);
  if (status == ai_keyboard::ConfigParseStatus::Ok) {
    app->config_state.set_target_platform(stored_platform);
    sync_encoder_scroll_axis(app);
    sync_keyboard_audio_config(app, "boot");
  }
  ESP_LOGI(kTag,
           "CONFIG load status=%s bytes=%u",
           parse_status_name(status),
           static_cast<unsigned>(stored_json.size()));
  publish_config_status_for_json(app,
                                 "boot",
                                 parse_status_name(status),
                                 stored_json,
                                 status == ai_keyboard::ConfigParseStatus::Ok);
}

// B 协议(罗技 HID++ 式):配置/音频控制经 HID 送达后,从 0x11 输入报文
// 回传 bytes/crc16 指纹;App 匹配指纹即确认送达,蓝牙下不再依赖 GATT 读回。
enum class ConfigIngressTransport : std::uint8_t {
  Usb,
  Ble,
  Wifi,
};

void send_hid_config_ack(AppContext* app,
                         std::uint8_t phase_code,
                         bool ok,
                         const std::string& json,
                         bool saved,
                         ConfigIngressTransport origin_transport,
                         std::uint32_t usb_origin_epoch,
                         ai_keyboard::BleOwnerToken ble_origin_owner) {
  const auto bytes = static_cast<std::uint16_t>(
      std::min<std::size_t>(json.size(), UINT16_MAX));
  const auto crc16 = config_json_crc16(json);
  if (origin_transport == ConfigIngressTransport::Usb) {
    if (!app->usb.send_config_ack_for_epoch(
            phase_code, ok, bytes, crc16, saved, usb_origin_epoch)) {
      ESP_LOGW(kTag,
               "CONFIG ACK USB origin expired/rejected phase=%u bytes=%u epoch=%lu",
               static_cast<unsigned>(phase_code),
               static_cast<unsigned>(bytes),
               static_cast<unsigned long>(usb_origin_epoch));
    }
    return;
  }
  if (origin_transport == ConfigIngressTransport::Ble) {
    if (!app->ble.send_config_ack_for_owner(
            phase_code, ok, bytes, crc16, saved, ble_origin_owner)) {
      ESP_LOGW(kTag,
               "CONFIG ACK BLE origin expired/unavailable phase=%u bytes=%u "
               "owner=%u/%lu",
               static_cast<unsigned>(phase_code),
               static_cast<unsigned>(bytes),
               static_cast<unsigned>(ble_origin_owner.conn_handle),
               static_cast<unsigned long>(ble_origin_owner.generation));
    }
    return;
  }
  // Wi-Fi config has no HID request origin. Its own control flow observes the
  // published status; never leak that ACK to an unrelated ambient USB/BLE host.
  ESP_LOGD(kTag,
           "CONFIG ACK skipped for Wi-Fi origin phase=%u bytes=%u",
           static_cast<unsigned>(phase_code),
           static_cast<unsigned>(bytes));
}

void apply_pending_config(AppContext* app) {
  std::string json;
  const char* source = "usb";
  ConfigIngressTransport origin_transport = ConfigIngressTransport::Usb;
  std::uint32_t usb_origin_epoch = 0;
  ai_keyboard::BleOwnerToken ble_origin_owner{};
  if (!app->usb.take_pending_config(&json, &usb_origin_epoch)) {
    source = "ble";
    origin_transport = ConfigIngressTransport::Ble;
    if (!app->ble.take_pending_config(&json, &ble_origin_owner)) {
      source = "wifi";
      origin_transport = ConfigIngressTransport::Wifi;
      if (!app->audio.take_pending_config(&json)) {
        return;
      }
    }
  }

  if (json.empty()) {
    return;
  }
  auto candidate_state = app->config_state;
  const auto status = candidate_state.apply_json(json);
  if (status != ai_keyboard::ConfigParseStatus::Ok) {
    ESP_LOGW(kTag,
             "CONFIG %s push rejected status=%s bytes=%u",
             source,
             parse_status_name(status),
             static_cast<unsigned>(json.size()));
    publish_config_status_for_json(app, "push", parse_status_name(status), json, false);
    send_hid_config_ack(
        app,
        1,
        false,
        json,
        false,
        origin_transport,
        usb_origin_epoch,
        ble_origin_owner);
    return;
  }

  esp_err_t save_err = ESP_OK;
  const auto& applied_json = candidate_state.last_applied_json();
  const bool saved = app->config_store.save_config_and_host_platform(
      applied_json, candidate_state.target_platform(), &save_err);
  const bool platform_changed =
      candidate_state.target_platform() != app->config_state.target_platform();
  if (saved) {
    if (platform_changed) {
      release_keyboard_reports(app);
    }
    app->config_state = candidate_state;
    sync_encoder_scroll_axis(app);
    sync_keyboard_audio_config(app, source);
  }
  ESP_LOGI(kTag,
           "CONFIG %s push status=ok bytes=%u save=%s",
           source,
           static_cast<unsigned>(applied_json.size()),
           saved ? "ok" : esp_err_to_name(save_err));
  publish_config_status_for_json(app,
                                 "push",
                                 saved ? "ok" : "save_failed",
                                 applied_json,
                                 saved);
  // 回执按"收到的原始 payload"计算指纹,App 端对照的是自己发出的字节。
  send_hid_config_ack(
      app,
      1,
      true,
      json,
      saved,
      origin_transport,
      usb_origin_epoch,
      ble_origin_owner);
}

void apply_pending_agent_status(AppContext* app, std::uint32_t now_ms) {
  ai_keyboard::AgentStatusCommand selected{};
  bool pending = false;
  const char* source = "none";

  ai_keyboard::AgentStatusCommand ble_command{};
  if (app->ble.take_pending_agent_status(&ble_command)) {
    selected = ble_command;
    pending = true;
    source = "ble";
  }

  ai_keyboard::AgentStatusCommand usb_command{};
  if (app->usb.take_pending_agent_status(&usb_command)) {
    if (!pending ||
        ai_keyboard::agent_status_sequence_is_newer(usb_command.sequence,
                                                    selected.sequence)) {
      selected = usb_command;
      source = "usb";
    }
    pending = true;
  }

  if (!pending) {
    return;
  }

  // Only suppress an exact repeat. Sequence values restart with the desktop
  // process, so treating them as a permanent monotonic counter would discard
  // valid commands after an App restart.
  if (app->last_agent_status_valid &&
      ai_keyboard::agent_status_commands_equal(
          selected, app->last_agent_status_command)) {
    ESP_LOGD(kTag,
             "agent status duplicate transport=%s seq=%lu source=%08lx",
             source,
             static_cast<unsigned long>(selected.sequence),
             static_cast<unsigned long>(selected.source_hash));
    return;
  }

  app->last_agent_status_valid = true;
  app->last_agent_status_command = selected;
  app->leds.set_agent_status(selected, now_ms);
  ESP_LOGI(kTag,
           "agent status transport=%s state=%u seq=%lu ttl_ms=%lu source=%08lx",
           source,
           static_cast<unsigned>(selected.state),
           static_cast<unsigned long>(selected.sequence),
           static_cast<unsigned long>(selected.ttl_ms),
           static_cast<unsigned long>(selected.source_hash));
}

bool sync_power_sample(AppContext* app, std::uint32_t now_ms, bool force) {
  if (app->audio.streaming()) {
    return false;
  }
  const auto interval = kAwakePowerSampleIntervalMs;
  if (!force && app->last_power_log_ms != 0 && now_ms - app->last_power_log_ms < interval) {
    return false;
  }
  app->last_power_log_ms = now_ms;

  const auto sample = app->battery.read();
  if (sample.error != ESP_OK) {
    ESP_LOGW(kTag, "power read failed: %s", esp_err_to_name(sample.error));
    return false;
  }

  const int sen_vin = read_optional_gpio(ai_keyboard::kExternalPowerSensePin);
  const int sen_chrg = read_optional_gpio(ai_keyboard::kChargeStatusPin);
  const auto charge_state = charge_state_for(app);
  const auto estimate = app->battery_estimator.update(sample.rail_mv, charge_state, now_ms);
  if (!sample.calibrated || !estimate.valid) {
    ESP_LOGW(kTag,
             "power sample ignored calibrated=%d measured_mv=%d",
             sample.calibrated ? 1 : 0,
             sample.rail_mv);
    return false;
  }

  if (estimate.full_anchor_updated) {
    esp_err_t save_error = ESP_OK;
    if (!app->config_store.save_battery_full_anchor_mv(estimate.full_anchor_mv, &save_error)) {
      ESP_LOGW(kTag,
               "battery full anchor save failed measured_mv=%d: %s",
               estimate.full_anchor_mv,
               esp_err_to_name(save_error));
    }
  }

  ESP_LOGI(kTag,
           "power raw=%d sense_mv=%d measured_mv=%d corrected_mv=%d battery=%u%% adc_calibrated=%s full_anchor=%d interval_ms=%lu sen_vin=%d sen_chrg=%d charge=%s",
           sample.raw,
           sample.sense_mv,
           sample.rail_mv,
           estimate.corrected_mv,
           static_cast<unsigned>(estimate.percent),
           sample.calibrated ? "true" : "false",
           estimate.full_anchor_mv,
           static_cast<unsigned long>(interval),
           sen_vin,
           sen_chrg,
           charge_state_name(charge_state));
  app->battery_raw_mv = static_cast<std::uint16_t>(
      std::clamp(sample.rail_mv, 0, static_cast<int>(UINT16_MAX)));
  app->battery_mv = static_cast<std::uint16_t>(
      std::clamp(estimate.corrected_mv, 0, static_cast<int>(UINT16_MAX)));
  app->battery_percent = estimate.percent;
  app->battery_sample_ms = now_ms;
  app->battery_sample_valid = true;
  app->ble.update_battery_level(app->battery_percent);
  return true;
}

void process_pending_status_refresh(AppContext* app, std::uint32_t now_ms) {
  ai_keyboard::StatusHidRequest usb_request{};
  std::uint32_t usb_request_epoch = 0;
  const bool usb_requested =
      app->usb.take_pending_status_request(&usb_request, &usb_request_epoch);
  const bool ble_requested =
      app->status_refresh_pending.exchange(false, std::memory_order_acq_rel);
  if (!usb_requested && !ble_requested) {
    return;
  }

  const bool fresh_requested =
      ble_requested ||
      (usb_requested &&
       (usb_request.flags & ai_keyboard::kStatusRequestFlagFresh) != 0);
  [[maybe_unused]] const bool sampled = fresh_requested &&
      sync_power_sample(app, now_ms, true);
  // Status reads on every management transport describe the same persisted
  // configuration fact. Runtime battery/connection fields may change, but
  // they must never erase the config bytes/CRC used for sync confirmation.
  const auto& applied_json = app->config_state.last_applied_json();
  const auto applied_crc =
      applied_json.empty() ? 0 : config_json_crc16(applied_json);

#if defined(EASY_INPUT_SPEAKER_OPUS_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_IMA_ADPCM_DIAGNOSTIC)
  // Capture one immutable probe generation and build one wire object for both
  // transports. A simultaneous BLE refresh must not replace it with a battery
  // view, and USB confirmation must not drop its diagnostic fields.
  const auto speaker_probe = app->speaker.probe_snapshot();
  const auto speaker_status_json = publish_config_status(
      app,
      "spk_probe",
      ai_keyboard::speaker_probe_result_name(speaker_probe.result),
      applied_json.size(),
      applied_crc,
      true,
      false,
      &speaker_probe);
  if (usb_requested &&
      (speaker_status_json.empty() ||
       !app->usb.queue_status_response_for_epoch(
           usb_request.request_id,
           speaker_status_json,
           usb_request_epoch))) {
    ESP_LOGW(kTag,
             "USB speaker probe status unavailable request=%08lx bytes=%u",
             static_cast<unsigned long>(usb_request.request_id),
             static_cast<unsigned>(speaker_status_json.size()));
  }
  return;
#else
  if (usb_requested) {
    // A USB status response doubles as recovery after a lost config ACK, so
    // its fingerprint must describe the currently applied/persisted config,
    // not the 0x13 request itself. An empty config is the valid factory case.
    const auto status_json = publish_config_status(
        app,
        "status",
        sampled ? "fresh" : "cached",
        applied_json.size(),
        applied_crc,
        true,
        true);
    if (status_json.empty() ||
        !app->usb.queue_status_response_for_epoch(
            usb_request.request_id, status_json, usb_request_epoch)) {
      ESP_LOGW(kTag,
               "USB STATUS response unavailable request=%08lx bytes=%u",
               static_cast<unsigned long>(usb_request.request_id),
               static_cast<unsigned>(status_json.size()));
    }
  }

  // GATT keeps detailed battery/power metadata, but carries the same stable
  // config fingerprint as USB so repeated reads and App reactivation can
  // revalidate device sync without relying on a stale push acknowledgement.
  if (ble_requested) {
    publish_config_status(app,
                          "battery",
                          sampled ? "fresh" : "cached",
                          applied_json.size(),
                          applied_crc,
                          true);
  }
#endif
}

void log_heartbeat(AppContext* app, std::uint32_t now_ms) {
  if (now_ms - app->last_heartbeat_log_ms < kAwakeHeartbeatLogIntervalMs) {
    return;
  }
  app->last_heartbeat_log_ms = now_ms;
  ESP_LOGI(kTag,
           "heartbeat ms=%lu firmware=%s keys=%u leds=%u audio=%s power_state=awake inactive_ms=%lu next_wait=%s next_wait_ms=%lu",
           static_cast<unsigned long>(now_ms),
           kFirmwareVersion,
           static_cast<unsigned>(ai_keyboard::kKeyPins.size()),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           ai_keyboard::kAudioHardwareAvailable ? "available" : "unavailable",
           static_cast<unsigned long>(now_ms - app->last_user_activity_ms),
           app->next_awake_wait.reason,
           static_cast<unsigned long>(app->next_awake_wait.wait_ms));
}

void log_boot_summary() {
  const esp_reset_reason_t reset_reason = esp_reset_reason();
  ESP_LOGI(kTag,
           "reset_reason=%d brownout=%d",
           static_cast<int>(reset_reason),
           reset_reason == ESP_RST_BROWNOUT ? 1 : 0);
  ESP_LOGI(kTag, "%s firmware %s", kFirmwareName, kFirmwareVersion);
  ESP_LOGI(kTag,
           "board=%s target=esp32s3 keys=%u encoder=(GPIO%u,GPIO%u,GPIO%u) leds=%u led_gpio=GPIO%u key_wake=GPIO%d battery=(en GPIO%d adc GPIO%d)",
           ai_keyboard::kBoardName,
           static_cast<unsigned>(ai_keyboard::kKeyPins.size()),
           static_cast<unsigned>(ai_keyboard::kEncoderPinA),
           static_cast<unsigned>(ai_keyboard::kEncoderPinB),
           static_cast<unsigned>(ai_keyboard::kEncoderPressPin),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           static_cast<unsigned>(ai_keyboard::kWs2812Pin),
           static_cast<int>(ai_keyboard::kKeyWakePin),
           static_cast<int>(ai_keyboard::kBatterySenseEnablePin),
           static_cast<int>(ai_keyboard::kBatterySenseAdcPin));
  ESP_LOGI(kTag,
           "v2 power pins sen_vin=GPIO%d active=%d sen_chrg=GPIO%d key_wake=GPIO%d pwr_en=GPIO%d active=%d",
           static_cast<int>(ai_keyboard::kExternalPowerSensePin),
           static_cast<int>(ai_keyboard::kExternalPowerSenseActiveLevel),
           static_cast<int>(ai_keyboard::kChargeStatusPin),
           static_cast<int>(ai_keyboard::kKeyWakePin),
           static_cast<int>(ai_keyboard::kPeripheralPowerEnablePin),
           static_cast<int>(ai_keyboard::kPeripheralPowerEnableActiveLevel));
  ESP_LOGI(kTag,
           "v2 audio pins mic_i2s=(bclk GPIO%d ws GPIO%d din GPIO%d right) spk_i2s=(bclk GPIO%d ws GPIO%d dout GPIO%d left)",
           static_cast<int>(ai_keyboard::kMicI2sBclkPin),
           static_cast<int>(ai_keyboard::kMicI2sWsPin),
           static_cast<int>(ai_keyboard::kMicI2sDataInPin),
           static_cast<int>(ai_keyboard::kSpkI2sBclkPin),
           static_cast<int>(ai_keyboard::kSpkI2sWsPin),
           static_cast<int>(ai_keyboard::kSpkI2sDataOutPin));
  ESP_LOGI(kTag, "%s", ai_keyboard::kAudioUnavailableReason);
  ESP_LOGI(kTag,
           "power policy awake_scheduler=event_deadline deep_sleep_after=%lums battery_sample=%lums automatic_light_sleep=false",
           static_cast<unsigned long>(kDeepSleepAfterMs),
           static_cast<unsigned long>(kAwakePowerSampleIntervalMs));
  ESP_LOGI(kTag,
           "BLE connection power stable_interval=%u-%u stable_latency=%u supervision_timeout=%u",
           static_cast<unsigned>(kBleStableConnIntervalMin),
           static_cast<unsigned>(kBleStableConnIntervalMax),
           static_cast<unsigned>(kBleStableConnLatency),
           static_cast<unsigned>(kBleStableConnSupervisionTimeout));
}

void wait_for_awake_work(const ai_keyboard::AwakeWaitDecision& decision) {
  if (decision.immediate) {
    return;
  }
  if (!decision.has_deadline) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return;
  }
  ulTaskNotifyTake(pdTRUE, delay_ticks(decision.wait_ms));
}

bool bridged_hotkey_work_pending(const AppContext* app) {
  for (const auto& delivery : app->bridged_hotkey_deliveries) {
    if (delivery.pending()) {
      return true;
    }
  }
  return false;
}

ai_keyboard::AwakeWaitDecision plan_next_awake_work(AppContext* app,
                                                    std::uint32_t now_ms) {
  ai_keyboard::AwakeWaitPlanner planner(now_ms);

  std::uint32_t deadline_ms = 0;
  const bool input_deadline =
      app->inputs.next_transition_deadline_ms(&deadline_ms);
  planner.add_deadline(input_deadline, deadline_ms, "input_debounce");
  // An ISR may publish work while this plan is being assembled. Debounce is
  // deadline-driven; raw snapshots/encoder steps that have no such deadline
  // are immediately runnable. A notification arriving after this snapshot is
  // retained by the task notification counter.
  planner.request_now(app->inputs.activity_pending() && !input_deadline,
                      "input_edge");

  deadline_ms = 0;
  const bool usb_presence_deadline =
      app->usb_physical_presence.next_update_deadline_ms(&deadline_ms);
  planner.add_deadline(usb_presence_deadline, deadline_ms, "usb_presence");

  deadline_ms = 0;
  const bool encoder_hold_deadline =
      app->encoder_press_gesture.config_deadline(
          kEncoderConfigModeHoldMs, &deadline_ms);
  planner.add_deadline(
      encoder_hold_deadline,
      deadline_ms,
      "encoder_hold");
  deadline_ms = 0;
  const bool platform_deadline =
      app->platform_selection.next_deadline_ms(&deadline_ms);
  planner.add_deadline(platform_deadline,
                       deadline_ms,
                       "platform_selection");

  if (!app->pending_wheel_reports.empty() && app->last_wheel_flush_ms != 0) {
    planner.add_deadline(true,
                         app->last_wheel_flush_ms + kEncoderWheelFlushIntervalMs,
                         "wheel_flush");
  }
  deadline_ms = 0;
  const bool led_deadline =
      app->leds.next_update_deadline_ms(now_ms, &deadline_ms);
  planner.add_deadline(led_deadline,
                       deadline_ms,
                       "status_led");
  deadline_ms = 0;
  const bool cold_boot_feedback_deadline =
      app->cold_boot_feedback.next_deadline_ms(&deadline_ms);
  planner.add_deadline(cold_boot_feedback_deadline,
                       deadline_ms,
                       "cold_boot_feedback_liveness");

  if (!app->audio.streaming()) {
    planner.add_deadline(true,
                         app->last_power_log_ms + kAwakePowerSampleIntervalMs,
                         "battery_sample");
  }
  planner.add_deadline(true,
                       app->last_heartbeat_log_ms +
                           kAwakeHeartbeatLogIntervalMs,
                       "heartbeat");

#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  const bool speaker_assets_steps_allowed =
      speaker_asset_resource_steps_allowed(app, now_ms);
  const auto speaker_assets_schedule =
      app->speaker_assets.work_schedule(
          now_ms, speaker_assets_steps_allowed);
  planner.add_schedule(speaker_assets_schedule, "speaker_assets");

  const auto speaker_boot_schedule =
      app->speaker_assets.boot_work_schedule();
  planner.add_schedule(speaker_boot_schedule, "speaker_boot");
  // A release completion may change ReleasePending to Idle between owner
  // passes. The startup coordinator still needs one final pass to consume Idle
  // and enter Ready. The phase guard keeps the supervisor's steady Idle state
  // from becoming a permanent busy loop.
  planner.request_now(
      app->speaker_startup_phase == SpeakerStartupPhase::WaitLeaseIdle &&
          app->speaker.shutdown_complete() &&
          app->speaker_assets.boot_idle(),
      "speaker_boot_idle");

  // Store work is intentionally denied for 30ms after physical input. That
  // gate is time-based and emits no completion notification when it expires,
  // so outstanding Boot work must publish the exact deadline. Preparing and
  // ReleasePending remain non-runnable while they wait for the Store worker.
  const bool speaker_assets_input_quiet =
      (speaker_assets_schedule.outstanding ||
       speaker_boot_schedule.outstanding) &&
      app->last_input_ms != 0U &&
      now_ms - app->last_input_ms < kSpeakerAssetsInputQuietMs;
  planner.add_deadline(
      speaker_assets_input_quiet,
      app->last_input_ms + kSpeakerAssetsInputQuietMs,
      "speaker_assets_input_quiet");

  // The worker publishes quiesced before it can suspend itself. The matching
  // notification may therefore wake the owner a few instructions too early;
  // I2S disable/delete can also report a transient failure. Revisit teardown
  // at a bounded cadence until the owner observes the exact suspended task and
  // releases the channel. This is a short lifecycle deadline, not a periodic
  // Awake poll and never an immediate busy loop.
  const bool speaker_shutdown_settle =
      (app->speaker_startup_phase ==
           SpeakerStartupPhase::ShutdownOutput ||
       app->speaker_startup_phase ==
           SpeakerStartupPhase::ReleaseLease) &&
      !app->speaker.shutdown_complete();
  planner.add_deadline(
      speaker_shutdown_settle,
      now_ms + kSpeakerShutdownSettleRetryMs,
      "speaker_shutdown_settle");

  planner.add_deadline(app->speaker_local_retry_after_ms != 0U,
                       app->speaker_local_retry_after_ms,
                       "speaker_local_retry");
  const bool speaker_wifi_retry_eligible =
      app->speaker_wifi_admission ==
          ai_keyboard::SpeakerWifiAdmissionState::Deferred &&
      app->speaker_startup_phase == SpeakerStartupPhase::Ready &&
      app->speaker.shutdown_complete() &&
      app->speaker_assets.boot_idle() && !app->speaker.busy() &&
      !app->audio.streaming() &&
      !app->audio_io_arbiter.microphone_requested();
  // A deferred carrier retry is intentionally dormant while Boot owns the
  // shared internal heap. Publishing an already-expired deadline during that
  // interval would turn the event-driven Awake owner into a busy loop. The
  // transition to Ready is itself scheduled, then this exact retry deadline
  // becomes eligible on the following owner pass.
  planner.add_deadline(
      speaker_wifi_retry_eligible &&
          app->speaker_wifi_retry_after_ms != 0U,
      app->speaker_wifi_retry_after_ms,
      "speaker_wifi_retry");
#endif

  planner.request_now(
      app->status_refresh_pending.load(std::memory_order_acquire),
      "status_refresh");
  planner.add_schedule(app->usb.work_schedule(now_ms), "usb_hid");

  std::uint64_t ble_deadline_us = 0;
  if (app->ble.next_work_deadline_us(&ble_deadline_us)) {
    const auto now_us = static_cast<std::uint64_t>(esp_timer_get_time());
    const auto delta_us = ble_deadline_us > now_us
                              ? ble_deadline_us - now_us
                              : 0U;
    const auto delta_ms = static_cast<std::uint32_t>(
        std::min<std::uint64_t>((delta_us + 999U) / 1000U,
                                0x7fffffffU));
    planner.add_deadline(true, now_ms + delta_ms, "ble");
  }
  // These owner-level states are normally advanced by BLE/USB completion
  // notifications. They remain Deep Sleep blockers, but are not treated as
  // runnable here after an attempted enqueue, which prevents endpoint-busy
  // spin loops.
  static_cast<void>(bridged_hotkey_work_pending(app));

  const auto inactive_ms = now_ms - app->last_user_activity_ms;
  if (inactive_ms < kDeepSleepAfterMs) {
    planner.add_deadline(true,
                         app->last_user_activity_ms + kDeepSleepAfterMs,
                         "deep_sleep");
  } else {
    const auto deep_sleep = ai_keyboard::evaluate_deep_sleep_policy(
        power_policy_inputs(app, now_ms));
    if (deep_sleep.deep_sleep_allowed) {
      // A pre-commit operation may have failed without an asynchronous owner
      // event. Retry at a bounded cadence; successful commit never returns.
      planner.add_deadline(true, now_ms + 1000U, "deep_sleep_retry");
    }
  }
  return planner.decision();
}

}  // namespace

extern "C" void app_main(void) {
#if defined(EASY_INPUT_SPEAKER_ASSETS_DIAGNOSTIC)
  // Link-only diagnostic: returns a retained table address and performs no
  // sound-bank read, erase or write. The volatile sink keeps this root
  // observable even if a future diagnostic build enables whole-program LTO.
  const void* volatile speaker_assets_link_anchor =
      easy_input_speaker_assets_diagnostic_link_anchor();
  static_cast<void>(speaker_assets_link_anchor);
#endif
  static AppContext app;
  app.platform_task = xTaskGetCurrentTaskHandle();

  log_boot_summary();
  const auto wake_cause = esp_sleep_get_wakeup_cause();
  restore_retained_power_cycle(&app, wake_cause);
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    // Deep Sleep was woken by the board's diode-combined KEY_WAKE line. This
    // proves the wake path for the new boot and allows a later Deep Sleep
    // transaction after another full inactivity window.
    app.key_wake_verified = true;
    app.last_wake_reason = "deep_sleep_key_wake";
    ESP_LOGI(kTag, "boot from deep sleep via KEY_WAKE");
  }
  configure_power_management();
  configure_board_status_inputs();
  ESP_ERROR_CHECK(app.peripheral_power.begin_awake());
  app.last_user_activity_ms = millis();
  ESP_ERROR_CHECK(easy_input::initialize_nvs_storage());
  app.inputs.set_notify_task(xTaskGetCurrentTaskHandle());
  ESP_ERROR_CHECK(app.inputs.begin(millis()));
  configure_board_status_notifications(&app);
  app.audio_io_arbiter.set_work_ready_callback(signal_async_work, &app);
  app.audio.set_audio_io_arbiter(&app.audio_io_arbiter);
  const esp_err_t audio_err = app.audio.begin();
  app.audio_ready = audio_err == ESP_OK;
  if (audio_err != ESP_OK) {
    ESP_LOGW(kTag, "Keyboard audio link unavailable: %s", esp_err_to_name(audio_err));
  } else {
    app.audio.set_work_ready_callback(signal_async_work, &app);
  }
  app.ble.set_status_read_callback(signal_status_read, &app);
  app.ble.set_work_ready_callback(signal_async_work, &app);
  ESP_ERROR_CHECK(app.ble.begin());
  app.usb.set_work_ready_callback(signal_async_work, &app);
  if constexpr (ai_keyboard::kExternalPowerSensePin >= 0) {
    app.usb_physical_presence.reset(
        usb_vbus_status_present(), millis());
  }
  const esp_err_t usb_err = app.usb.begin();
  if (usb_err != ESP_OK) {
    ESP_LOGW(kTag, "USB HID unavailable: %s", esp_err_to_name(usb_err));
  }
  publish_config_status_without_payload(&app, "boot", "initializing", false);
  app.deep_sleep_wakeup_configured = configure_deep_sleep_wakeup();
  ESP_ERROR_CHECK(app.leds.begin());
  const esp_err_t cdc_err = app.cdc_light.begin(&app.leds);
  if (cdc_err != ESP_OK) {
    ESP_LOGW(kTag, "CDC light control unavailable: %s", esp_err_to_name(cdc_err));
    easy_input::g_light_frame_sink = nullptr;
  } else {
    // v1.9 BLE 多灯通道：挂载全局帧入口，BLE 写回调经 submit_frame 提交。
    easy_input::g_light_frame_sink = &app.cdc_light;
  }
  apply_cold_boot_feedback_action(
      &app,
      app.cold_boot_feedback.begin(
          wake_cause != ESP_SLEEP_WAKEUP_EXT1),
      "cold_boot_reserved");
  ESP_ERROR_CHECK(app.battery.begin());
  std::int32_t stored_battery_full_mv = 0;
  esp_err_t battery_anchor_error = ESP_OK;
  if (app.config_store.load_battery_full_anchor_mv(&stored_battery_full_mv,
                                                   &battery_anchor_error)) {
    if (!app.battery_estimator.set_full_anchor_mv(stored_battery_full_mv)) {
      ESP_LOGW(kTag,
               "ignored invalid stored battery full anchor measured_mv=%ld",
               static_cast<long>(stored_battery_full_mv));
    }
  } else if (battery_anchor_error != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(kTag,
             "battery full anchor load failed: %s",
             esp_err_to_name(battery_anchor_error));
  }
  sync_power_sample(&app, millis(), true);
  load_stored_config(&app);
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  // Keep the microphone's frozen resource pool first. The platform loop then
  // starts local Store/USB and the required Wi-Fi sound service before the
  // optional Boot playback pipeline. Playback can be skipped or fail without
  // suppressing the listener advertised through the signed heartbeat.
  app.speaker_skip_boot_after_deep_sleep =
      wake_cause == ESP_SLEEP_WAKEUP_EXT1;
  if (app.speaker_skip_boot_after_deep_sleep) {
    ESP_LOGI(kTag, "speaker boot sound skipped after deep-sleep key wake");
  }
#elif defined(EASY_INPUT_SPEAKER_DIAGNOSTIC)
  app.speaker.mark_boot_pending(
      app.audio_io_arbiter.microphone_generation());
  const esp_err_t speaker_err =
      app.speaker.begin(app.platform_task, &app.audio_io_arbiter);
  if (speaker_err != ESP_OK) {
    ESP_LOGW(kTag,
             "speaker diagnostic unavailable: %s",
             esp_err_to_name(speaker_err));
    settle_cold_boot_silent(&app, "diagnostic_speaker_begin_failed");
  }
  if (speaker_err == ESP_OK) {
    if (wake_cause != ESP_SLEEP_WAKEUP_EXT1) {
      // The boot sound is a local device capability. Once the speaker is ready,
      // request it for cold/restart boots regardless of USB/VBUS, BLE, or App
      // state. A key wake from our 30-minute deep sleep is not a power-on event.
      app.speaker_probe_pending = true;
    } else {
      ESP_LOGI(kTag, "speaker boot sound skipped after deep-sleep key wake");
    }
  }
#endif
#if !defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) && \
    !defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
  settle_cold_boot_silent(&app, "speaker_feature_not_built");
#endif
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    ESP_LOGI(kTag, "LED boot self-test skipped after deep-sleep key wake");
  }
  // Capture the initial USB/BLE state after Boot owns the strip. Status events
  // are retained by the LED owner and replayed after the self-test, preserving
  // the existing blue connected feedback without serializing speaker startup.
  sync_led_status(&app, millis());
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    const auto recovered_mask = app.inputs.recover_pressed_after_deep_sleep(
        millis(), handle_input_event, &app);
    ESP_LOGI(kTag,
             "deep-sleep wake input recovery mask=0x%03lx",
             static_cast<unsigned long>(recovered_mask));
  }

  // Reservation starts before initial status capture, while this watchdog
  // starts only now: the owner loop below is the first point at which
  // Store/Wi-Fi/Speaker admission can actually make progress. Platform setup
  // time must not consume the audio admission budget.
  app.cold_boot_feedback.start_admission_window(
      millis(), kColdBootFeedbackMaxAdmissionWaitMs);

  ESP_LOGI(kTag, "platform loop started; BLE HID + USB HID + S3C config enabled");
  while (true) {
    const auto now = millis();
    sync_usb_physical_presence(&app, now);
    if (app.inputs.take_wake_edge_count() > 0) {
      app.key_wake_verified = true;
      app.audio.cancel_wifi_release_for_device_activity();
    }
    if (app.inputs.take_input_edge_count() > 0) {
      // Raw edges wake the owner and cancel a reversible shutdown prepare,
      // but only the debounced event below advances last_user_activity_ms.
      app.audio.cancel_wifi_release_for_device_activity();
    }
    apply_pending_config(&app);
    apply_pending_agent_status(&app, millis());
    app.cdc_light.apply_pending(millis());
    reconcile_keyboard_transport_lifetimes(&app);
    // BLE profile preparation above may take long enough for both edges of a
    // short click to arrive. Poll with a fresh timestamp so queued ISR
    // snapshots and the settling sample always stay in chronological order.
    app.inputs.poll(millis(), handle_input_event, &app);
    flush_pending_wheel_report(&app, millis());
#if defined(EASY_INPUT_SPEAKER_DIAGNOSTIC) || \
    defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
    // Producer pass: publish a new Boot Store request (or consume an older
    // Speaker worker event) before the shared resource owner is serviced.
    // Without this pass the first Store worker can sleep forever with priority
    // denied because no later notification is guaranteed to revisit poll().
    service_speaker(&app);
#endif
    // Drain high-priority input before publishing the Store priority gate.
    // If this pass clears the final queued report, the same owner pass must
    // reopen bounded Flash work; waiting for an unrelated future notification
    // can otherwise strand a Preparing Boot job.
    app.usb.poll_pending_reports();
    app.ble.poll_input_delivery(millis());
    // Progress native Shift+Arrow chords even without a new input edge. A
    // rejected source run remains retained by the scanner until a complete
    // press/restore pair can enter the exact-owner keyboard FIFO in order.
    flush_encoder_text_selection(&app);
    // Polling may have freed a bounded transport slot. Retry the coalesced
    // latest full keyboard state and every stateful App hotkey transition once
    // per loop, outside the input callback.
    flush_pending_keyboard_snapshot(&app);
    flush_pending_bridged_hotkey_events(&app);
    flush_input_led_feedback(&app);
#if defined(EASY_INPUT_SPEAKER_ASSETS_PRODUCT)
    // Resource pass: grant the newly-published Store job or consume an already
    // published completion using the final input-queue state from this pass.
    // Both operations are bounded owner-side steps.
    const auto resource_now = millis();
    app.speaker_assets.poll(
        resource_now,
        speaker_asset_resource_steps_allowed(
            &app, resource_now));
    // Consumer pass: immediately observe any synchronous state transition
    // made by poll(). This is a fixed two-pass pump, not an unbounded drain;
    // asynchronous Flash/Speaker work still resumes only by notification.
    service_speaker(&app);
#endif
    // Timeout is the final arbitration step: first consume input delivery,
    // Store, and Speaker worker publications so an accepted request/first PCM
    // cannot be misclassified as silence merely because notifications were
    // coalesced.
    service_cold_boot_feedback_liveness(&app, millis());
    // Apply config above and deliver physical input first. A status request
    // arriving with the final config chunk must observe the new saved state.
    process_pending_status_refresh(&app, millis());
    const auto encoder_hold_now = millis();
    check_encoder_press_config_hold(
        &app,
        encoder_hold_now,
        encoder_hold_now,
        app.inputs.low_active_pressed(ai_keyboard::kEncoderPressPin));
    const auto platform_selection_now = millis();
    handle_platform_selection_result(
        &app,
        app.platform_selection.update(platform_selection_now),
        platform_selection_now);
    sync_led_status(&app, millis());
    sync_audio_power_hold(&app);
    app.leds.update(millis());
    if (app.cold_boot_feedback.state() ==
            ai_keyboard::ColdBootFeedbackState::VisualRunning &&
        !app.leds.cold_boot_sequence_active()) {
      app.cold_boot_feedback.mark_visual_complete();
    }
    sync_power_sample(&app, millis(), false);
    log_heartbeat(&app, millis());
    maybe_enter_deep_sleep(&app, millis());
    app.next_awake_wait = plan_next_awake_work(&app, millis());
    wait_for_awake_work(app.next_awake_wait);
  }
}
