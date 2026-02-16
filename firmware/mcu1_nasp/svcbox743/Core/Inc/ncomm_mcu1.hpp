#pragma once

#include <cstdint>

struct UART_HandleTypeDef;

namespace ncomm::mcu1 {

// MVP config (can be overridden later via SET_CONFIG)
struct Config {
  uint16_t vad_positive_required = 3;   // N подряд
  uint16_t audio_samples_per_chunk = 256; // 16ms @ 16kHz
};

// init/loop are called from main.c
void Init(UART_HandleTypeDef* huart);
void Loop();

} // namespace ncomm::mcu1
