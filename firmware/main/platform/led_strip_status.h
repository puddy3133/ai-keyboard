#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "keyboard/agent_status.h"
#include "keyboard/board_pins.h"
#include "keyboard/boot_led_sequence.h"
#include "keyboard/input_feedback.h"
#include "keyboard/keymap.h"

namespace easy_input {

struct Rgb {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

enum class StatusLedEvent {
  BleConnected,
  BleDisconnected,
  UsbConnected,
  UsbDisconnected,
  ConfigMode,
  PlatformMacOS,
  PlatformWindows,
  SaveFailed,
};

class StatusLedStrip {
 public:
  esp_err_t begin();
  bool ready() const;

  void clear();
  esp_err_t prepare_for_deep_sleep();
  void reserve_cold_boot_sequence();
  void start_cold_boot_sequence(std::uint32_t now_ms);
  bool cold_boot_sequence_active() const;
  void show_raw_color(Rgb color);
  void show_pixel(std::size_t index, Rgb color);
  void set_agent_status(const ai_keyboard::AgentStatusCommand& command,
                        std::uint32_t now_ms);
  // v1.6 灯控：CDC 5 灯独立模式。states[i] 对应第 i 颗灯（0..4）的状态。
  // 任一灯状态为 kIdle 时该灯熄灭。30 秒无任何帧（含心跳）整体退出 multi
  // 模式并回落到 single/idle 渲染。
  void set_multi_agent_status(
      const std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count>& states,
      std::uint32_t now_ms);
  void refresh_multi_agent_ttl(std::uint32_t now_ms);
  bool multi_agent_active() const;
  void show_scroll_event(std::int8_t vertical,
                         std::int8_t horizontal,
                         std::uint32_t now_ms);
  void show_status_event(StatusLedEvent event, std::uint32_t now_ms);
  void show_input_event(ai_keyboard::InputId input,
                        ai_keyboard::InputPhase phase,
                        std::uint32_t now_ms);
  void update(std::uint32_t now_ms);
  bool next_update_deadline_ms(std::uint32_t now_ms,
                               std::uint32_t* deadline_ms) const;

 private:
  void show_feedback(const ai_keyboard::InputActivityFeedback& feedback,
                     std::uint32_t now_ms,
                     bool replay_full_duration_after_boot = false);
  void activate_feedback(const ai_keyboard::InputActivityFeedback& feedback,
                         std::uint32_t now_ms,
                         std::uint32_t effect_until_ms);
  bool update_active_feedback(std::uint32_t now_ms);
  void apply_cold_boot_frame(const ai_keyboard::BootLedFrame& frame,
                             std::uint32_t now_ms);
  void release_cold_boot_visual_ownership(std::uint32_t now_ms);
  bool agent_status_valid(std::uint32_t now_ms) const;
  bool multi_agent_valid(std::uint32_t now_ms) const;
  void render_background_status(std::uint32_t now_ms);
  void render_agent_status();
  void render_multi_agent_status();
  void render_multi_agent_status_animated(std::uint32_t now_ms);
  void render_idle_status();
  void set_all(Rgb color);
  void render_active_effect();
  void render_solid_effect();
  void render_light_bar_ripple_effect();
  void render_directional_flow_effect();
  void render_confirm_pulse_effect();
  void render_rainbow_marquee_effect();
  esp_err_t flush();

  rmt_channel_handle_t rmt_channel_ = nullptr;
  rmt_encoder_handle_t rmt_copy_encoder_ = nullptr;
  std::array<Rgb, ai_keyboard::kWs2812Count> leds_ = {};
  ai_keyboard::InputActivityFeedback active_feedback_;
  std::uint32_t effect_until_ms_ = 0;
  std::uint32_t last_frame_ms_ = 0;
  std::uint8_t cursor_ = 0;
  ai_keyboard::BootLedSequence cold_boot_sequence_;
  ai_keyboard::BootLedDeferredFeedback deferred_feedback_;
  ai_keyboard::AgentStatusCommand agent_status_;
  std::uint32_t agent_status_expires_ms_ = 0;
  bool agent_status_active_ = false;
  bool agent_status_rendered_ = false;
  // v1.6 CDC 5 灯独立状态
  std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count> multi_agent_states_ = {};
  std::uint32_t multi_agent_last_rx_ms_ = 0;
  bool multi_agent_active_ = false;
  bool idle_rendered_ = false;
  // v1.10 动画：每灯状态跃迁检测 + 状态起始时刻 + 动画帧调度
  std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count> multi_agent_prev_states_ = {};
  std::array<std::uint32_t, ai_keyboard::kWs2812Count> multi_agent_since_ms_ = {};
  std::uint32_t multi_agent_anim_frame_ms_ = 0;
};

}  // namespace easy_input
