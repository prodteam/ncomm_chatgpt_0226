#include "audio_i2s_out.hpp"
#include <cstring>

// ---------- Config ----------
static constexpr uint32_t SAMPLE_RATE_HZ = 16000;

// 10 ms chunk => 160 samples mono
static constexpr uint32_t CHUNK_SAMPLES = 160;

// I2S dataformat = 32-bit per channel, stereo => 2x 32-bit words per mono sample
static constexpr uint32_t WORDS_PER_MONO_SAMPLE = 2; // L and R
static constexpr uint32_t TX_WORDS_PER_CHUNK = CHUNK_SAMPLES * WORDS_PER_MONO_SAMPLE;

// FIFO depth: 320 ms ~= 5120 samples @16k (safe jitter)
static constexpr uint32_t FIFO_SAMPLES = 8192; // power of 2 recommended
static_assert((FIFO_SAMPLES & (FIFO_SAMPLES - 1)) == 0, "FIFO_SAMPLES must be power of 2");

// ---------- State ----------
static I2S_HandleTypeDef* g_hi2s = nullptr;

// FIFO of mono int16 samples
static int16_t fifo_[FIFO_SAMPLES];
static volatile uint32_t wpos_ = 0;
static volatile uint32_t rpos_ = 0;

// ping-pong buffers in uint32 (I2S writes 32-bit words)
static uint32_t tx_buf_a_[TX_WORDS_PER_CHUNK];
static uint32_t tx_buf_b_[TX_WORDS_PER_CHUNK];
static uint32_t* cur_tx_ = tx_buf_a_;
static uint32_t* next_tx_ = tx_buf_b_;
static volatile bool tx_active_ = false;

static audio_i2s_out::Stats st_{};

static inline uint32_t fifo_level_unsafe_() {
  return (wpos_ - rpos_) & (FIFO_SAMPLES - 1);
}

static inline void fifo_push_one_(int16_t s) {
  const uint32_t next = (wpos_ + 1) & (FIFO_SAMPLES - 1);
  if (next == rpos_) {
    // overflow: drop newest
    st_.overflow++;
    return;
  }
  fifo_[wpos_] = s;
  wpos_ = next;
}

static inline bool fifo_pop_one_(int16_t& out) {
  if (rpos_ == wpos_) return false;
  out = fifo_[rpos_];
  rpos_ = (rpos_ + 1) & (FIFO_SAMPLES - 1);
  return true;
}

// Convert int16 sample to 32-bit I2S word.
// We left-shift by 16 so sample lands in MS16 bits (common for 16-bit audio over 32-bit slot).
static inline uint32_t pcm16_to_i2s32_(int16_t s) {
  return ((uint32_t)(uint16_t)s) << 16; // keep sign in top bits when reinterpreted by DAC
}

static void fill_tx_words_(uint32_t* dst_words, uint32_t words_count) {
  // words_count should equal TX_WORDS_PER_CHUNK
  // For each mono sample: write L then R (same)
  const uint32_t frames = words_count / 2;

  for (uint32_t i = 0; i < frames; i++) {
    int16_t s = 0;
    if (!fifo_pop_one_(s)) {
      // underrun => output silence
      st_.underrun++;
      s = 0;
    }
    const uint32_t w = pcm16_to_i2s32_(s);
    dst_words[i * 2 + 0] = w; // L
    dst_words[i * 2 + 1] = w; // R
  }

  st_.fifo_samples = fifo_level_unsafe_();
}

namespace audio_i2s_out {

void init(I2S_HandleTypeDef* hi2s) {
  g_hi2s = hi2s;
  wpos_ = rpos_ = 0;
  tx_active_ = false;
  st_ = {};

  // prefill both buffers with silence (or FIFO content if already)
  fill_tx_words_(tx_buf_a_, TX_WORDS_PER_CHUNK);
  fill_tx_words_(tx_buf_b_, TX_WORDS_PER_CHUNK);

  cur_tx_ = tx_buf_a_;
  next_tx_ = tx_buf_b_;
}

void start() {
  if (!g_hi2s) return;
  if (tx_active_) return;

  // Start IT transfer of first chunk (Size = number of 32-bit words)
  // HAL expects pData as uint16_t*, but internally reads 32-bit words when DataFormat=32B.
  const HAL_StatusTypeDef rc =
      HAL_I2S_Transmit_IT(g_hi2s, reinterpret_cast<uint16_t*>(cur_tx_), (uint16_t)TX_WORDS_PER_CHUNK);

  tx_active_ = (rc == HAL_OK);
}

void stop() {
  if (!g_hi2s) return;
  (void)HAL_I2S_DMAStop(g_hi2s); // safe even if not using DMA (HAL returns error, ignore)
  tx_active_ = false;
}

void push_pcm16_mono(const int16_t* samples, size_t n) {
  if (!samples || n == 0) return;

  // Push into FIFO (best effort)
  for (size_t i = 0; i < n; i++) {
    fifo_push_one_(samples[i]);
  }

  st_.fifo_samples = fifo_level_unsafe_();
}

const Stats& stats() { return st_; }

void on_i2s_tx_cplt(I2S_HandleTypeDef* hi2s) {
  if (!g_hi2s || hi2s != g_hi2s) return;
  st_.i2s_cb++;

  // prepare next buffer while current finished
  fill_tx_words_(next_tx_, TX_WORDS_PER_CHUNK);

  // swap buffers
  uint32_t* to_send = next_tx_;
  next_tx_ = cur_tx_;
  cur_tx_ = to_send;

  // restart next transfer
  (void)HAL_I2S_Transmit_IT(g_hi2s, reinterpret_cast<uint16_t*>(cur_tx_), (uint16_t)TX_WORDS_PER_CHUNK);
}

} // namespace audio_i2s_out
