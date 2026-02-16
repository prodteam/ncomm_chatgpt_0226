#include "main.h"
#include "dma.h"
#include "gpio.h"
#include "usart.h"
#include "i2s.h"

#include "ncomm_mcu2.hpp"
#include "audio_i2s_out.hpp"

#include <cstring>

/* USER CODE BEGIN PV */
static NcommMcu2 g_mcu2;
static uint8_t g_ui_rx_byte = 0;
/* USER CODE END PV */

void SystemClock_Config(void);

/* ---------- UART4 log helpers ---------- */
static void uart4_write_buf(const uint8_t* data, uint16_t len) {
  if (!data || !len) return;
  (void)HAL_UART_Transmit(&huart4, const_cast<uint8_t*>(data), len, 50);
}

static void uart4_write_str(const char* s) {
  if (!s) return;
  uart4_write_buf(reinterpret_cast<const uint8_t*>(s), (uint16_t)strlen(s));
}

static char* u32_to_dec(char* out, uint32_t v) {
  char tmp[11]; int n = 0;
  if (v == 0) { *out++ = '0'; *out = 0; return out; }
  while (v && n < 10) { tmp[n++] = char('0' + (v % 10)); v /= 10; }
  while (n--) { *out++ = tmp[n]; }
  *out = 0;
  return out;
}

static void log_mcu2_stats_1s() {
  static uint32_t last_ms = 0;
  const uint32_t now = HAL_GetTick();
  if ((now - last_ms) < 1000) return;
  last_ms = now;

  const auto& st = g_mcu2.stats();
  const auto& ao = audio_i2s_out::stats();

  char line[280];
  char* p = line;

  memcpy(p, "t=", 2); p += 2; p = u32_to_dec(p, now);

  memcpy(p, " rxB=", 5); p += 5; p = u32_to_dec(p, st.rx_bytes);
  memcpy(p, " ok=", 4); p += 4; p = u32_to_dec(p, st.rx_frames_ok);
  memcpy(p, " crcBad=", 8); p += 8; p = u32_to_dec(p, st.rx_frames_bad_crc);

  memcpy(p, " tx=", 4); p += 4; p = u32_to_dec(p, st.tx_frames);
  memcpy(p, " pong=", 6); p += 6; p = u32_to_dec(p, st.pong);
  memcpy(p, " vad=", 5); p += 5; p = u32_to_dec(p, st.vad);
  memcpy(p, " aRx=", 5); p += 5; p = u32_to_dec(p, st.audio_rx);
  memcpy(p, " aTx=", 5); p += 5; p = u32_to_dec(p, st.audio_tx);

  memcpy(p, " fifo=", 6); p += 6; p = u32_to_dec(p, ao.fifo_samples);
  memcpy(p, " und=", 5); p += 5; p = u32_to_dec(p, ao.underrun);
  memcpy(p, " ovf=", 5); p += 5; p = u32_to_dec(p, ao.overflow);

  memcpy(p, "\r\n", 2); p += 2; *p = 0;

  uart4_write_str(line);
}

static void send_ping_every_2s() {
  static uint32_t last_ms = 0;
  const uint32_t now = HAL_GetTick();
  if ((now - last_ms) < 2000) return;
  last_ms = now;
  g_mcu2.send_ping();
}

/* ---------- Callbacks ---------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart3)
  {
    g_mcu2.on_rx_byte(g_mcu2.last_rx_byte());
    return;
  }

  if (huart == &huart4)
  {
    const uint8_t c = g_ui_rx_byte;

    if (c == 'm' || c == 'M') {
      g_mcu2.set_audio_monitor_source(NcommMcu2::AudioMonitorSource::MIC);
      g_mcu2.set_stream(ncomm::StreamSelect::STREAM_MIC_RAW);
      uart4_write_str("AUD MON: MIC (TX)\r\n");
    } else if (c == 'r' || c == 'R') {
      g_mcu2.set_audio_monitor_source(NcommMcu2::AudioMonitorSource::RX);
      g_mcu2.set_stream(ncomm::StreamSelect::STREAM_RX_RAW);
      uart4_write_str("AUD MON: RX\r\n");
    } else if (c == 'h' || c == 'H' || c == '?') {
      uart4_write_str("Commands: m=monitor MIC(TX), r=monitor RX, h=?\r\n");
    }

    // re-arm UI RX
    HAL_UART_Receive_IT(&huart4, &g_ui_rx_byte, 1);
    return;
  }
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
  audio_i2s_out::on_i2s_tx_cplt(hi2s);
}

/* ---------- main ---------- */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART3_UART_Init();  // MCU1 link @1M
  MX_UART4_Init();        // UI/log @500k
  MX_I2S2_Init();         // PCM5102A output @16k, 32-bit

  g_mcu2.init(&huart3, &huart4);

  // start audio out
  audio_i2s_out::init(&hi2s2);
  audio_i2s_out::start();

  uart4_write_str("\r\nMCU2 MVP0+ DAC: init ok\r\n");
  uart4_write_str("Commands: m=monitor MIC(TX), r=monitor RX, h=?\r\n");

  // default: monitor RX
  g_mcu2.set_audio_monitor_source(NcommMcu2::AudioMonitorSource::RX);
  g_mcu2.set_stream(ncomm::StreamSelect::STREAM_RX_RAW);

  // arm UI RX
  HAL_UART_Receive_IT(&huart4, &g_ui_rx_byte, 1);

  while (1)
  {
    log_mcu2_stats_1s();
    send_ping_every_2s();
  }
}
