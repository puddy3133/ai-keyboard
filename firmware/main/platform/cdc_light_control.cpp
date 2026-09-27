#include "platform/cdc_light_control.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"

namespace easy_input {
namespace {

const char* const kTag = "cdc_light";

std::uint32_t millis() {
  return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

}  // namespace

CdcLightControl* g_light_frame_sink = nullptr;

bool CdcLightControl::parse_frame(const std::uint8_t* data,
                                  std::size_t len,
                                  Frame* out) {
  if (data == nullptr || out == nullptr || len < kFrameLen) {
    return false;
  }
  if (data[0] != kFrameMagic || data[1] != kFrameVersion) {
    return false;
  }
  Frame frame;
  for (std::size_t index = 0; index < frame.states.size(); ++index) {
    const std::uint8_t state = data[2 + index];
    // 未知状态一律按 idle（灭灯）处理，向前兼容后续协议扩展。
    if (state > static_cast<std::uint8_t>(ai_keyboard::AgentStatusState::kFailed)) {
      frame.states[index] = ai_keyboard::AgentStatusState::kIdle;
    } else {
      frame.states[index] = static_cast<ai_keyboard::AgentStatusState>(state);
    }
  }
  frame.heartbeat = (data[7] & kFlagHeartbeat) != 0;
  *out = frame;
  return true;
}

esp_err_t CdcLightControl::begin(StatusLedStrip* leds) {
  if (leds_ != nullptr) {
    return ESP_OK;
  }
  if (leds == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  leds_ = leds;
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) {
    leds_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  const BaseType_t created =
      xTaskCreate(task_entry, "cdc_light", 4096, this, 4, &task_);
  if (created != pdPASS) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
    leds_ = nullptr;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag, "CDC light task started (8-byte frame, magic 0x16)");
  return ESP_OK;
}

bool CdcLightControl::submit_frame(const std::uint8_t* data,
                                   std::size_t len) {
  if (mutex_ == nullptr) {
    return false;
  }
  Frame frame;
  if (!parse_frame(data, len, &frame)) {
    return false;
  }
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    for (std::size_t index = 0; index < pending_states_.size(); ++index) {
      pending_states_[index] = frame.states[index];
    }
    pending_heartbeat_ = frame.heartbeat;
    pending_valid_ = true;
    xSemaphoreGive(mutex_);
    return true;
  }
  return false;
}

void CdcLightControl::apply_pending(std::uint32_t now_ms) {
  if (leds_ == nullptr || mutex_ == nullptr) {
    return;
  }
  Frame frame;
  bool valid = false;
  if (xSemaphoreTake(mutex_, 0) == pdTRUE) {
    valid = pending_valid_;
    if (valid) {
      for (std::size_t index = 0; index < frame.states.size(); ++index) {
        frame.states[index] = pending_states_[index];
      }
      frame.heartbeat = pending_heartbeat_;
      pending_valid_ = false;
      pending_heartbeat_ = false;
    }
    xSemaphoreGive(mutex_);
  }
  if (!valid) {
    return;
  }
  if (frame.heartbeat) {
    leds_->refresh_multi_agent_ttl(now_ms);
  } else {
    leds_->set_multi_agent_status(frame.states, now_ms);
  }
}

void CdcLightControl::task_entry(void* arg) {
  auto* self = static_cast<CdcLightControl*>(arg);
  if (self != nullptr) {
    self->run();
  }
  vTaskDelete(nullptr);
}

void CdcLightControl::run() {
  std::uint8_t buffer[kFrameLen];
  while (true) {
    if (tud_cdc_available() >= kFrameLen) {
      const std::size_t received = tud_cdc_read(buffer, sizeof(buffer));
      if (received >= kFrameLen) {
        Frame frame;
        if (parse_frame(buffer, received, &frame)) {
          if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
            // 最新状态优先；主循环每轮消费 pending，正常 10s 心跳间隔下
            // 不会发生覆盖丢失。
            for (std::size_t index = 0; index < pending_states_.size();
                 ++index) {
              pending_states_[index] = frame.states[index];
            }
            pending_heartbeat_ = frame.heartbeat;
            pending_valid_ = true;
            xSemaphoreGive(mutex_);
          }
        } else {
          ESP_LOGW(kTag, "dropped %u bytes: invalid frame header",
                   static_cast<unsigned>(received));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace easy_input
