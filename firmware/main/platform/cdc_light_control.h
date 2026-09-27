#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "keyboard/agent_status.h"
#include "keyboard/board_pins.h"
#include "platform/led_strip_status.h"

namespace easy_input {

// v1.6 灯控 CDC 通道（8 字节定长帧）：
//   byte0 = magic 0x16
//   byte1 = version 0x01
//   byte2..6 = state_L1..L5（0=idle/灭, 1=running, 2=waiting, 3=done, 4=failed, 5=off）
//   byte7 = flags（bit0=1 表示纯心跳，仅刷新 TTL 不改状态）
// 固件 30s 无帧整体退出 5 灯模式（StatusLedStrip 内部处理）。
//
// v1.9 BLE 多灯通道：类名保留 CdcLightControl（避免大改），但帧提交统一走
// submit_frame()，CDC 任务与 BLE 写回调共用同一 pending + 主循环消费机制。
// app_main 在 begin() 成功后把实例挂到全局 g_light_frame_sink，BLE 层经此提交。
class CdcLightControl {
 public:
  esp_err_t begin(StatusLedStrip* leds);
  // 主循环（platform_task）调用：取走 pending 帧并应用到 LED 控制器，
  // 保证所有 LED 状态变更都发生在主循环上下文（线程安全）。
  void apply_pending(std::uint32_t now_ms);
  // 任意通道（CDC 任务 / BLE 回调）提交一帧 8 字节 0x16 帧。
  // 内部加锁写入 pending；主循环下一轮 apply_pending 消费。返回是否解析并接受。
  bool submit_frame(const std::uint8_t* data, std::size_t len);
  bool running() const { return task_ != nullptr; }

 private:
  static constexpr std::uint8_t kFrameMagic = 0x16;
  static constexpr std::uint8_t kFrameVersion = 0x01;
  static constexpr std::size_t kFrameLen = 8;
  static constexpr std::uint8_t kFlagHeartbeat = 0x01;

  struct Frame {
    std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count> states = {};
    bool heartbeat = false;
  };

  static bool parse_frame(const std::uint8_t* data,
                          std::size_t len,
                          Frame* out);
  static void task_entry(void* arg);
  void run();

  StatusLedStrip* leds_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  std::array<ai_keyboard::AgentStatusState, ai_keyboard::kWs2812Count>
      pending_states_ = {};
  bool pending_heartbeat_ = false;
  bool pending_valid_ = false;
};

// 全局帧提交入口：供 BLE 层（NimBLE 回调上下文）提交多灯状态帧。
// begin() 成功后由 app_main 赋值为实际实例；为空时 BLE 侧安全忽略。
extern CdcLightControl* g_light_frame_sink;

}  // namespace easy_input
