#include "platform/led_strip_status.h"

#include <cmath>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace easy_input {
namespace {

const char* const kTag = "status_led";
constexpr std::uint32_t kRmtResolutionHz = 20 * 1000 * 1000;
// v1.6 CDC 灯控：守护进程心跳 10s，30s 无帧整体退出 5 灯模式。
constexpr std::uint32_t kMultiAgentTimeoutMs = 30000;
constexpr std::uint16_t kT0hTicks = 6;       // 300 ns at 20 MHz
constexpr std::uint16_t kT0lTicks = 18;      // 900 ns at 20 MHz
constexpr std::uint16_t kT1hTicks = 16;      // 800 ns at 20 MHz
constexpr std::uint16_t kT1lTicks = 12;      // 600 ns at 20 MHz
constexpr std::uint16_t kResetTicks = 6000;  // 300 us at 20 MHz
constexpr std::size_t kWs2812SymbolCount = (ai_keyboard::kWs2812Count * 24U) + 1U;
// Five WS2812 pixels finish in under 1 ms. The project tick is 10 ms, so a
// literal 1 ms timeout becomes zero ticks; allow two ticks for completion
// while still bounding a faulty transfer.
constexpr int kRmtFlushWaitMs = 2 * portTICK_PERIOD_MS;
// v1.10 动画参数（可在此统一调整；守护进程侧 config 亦提供镜像说明）
constexpr std::uint32_t kMultiAgentAnimFrameMs = 50;  // 动画渲染帧间隔（20fps）
constexpr int kWorkFlashCount = 5;                    // 开始工作提醒：蓝快闪 5 次
constexpr std::uint32_t kWorkFlashOnMs = 150;         // 快闪亮
constexpr std::uint32_t kWorkFlashOffMs = 150;        // 快闪灭
constexpr std::uint32_t kBreathPeriodMs = 4000;       // 工作慢呼吸周期（正弦，4s 与等待慢闪 1s/1s 区分）
constexpr std::uint32_t kWorkDimAfterMs = 5 * 60 * 1000; // 工作持续 5 分钟后降为低亮呼吸（省寿命/省电）
constexpr std::uint32_t kWaitOnMs = 1000;             // 待确认慢闪亮 1s
constexpr std::uint32_t kWaitOffMs = 1000;            // 待确认慢闪灭 1s
constexpr std::uint32_t kWaitFlashMs = 60U * 1000U;   // 待确认周期：闪 1min
constexpr std::uint32_t kWaitQuietMs = 4U * 60U * 1000U;  // 待确认周期：灭 4min
constexpr std::uint32_t kErrOnMs = 300;               // 错误快闪亮 0.3s
constexpr std::uint32_t kErrOffMs = 300;              // 错误快闪灭 0.3s
constexpr std::uint32_t kErrFlashMs = 30U * 1000U;    // 错误周期：闪 30s
constexpr std::uint32_t kErrQuietMs = 4U * 60U * 1000U + 30U * 1000U;  // 错误周期：灭 4.5min
constexpr std::uint32_t kDoneFlashMs = 30U * 1000U;   // 完成：绿闪 30s（快完成信号）
constexpr std::uint32_t kDoneSolidMs = 30U * 1000U;   // 完成：绿常亮 30s（已完成的确认）
constexpr std::uint32_t kDoneOnMs = 1000;             // 完成绿闪：亮 1s
constexpr std::uint32_t kDoneOffMs = 1000;            // 完成绿闪：灭 1s
constexpr std::array<Rgb, ai_keyboard::BootLedSequence::kPixelCount>
    kColdBootProbeColors{{
        {34, 0, 0},
        {28, 10, 0},
        {0, 28, 0},
        {0, 0, 34},
        {24, 24, 24},
    }};
static_assert(ai_keyboard::kWs2812Count ==
              ai_keyboard::BootLedSequence::kPixelCount);

Rgb to_rgb(ai_keyboard::FeedbackColor color) {
  return {color.red, color.green, color.blue};
}

Rgb scale_rgb(Rgb color, std::uint8_t numerator, std::uint8_t denominator) {
  return {
      static_cast<std::uint8_t>((static_cast<unsigned>(color.red) * numerator) / denominator),
      static_cast<std::uint8_t>((static_cast<unsigned>(color.green) * numerator) / denominator),
      static_cast<std::uint8_t>((static_cast<unsigned>(color.blue) * numerator) / denominator),
  };
}

bool deadline_pending(std::uint32_t now_ms, std::uint32_t deadline_ms) {
  return static_cast<std::int32_t>(deadline_ms - now_ms) > 0;
}

Rgb agent_status_color(ai_keyboard::AgentStatusState state) {
  switch (state) {
    case ai_keyboard::AgentStatusState::kRunning:
      return {0, 0, 36};   // 纯蓝（提高亮度，让呼吸明显可辨）
    case ai_keyboard::AgentStatusState::kWaitingUser:
      return {30, 20, 0};  // 橙色（R:G≈3:2，待确认醒目且区别于纯黄）
    case ai_keyboard::AgentStatusState::kCompletedUnread:
      return {0, 28, 0};   // 纯绿（正色化）
    case ai_keyboard::AgentStatusState::kFailed:
      return {30, 0, 0};   // 纯红（正色化，去粉）
    case ai_keyboard::AgentStatusState::kIdle:
      return {};
  }
  return {};
}

// 慢呼吸调制：kBreathPeriodMs 周期正弦。full=全亮(0.0~1.0)；dim=低亮(0.05~0.2，工作超 5 分钟后用)。
Rgb breathe_rgb_amp(Rgb base, std::uint32_t phase_ms, double center, double amp) {
  const double t = static_cast<double>(phase_ms % kBreathPeriodMs) /
                   static_cast<double>(kBreathPeriodMs);
  const double k = center + amp * std::sin(2.0 * 3.14159265358979 * t);
  return {
      static_cast<std::uint8_t>(static_cast<double>(base.red) * k),
      static_cast<std::uint8_t>(static_cast<double>(base.green) * k),
      static_cast<std::uint8_t>(static_cast<double>(base.blue) * k),
  };
}
Rgb breathe_rgb(Rgb base, std::uint32_t phase_ms) {
  return breathe_rgb_amp(base, phase_ms, 0.5, 0.5); // 全亮 0.0~1.0（最暗全灭，呼吸更明显）
}
Rgb dim_breathe_rgb(Rgb base, std::uint32_t phase_ms) {
  return breathe_rgb_amp(base, phase_ms, 0.125, 0.075); // 低亮 0.05~0.2
}

// 点亮时间占比（占空比）控制：周期内 active 时间段点亮，否则灭。
inline bool duty_cycle_active(std::uint32_t now_ms,
                              std::uint32_t since_ms,
                              std::uint32_t active_ms,
                              std::uint32_t quiet_ms) {
  const std::uint32_t period = active_ms + quiet_ms;
  if (period == 0) {
    return true;
  }
  return (now_ms - since_ms) % period < active_ms;
}

void set_scaled_pixel(std::array<Rgb, ai_keyboard::kWs2812Count>* leds,
                      std::size_t index,
                      Rgb color,
                      std::uint8_t numerator,
                      std::uint8_t denominator) {
  if (leds == nullptr || index >= leds->size()) {
    return;
  }
  (*leds)[index] = scale_rgb(color, numerator, denominator);
}

Rgb rainbow_color(std::uint8_t index) {
  constexpr std::array<Rgb, 7> kRainbow{{
      {46, 0, 5},
      {46, 17, 0},
      {38, 35, 0},
      {0, 43, 11},
      {0, 36, 43},
      {12, 0, 47},
      {42, 0, 36},
  }};
  return kRainbow[index % kRainbow.size()];
}

void encode_ws2812_byte(std::uint8_t value, rmt_symbol_word_t* out) {
  if (out == nullptr) {
    return;
  }
  for (std::uint8_t bit = 0; bit < 8; ++bit) {
    const bool one = (value & (1U << (7U - bit))) != 0;
    out[bit].level0 = 1;
    out[bit].duration0 = one ? kT1hTicks : kT0hTicks;
    out[bit].level1 = 0;
    out[bit].duration1 = one ? kT1lTicks : kT0lTicks;
  }
}

}  // namespace

esp_err_t StatusLedStrip::begin() {
  rmt_tx_channel_config_t rmt_config = {};
  rmt_config.gpio_num = static_cast<gpio_num_t>(ai_keyboard::kWs2812Pin);
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = kRmtResolutionHz;
  rmt_config.mem_block_symbols = 64;
  rmt_config.trans_queue_depth = 1;
  rmt_config.flags.invert_out = false;
  rmt_config.flags.with_dma = false;
  const esp_err_t channel_err = rmt_new_tx_channel(&rmt_config, &rmt_channel_);
  if (channel_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_tx_channel GPIO%u failed: %s",
             static_cast<unsigned>(ai_keyboard::kWs2812Pin),
             esp_err_to_name(channel_err));
    return channel_err;
  }

  rmt_copy_encoder_config_t encoder_config = {};
  const esp_err_t encoder_err = rmt_new_copy_encoder(&encoder_config, &rmt_copy_encoder_);
  if (encoder_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_new_copy_encoder failed: %s", esp_err_to_name(encoder_err));
    return encoder_err;
  }

  clear();

  ESP_LOGI(kTag,
           "configured WS2812 custom RMT GPIO%u count=%u rmt_hz=%lu timing=(%u,%u,%u,%u)",
           static_cast<unsigned>(ai_keyboard::kWs2812Pin),
           static_cast<unsigned>(ai_keyboard::kWs2812Count),
           static_cast<unsigned long>(kRmtResolutionHz),
           static_cast<unsigned>(kT0hTicks),
           static_cast<unsigned>(kT0lTicks),
           static_cast<unsigned>(kT1hTicks),
           static_cast<unsigned>(kT1lTicks));
  return ESP_OK;
}

bool StatusLedStrip::ready() const {
  return rmt_channel_ != nullptr && rmt_copy_encoder_ != nullptr;
}

void StatusLedStrip::reserve_cold_boot_sequence() {
  const bool already_reserved = cold_boot_sequence_.reserved();
  cold_boot_sequence_.reserve();
  if (!already_reserved) {
    deferred_feedback_.clear();
  }
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  cursor_ = 0;
  idle_rendered_ = false;

  // Deliberately do not render or flush here. Reservation is only logical
  // ownership while the speaker reaches its authoritative playback edge.
  ESP_LOGI(kTag, "cold boot LED sequence reserved");
}

void StatusLedStrip::start_cold_boot_sequence(std::uint32_t now_ms) {
  const bool started_from_reservation = cold_boot_sequence_.start(now_ms);
  if (!started_from_reservation) {
    deferred_feedback_.clear();
  }
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  cursor_ = 0;
  idle_rendered_ = false;

  ai_keyboard::BootLedFrame frame;
  if (cold_boot_sequence_.take_due_frame(now_ms, &frame)) {
    apply_cold_boot_frame(frame, now_ms);
  }
  ESP_LOGI(kTag, "cold boot LED sequence started");
}

bool StatusLedStrip::cold_boot_sequence_active() const {
  return cold_boot_sequence_.active();
}

void StatusLedStrip::show_raw_color(Rgb color) {
  cold_boot_sequence_.cancel();
  deferred_feedback_.clear();
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  set_all(color);
  if (ready()) {
    flush();
  }
  idle_rendered_ = false;
}

void StatusLedStrip::show_pixel(std::size_t index, Rgb color) {
  cold_boot_sequence_.cancel();
  deferred_feedback_.clear();
  active_feedback_ = {};
  effect_until_ms_ = 0;
  last_frame_ms_ = 0;
  set_all({});
  if (index < leds_.size()) {
    leds_[index] = color;
  }
  if (ready()) {
    flush();
  }
  idle_rendered_ = false;
}

void StatusLedStrip::clear() {
  cold_boot_sequence_.cancel();
  deferred_feedback_.clear();
  active_feedback_ = {};
  agent_status_ = {};
  agent_status_active_ = false;
  agent_status_rendered_ = false;
  agent_status_expires_ms_ = 0;
  set_all({});
  esp_err_t err = ESP_OK;
  if (ready()) {
    err = flush();
  }
  idle_rendered_ = true;
  (void)err;
}

esp_err_t StatusLedStrip::prepare_for_deep_sleep() {
  if (!ready()) {
    return ESP_ERR_INVALID_STATE;
  }
  set_all({});
  const esp_err_t err = flush();
  if (err != ESP_OK) {
    ESP_LOGE(kTag,
             "deep sleep black frame failed: %s",
             esp_err_to_name(err));
    return err;
  }
  // Preserve the logical effect/status state until deep sleep actually
  // starts. If a late blocker cancels shutdown, the next update can render
  // that state again instead of leaving a still-awake device artificially
  // blank.
  idle_rendered_ = false;
  agent_status_rendered_ = false;
  multi_agent_active_ = false;
  return ESP_OK;
}

void StatusLedStrip::set_agent_status(const ai_keyboard::AgentStatusCommand& command,
                                      std::uint32_t now_ms) {
  // CDC 5 灯模式（守护进程持续驱动）优先：官方 App 的单 Agent 命令
  // 在 multi 会话存续期间忽略，避免与 5 灯控制互相覆盖。
  if (multi_agent_valid(now_ms)) {
    return;
  }
  agent_status_ = command;
  if (command.state == ai_keyboard::AgentStatusState::kIdle || command.ttl_ms == 0) {
    agent_status_active_ = false;
    agent_status_rendered_ = false;
    agent_status_expires_ms_ = 0;
  } else {
    agent_status_active_ = true;
    agent_status_expires_ms_ = now_ms + command.ttl_ms;
  }

  idle_rendered_ = false;
  if (!active_feedback_.active && !cold_boot_sequence_.active()) {
    render_background_status(now_ms);
    idle_rendered_ = true;
  }
}

void StatusLedStrip::set_multi_agent_status(
    const std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count>& states,
    std::uint32_t now_ms) {
  // v1.10：状态跃迁检测（守护进程每 2s 重发相同帧，仅状态值变化时重置动画计时）
  for (std::size_t index = 0; index < multi_agent_states_.size(); ++index) {
    const auto next = states[index];
    if (next != multi_agent_states_[index]) {
      multi_agent_prev_states_[index] = multi_agent_states_[index];
      multi_agent_since_ms_[index] = now_ms;
      multi_agent_states_[index] = next;
    }
  }
  multi_agent_active_ = true;
  multi_agent_last_rx_ms_ = now_ms;
  agent_status_active_ = false;
  agent_status_rendered_ = false;
  idle_rendered_ = false;
  // 动画帧立即调度（下一轮 update 即渲染）
  multi_agent_anim_frame_ms_ = now_ms;
  if (!active_feedback_.active && !cold_boot_sequence_.active()) {
    render_multi_agent_status_animated(now_ms);
    flush();
  }
}

void StatusLedStrip::refresh_multi_agent_ttl(std::uint32_t now_ms) {
  if (!multi_agent_active_) {
    return;
  }
  multi_agent_last_rx_ms_ = now_ms;
  // 心跳仅刷新 TTL，不重置动画计时；按帧推进调度。
  multi_agent_anim_frame_ms_ = now_ms;
  if (!idle_rendered_) {
    render_multi_agent_status_animated(now_ms);
    flush();
  }
}

bool StatusLedStrip::multi_agent_active() const {
  return multi_agent_active_;
}

void StatusLedStrip::show_scroll_event(std::int8_t vertical,
                                       std::int8_t horizontal,
                                       std::uint32_t now_ms) {
  if (vertical == 0 && horizontal == 0) {
    return;
  }

  const bool horizontal_scroll = horizontal != 0;
  const auto value = horizontal_scroll ? horizontal : vertical;
  show_feedback({true,
                 ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                 value > 0 ? ai_keyboard::FeedbackDirection::Right
                           : ai_keyboard::FeedbackDirection::Left,
                 horizontal_scroll ? ai_keyboard::FeedbackColor{0, 22, 22}
                                   : ai_keyboard::FeedbackColor{0, 0, 28},
                 160,
                 40},
                now_ms);
}

void StatusLedStrip::show_status_event(StatusLedEvent event, std::uint32_t now_ms) {
  const auto show_status_feedback =
      [&](const ai_keyboard::InputActivityFeedback& feedback) {
        show_feedback(feedback, now_ms, true);
      };
  switch (event) {
    case StatusLedEvent::BleConnected:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::LightBarRipple,
                            ai_keyboard::FeedbackDirection::None,
                            {0, 0, 28},
                            900,
                            180});
      return;
    case StatusLedEvent::BleDisconnected:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::ConfirmPulse,
                            ai_keyboard::FeedbackDirection::None,
                            {4, 4, 8},
                            420,
                            80});
      return;
    case StatusLedEvent::UsbConnected:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                            ai_keyboard::FeedbackDirection::Right,
                            {18, 18, 18},
                            700,
                            90});
      return;
    case StatusLedEvent::UsbDisconnected:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                            ai_keyboard::FeedbackDirection::Left,
                            {10, 10, 10},
                            360,
                            70});
      return;
    case StatusLedEvent::ConfigMode:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::RainbowMarquee,
                            ai_keyboard::FeedbackDirection::None,
                            {0, 0, 0},
                            1200,
                            110});
      return;
    case StatusLedEvent::PlatformMacOS:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                            ai_keyboard::FeedbackDirection::Left,
                            {24, 24, 24},
                            1000,
                            100});
      return;
    case StatusLedEvent::PlatformWindows:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::DirectionalFlow,
                            ai_keyboard::FeedbackDirection::Right,
                            {0, 8, 30},
                            1000,
                            100});
      return;
    case StatusLedEvent::SaveFailed:
      show_status_feedback({true,
                            ai_keyboard::FeedbackEffectKind::ConfirmPulse,
                            ai_keyboard::FeedbackDirection::None,
                            {32, 0, 0},
                            1000,
                            120});
      return;
  }
}

void StatusLedStrip::show_input_event(ai_keyboard::InputId input,
                                      ai_keyboard::InputPhase phase,
                                      std::uint32_t now_ms) {
  const auto feedback = ai_keyboard::feedback_for_input_event(input, phase);
  if (!feedback.active) {
    return;
  }

  show_feedback(feedback, now_ms);
}

void StatusLedStrip::show_feedback(const ai_keyboard::InputActivityFeedback& feedback,
                                   std::uint32_t now_ms,
                                   bool replay_full_duration_after_boot) {
  if (!feedback.active) {
    return;
  }
  if (cold_boot_sequence_.active()) {
    // Cold-boot frames own the physical strip for a nominal 1.77 s visual
    // window. Keep only the newest runtime feedback; input retains its expiry,
    // while semantic connection/config status gets a full replay afterwards.
    deferred_feedback_.defer(feedback,
                             now_ms,
                             replay_full_duration_after_boot);
    return;
  }
  activate_feedback(feedback, now_ms, now_ms + feedback.duration_ms);
}

void StatusLedStrip::activate_feedback(
    const ai_keyboard::InputActivityFeedback& feedback,
    std::uint32_t now_ms,
    std::uint32_t effect_until_ms) {
  active_feedback_ = feedback;
  effect_until_ms_ = effect_until_ms;
  last_frame_ms_ = now_ms;
  cursor_ = 0;
  render_active_effect();
}

bool StatusLedStrip::update_active_feedback(std::uint32_t now_ms) {
  if (active_feedback_.active && now_ms < effect_until_ms_) {
    if (active_feedback_.frame_interval_ms == 0) {
      return true;
    }
    if (now_ms - last_frame_ms_ < active_feedback_.frame_interval_ms) {
      return true;
    }
    last_frame_ms_ = now_ms;
    cursor_ = static_cast<std::uint8_t>((cursor_ + 1U) % ai_keyboard::kWs2812Count);
    render_active_effect();
    return true;
  }
  active_feedback_ = {};
  return false;
}

void StatusLedStrip::apply_cold_boot_frame(
    const ai_keyboard::BootLedFrame& frame,
    std::uint32_t now_ms) {
  switch (frame.kind) {
    case ai_keyboard::BootLedFrameKind::Pixel:
      active_feedback_ = {};
      effect_until_ms_ = 0;
      last_frame_ms_ = 0;
      set_all({});
      if (frame.pixel_index < leds_.size()) {
        leds_[frame.pixel_index] = kColdBootProbeColors[frame.pixel_index];
      }
      if (ready()) {
        flush();
      }
      idle_rendered_ = false;
      ESP_LOGI(kTag,
               "cold boot LED pixel=%u GPIO%u",
               static_cast<unsigned>(frame.pixel_index),
               static_cast<unsigned>(ai_keyboard::kWs2812Pin));
      return;
    case ai_keyboard::BootLedFrameKind::AllPixels:
      active_feedback_ = {};
      effect_until_ms_ = 0;
      last_frame_ms_ = 0;
      set_all({18, 18, 18});
      if (ready()) {
        flush();
      }
      idle_rendered_ = false;
      return;
    case ai_keyboard::BootLedFrameKind::Rainbow:
      // The semantic Boot effect is not ConfigMode. It keeps animating for its
      // full 1.2 s unless newer still-valid runtime feedback takes over after
      // the historical 450 ms Boot ownership window.
      activate_feedback({true,
                         ai_keyboard::FeedbackEffectKind::RainbowMarquee,
                         ai_keyboard::FeedbackDirection::None,
                         {0, 0, 0},
                         1200,
                         110},
                        now_ms,
                        now_ms + 1200U);
      return;
    case ai_keyboard::BootLedFrameKind::Complete:
      release_cold_boot_visual_ownership(now_ms);
      ESP_LOGI(kTag, "cold boot LED self-test complete");
      return;
  }
}

void StatusLedStrip::release_cold_boot_visual_ownership(
    std::uint32_t now_ms) {
  ai_keyboard::BootLedDeferredPlayback playback;
  if (deferred_feedback_.take(now_ms, &playback)) {
    activate_feedback(playback.feedback,
                      now_ms,
                      playback.effect_until_ms);
  }
}

void StatusLedStrip::update(std::uint32_t now_ms) {
  if (agent_status_active_ && !agent_status_valid(now_ms)) {
    agent_status_active_ = false;
    agent_status_rendered_ = false;
    idle_rendered_ = false;
  }
  if (multi_agent_active_ && !multi_agent_valid(now_ms)) {
    // 守护进程失联：整体退出 5 灯模式并回落到 single/idle 渲染。
    multi_agent_active_ = false;
    idle_rendered_ = false;
  }

  ai_keyboard::BootLedFrame boot_frame;
  if (cold_boot_sequence_.take_due_frame(now_ms, &boot_frame)) {
    apply_cold_boot_frame(boot_frame, now_ms);
  }
  if (cold_boot_sequence_.active()) {
    // Pixel/all frames are static; Rainbow uses the existing frame renderer.
    // In both cases one bounded update is enough for this owner pass.
    (void)update_active_feedback(now_ms);
    return;
  }

  if (update_active_feedback(now_ms)) {
    return;
  }
  // v1.10：multi_agent 动画按帧渲染（快闪/呼吸/慢闪/超时周期）
  if (multi_agent_active_ && multi_agent_valid(now_ms)) {
    render_multi_agent_status_animated(now_ms);
    flush();
    multi_agent_anim_frame_ms_ = now_ms + kMultiAgentAnimFrameMs;
    idle_rendered_ = true;
    return;
  }
  if (!idle_rendered_) {
    render_background_status(now_ms);
    idle_rendered_ = true;
  }
}

bool StatusLedStrip::next_update_deadline_ms(
    std::uint32_t now_ms,
    std::uint32_t* deadline_ms) const {
  if (deadline_ms == nullptr) {
    return false;
  }
  bool armed = false;
  std::uint32_t nearest = 0;
  const auto add = [&](bool available, std::uint32_t candidate) {
    if (!available) {
      return;
    }
    if (!armed || static_cast<std::int32_t>(candidate - nearest) < 0) {
      nearest = candidate;
      armed = true;
    }
  };

  std::uint32_t cold_boot_deadline_ms = 0;
  const bool cold_boot_deadline =
      cold_boot_sequence_.next_deadline_ms(&cold_boot_deadline_ms);
  add(cold_boot_deadline, cold_boot_deadline_ms);

  if (active_feedback_.active) {
    add(true, effect_until_ms_);
    if (active_feedback_.frame_interval_ms != 0) {
      add(true, last_frame_ms_ + active_feedback_.frame_interval_ms);
    }
  }
  add(agent_status_active_, agent_status_expires_ms_);
  // v1.10：multi_agent 动画帧调度（快闪/呼吸/慢闪需要周期性刷新）
  if (multi_agent_active_ && multi_agent_valid(now_ms)) {
    add(true, multi_agent_anim_frame_ms_);
    add(true, multi_agent_last_rx_ms_ + kMultiAgentTimeoutMs);
  } else {
    add(multi_agent_active_, multi_agent_last_rx_ms_ + kMultiAgentTimeoutMs);
  }
  if (!idle_rendered_ && !active_feedback_.active &&
      !cold_boot_sequence_.active()) {
    add(true, now_ms);
  }
  if (armed) {
    *deadline_ms = nearest;
  }
  return armed;
}

bool StatusLedStrip::agent_status_valid(std::uint32_t now_ms) const {
  return agent_status_active_ && deadline_pending(now_ms, agent_status_expires_ms_);
}

bool StatusLedStrip::multi_agent_valid(std::uint32_t now_ms) const {
  return multi_agent_active_ &&
         deadline_pending(now_ms, multi_agent_last_rx_ms_ + kMultiAgentTimeoutMs);
}

void StatusLedStrip::render_background_status(std::uint32_t now_ms) {
  if (multi_agent_valid(now_ms)) {
    render_multi_agent_status_animated(now_ms);
    flush();
    return;
  }
  if (agent_status_valid(now_ms)) {
    render_agent_status();
    flush();
    agent_status_rendered_ = true;
    return;
  }

  agent_status_rendered_ = false;
  render_idle_status();
}

void StatusLedStrip::render_agent_status() {
  set_all({});
  if (leds_.empty()) {
    return;
  }

  const auto color = agent_status_color(agent_status_.state);
  const auto center = leds_.size() / 2U;
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const auto distance = index > center ? index - center : center - index;
    if (distance == 0U) {
      leds_[index] = color;
    } else if (distance == 1U) {
      leds_[index] = scale_rgb(color, 1, 2);
    } else {
      leds_[index] = scale_rgb(color, 1, 4);
    }
  }
}

void StatusLedStrip::render_multi_agent_status() {
  // 保持静态版本（兼容内部其他路径），动画逻辑见 *_animated。
  set_all({});
  if (leds_.empty()) {
    return;
  }
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    if (index < multi_agent_states_.size()) {
      leds_[index] = agent_status_color(multi_agent_states_[index]);
    }
  }
}

// v1.10 动画渲染：每颗灯按状态机渲染（快闪提醒/慢呼吸/慢闪/静态绿自灭/快闪），
// 并结合占空比超时周期（方案 B：闪一段时间后静默，省寿命 + 不漏提醒）。
void StatusLedStrip::render_multi_agent_status_animated(std::uint32_t now_ms) {
  set_all({});
  if (leds_.empty()) {
    return;
  }
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    if (index >= multi_agent_states_.size()) {
      continue;
    }
    const auto state = multi_agent_states_[index];
    const std::uint32_t since = multi_agent_since_ms_[index];
    const std::uint32_t elapsed = now_ms - since;
    Rgb color = {};
    switch (state) {
      case ai_keyboard::AgentStatusState::kIdle:
        color = {};
        break;
      case ai_keyboard::AgentStatusState::kRunning: {
        const Rgb blue = agent_status_color(state);
        // 开始工作提醒：蓝快闪 N 次 → 之后进入慢呼吸
        const std::uint32_t flash_total =
            static_cast<std::uint32_t>(kWorkFlashCount) *
            (kWorkFlashOnMs + kWorkFlashOffMs);
        if (elapsed < flash_total) {
          const std::uint32_t t = elapsed % (kWorkFlashOnMs + kWorkFlashOffMs);
          color = t < kWorkFlashOnMs ? blue : Rgb{};
        } else if (elapsed >= kWorkDimAfterMs) {
          // 工作持续 5 分钟后降为低亮呼吸（省寿命/省电，仍可感知"在忙"）
          color = dim_breathe_rgb(blue, now_ms);
        } else {
          color = breathe_rgb(blue, now_ms);
        }
        break;
      }
      case ai_keyboard::AgentStatusState::kWaitingUser: {
        // 慢闪（亮1s灭1s），占空比周期：闪 1min → 静默 4min → 循环
        if (!duty_cycle_active(elapsed, 0, kWaitFlashMs, kWaitQuietMs)) {
          color = {};
        } else {
          const std::uint32_t t = elapsed % (kWaitOnMs + kWaitOffMs);
          color = t < kWaitOnMs ? agent_status_color(state) : Rgb{};
        }
        break;
      }
      case ai_keyboard::AgentStatusState::kCompletedUnread: {
        // v1.11 完成：绿闪 30s（快完成）→ 绿常亮 30s（完成确认）→ 自动熄灭
        // 让用户看到绿灯闪 = 快完成，绿常亮 = 已完成，然后灭
        if (elapsed <= kDoneFlashMs) {
          const std::uint32_t t = elapsed % (kDoneOnMs + kDoneOffMs);
          color = t < kDoneOnMs ? agent_status_color(state) : Rgb{};
        } else if (elapsed <= kDoneFlashMs + kDoneSolidMs) {
          color = agent_status_color(state);
        } else {
          color = Rgb{};
        }
        break;
      }
      case ai_keyboard::AgentStatusState::kFailed: {
        // 快闪，占空比周期：闪 30s → 静默 4.5min → 循环
        if (!duty_cycle_active(elapsed, 0, kErrFlashMs, kErrQuietMs)) {
          color = {};
        } else {
          const std::uint32_t t = elapsed % (kErrOnMs + kErrOffMs);
          color = t < kErrOnMs ? agent_status_color(state) : Rgb{};
        }
        break;
      }
      default:
        color = {};
        break;
    }
    leds_[index] = color;
  }
}

void StatusLedStrip::render_idle_status() {
  set_all({});
  flush();
}

void StatusLedStrip::set_all(Rgb color) {
  for (auto& led : leds_) {
    led = color;
  }
}

void StatusLedStrip::render_active_effect() {
  agent_status_rendered_ = false;
  switch (active_feedback_.effect) {
    case ai_keyboard::FeedbackEffectKind::Solid:
      render_solid_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::LightBarRipple:
      render_light_bar_ripple_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::DirectionalFlow:
      render_directional_flow_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::ConfirmPulse:
      render_confirm_pulse_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::RainbowMarquee:
      render_rainbow_marquee_effect();
      break;
    case ai_keyboard::FeedbackEffectKind::None:
      set_all({});
      break;
  }
  flush();
  idle_rendered_ = false;
}

void StatusLedStrip::render_solid_effect() {
  set_all(to_rgb(active_feedback_.color));
}

void StatusLedStrip::render_light_bar_ripple_effect() {
  const auto base = to_rgb(active_feedback_.color);
  set_all({});

  switch (cursor_ % 4U) {
    case 0:
      set_scaled_pixel(&leds_, 1, base, 1, 5);
      set_scaled_pixel(&leds_, 2, base, 1, 1);
      set_scaled_pixel(&leds_, 3, base, 1, 5);
      break;
    case 1:
      set_scaled_pixel(&leds_, 0, base, 1, 4);
      set_scaled_pixel(&leds_, 1, base, 1, 1);
      set_scaled_pixel(&leds_, 2, base, 1, 2);
      set_scaled_pixel(&leds_, 3, base, 1, 1);
      set_scaled_pixel(&leds_, 4, base, 1, 4);
      break;
    case 2:
      set_scaled_pixel(&leds_, 0, base, 1, 1);
      set_scaled_pixel(&leds_, 1, base, 1, 4);
      set_scaled_pixel(&leds_, 3, base, 1, 4);
      set_scaled_pixel(&leds_, 4, base, 1, 1);
      break;
    default:
      set_scaled_pixel(&leds_, 0, base, 1, 4);
      set_scaled_pixel(&leds_, 1, base, 1, 6);
      set_scaled_pixel(&leds_, 2, base, 1, 8);
      set_scaled_pixel(&leds_, 3, base, 1, 6);
      set_scaled_pixel(&leds_, 4, base, 1, 4);
      break;
  }
}

void StatusLedStrip::render_directional_flow_effect() {
  const auto count = static_cast<std::uint8_t>(ai_keyboard::kWs2812Count);
  const auto base = to_rgb(active_feedback_.color);
  const bool right = active_feedback_.direction == ai_keyboard::FeedbackDirection::Right;
  const auto offset = static_cast<std::uint8_t>(cursor_ % count);
  const auto head = right ? offset : static_cast<std::uint8_t>((count - 1U) - offset);
  constexpr std::array<std::uint8_t, ai_keyboard::kWs2812Count> kNumerators{{12, 7, 4, 2, 1}};

  for (std::uint8_t index = 0; index < count; ++index) {
    const auto distance = right
                              ? static_cast<std::uint8_t>((head + count - index) % count)
                              : static_cast<std::uint8_t>((index + count - head) % count);
    leds_[index] = scale_rgb(base, kNumerators[distance], 12);
  }
}

void StatusLedStrip::render_confirm_pulse_effect() {
  const auto base = to_rgb(active_feedback_.color);
  constexpr std::array<std::uint8_t, 5> kNumerators{{5, 12, 7, 3, 1}};
  const auto numerator = kNumerators[cursor_ % kNumerators.size()];
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const bool center = index == leds_.size() / 2;
    const auto pixel_numerator = center ? numerator : static_cast<std::uint8_t>((numerator * 2) / 3);
    leds_[index] = scale_rgb(base, pixel_numerator, 12);
  }
}

void StatusLedStrip::render_rainbow_marquee_effect() {
  for (std::size_t index = 0; index < ai_keyboard::kWs2812Count; ++index) {
    leds_[index] = rainbow_color(static_cast<std::uint8_t>(index + cursor_));
  }
}

esp_err_t StatusLedStrip::flush() {
  if (!ready()) {
    return ESP_ERR_INVALID_STATE;
  }

  std::array<rmt_symbol_word_t, kWs2812SymbolCount> symbols = {};
  for (std::size_t index = 0; index < leds_.size(); ++index) {
    const auto& color = leds_[index];
    auto* pixel = &symbols[index * 24U];
    encode_ws2812_byte(color.green, pixel);
    encode_ws2812_byte(color.red, pixel + 8U);
    encode_ws2812_byte(color.blue, pixel + 16U);
  }

  auto& reset = symbols[ai_keyboard::kWs2812Count * 24U];
  reset.level0 = 0;
  reset.duration0 = kResetTicks;
  reset.level1 = 0;
  reset.duration1 = kResetTicks;

  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;

  const esp_err_t enable_err = rmt_enable(rmt_channel_);
  if (enable_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_enable GPIO%u failed: %s",
             static_cast<unsigned>(ai_keyboard::kWs2812Pin),
             esp_err_to_name(enable_err));
    return enable_err;
  }

  const esp_err_t transmit_err =
      rmt_transmit(rmt_channel_,
                   rmt_copy_encoder_,
                   symbols.data(),
                   symbols.size() * sizeof(symbols[0]),
                   &tx_config);
  if (transmit_err != ESP_OK) {
    ESP_LOGE(kTag, "transmit failed: %s", esp_err_to_name(transmit_err));
    const esp_err_t disable_err = rmt_disable(rmt_channel_);
    if (disable_err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_disable after transmit failure: %s",
               esp_err_to_name(disable_err));
    }
    return transmit_err;
  }

  const esp_err_t wait_err = rmt_tx_wait_all_done(rmt_channel_, kRmtFlushWaitMs);
  if (wait_err != ESP_OK) {
    ESP_LOGW(kTag, "wait tx done timed out/dropped frame: %s",
             esp_err_to_name(wait_err));
  }

  // rmt_enable() acquires a PM lock. The LED frame is complete (or has been
  // abandoned after timeout), so release it before returning to the idle loop.
  const esp_err_t disable_err = rmt_disable(rmt_channel_);
  if (disable_err != ESP_OK) {
    ESP_LOGE(kTag, "rmt_disable after frame: %s", esp_err_to_name(disable_err));
  }
  return wait_err != ESP_OK ? wait_err : disable_err;
}

}  // namespace easy_input
