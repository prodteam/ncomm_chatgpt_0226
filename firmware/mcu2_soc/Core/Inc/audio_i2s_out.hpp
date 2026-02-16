#pragma once
#include <cstddef>
#include <cstdint>

#include "i2s.h"

namespace audio_i2s_out {

// 16 kHz mono PCM16 -> I2S2 32-bit stereo (L=R)
// IT ping-pong buffers + FIFO.
void init(I2S_HandleTypeDef* hi2s);
void start();
void stop();

// push PCM16 mono samples (signed little-endian in memory)
void push_pcm16_mono(const int16_t* samples, size_t n);

// stats for logs
struct Stats {
  uint32_t fifo_samples = 0;
  uint32_t underrun = 0;
  uint32_t overflow = 0;
  uint32_t i2s_cb = 0;
};
const Stats& stats();

void on_i2s_tx_cplt(I2S_HandleTypeDef* hi2s);

} // namespace audio_i2s_out
