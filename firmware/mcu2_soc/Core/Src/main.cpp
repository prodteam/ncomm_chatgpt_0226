/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.cpp
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "gpio.h"
#include "usart.h"

/* USER CODE BEGIN Includes */
#include "ncomm_mcu2.hpp"
#include <cstring>
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static NcommMcu2 g_mcu2;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN 0 */

// ---------- UART4 (UI) log helpers ----------
static void uart4_write_buf(const uint8_t* data, uint16_t len) {
  if (!data || !len) return;
  (void)HAL_UART_Transmit(&huart4, const_cast<uint8_t*>(data), len, 50);
}

static void uart4_write_str(const char* s) {
  if (!s) return;
  uart4_write_buf(reinterpret_cast<const uint8_t*>(s), (uint16_t)strlen(s));
}

static char* u32_to_dec(char* out, uint32_t v) {
  char tmp[11];
  int n = 0;
  if (v == 0) { *out++ = '0'; *out = 0; return out; }
  while (v && n < 10) { tmp[n++] = char('0' + (v % 10)); v /= 10; }
  while (n--) { *out++ = tmp[n]; }
  *out = 0;
  return out;
}

static void log_mcu2_stats_1s(NcommMcu2& mcu2) {
  static uint32_t last_ms = 0;
  const uint32_t now = HAL_GetTick();
  if ((now - last_ms) < 1000) return;
  last_ms = now;

  const auto& st = mcu2.stats();

  char line[240];
  char* p = line;

  // "t=12345 rxB=.. ok=.. crcBad=.. tx=.. pong=.. vad=.. aRx=.. aTx=.. ackM=.. ackS=.. ackV=.. err=..\r\n"
  memcpy(p, "t=", 2); p += 2; p = u32_to_dec(p, now);

  memcpy(p, " rxB=", 5); p += 5; p = u32_to_dec(p, st.rx_bytes);
  memcpy(p, " ok=", 4); p += 4; p = u32_to_dec(p, st.rx_frames_ok);
  memcpy(p, " crcBad=", 8); p += 8; p = u32_to_dec(p, st.rx_frames_bad_crc);

  memcpy(p, " tx=", 4); p += 4; p = u32_to_dec(p, st.tx_frames);

  memcpy(p, " pong=", 6); p += 6; p = u32_to_dec(p, st.pong);
  memcpy(p, " vad=", 5); p += 5; p = u32_to_dec(p, st.vad);

  memcpy(p, " aRx=", 5); p += 5; p = u32_to_dec(p, st.audio_rx);
  memcpy(p, " aTx=", 5); p += 5; p = u32_to_dec(p, st.audio_tx);

  memcpy(p, " ackM=", 6); p += 6; p = u32_to_dec(p, st.ack_mode);
  memcpy(p, " ackS=", 6); p += 6; p = u32_to_dec(p, st.ack_streams);
  memcpy(p, " ackV=", 6); p += 6; p = u32_to_dec(p, st.ack_vad_cfg);

  memcpy(p, " err=", 5); p += 5; p = u32_to_dec(p, st.evt_error);

  memcpy(p, "\r\n", 2); p += 2;
  *p = 0;

  uart4_write_str(line);
}

// Optional: periodic ping (helps prove link alive)
static void send_ping_every_2s(NcommMcu2& mcu2) {
  static uint32_t last_ms = 0;
  const uint32_t now = HAL_GetTick();
  if ((now - last_ms) < 2000) return;
  last_ms = now;
  mcu2.send_ping();
}

/**
 * NOTE:
 * - USART3 @ 1,000,000 is MCU1<->MCU2 link (protocol + audio frames)
 * - UART4  @ 500,000 is MCU2<->UI/MCU3 debug/log link (text logs)
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == &huart3)
  {
    // One byte received from MCU1 link -> consume it.
    g_mcu2.on_rx_byte(g_mcu2.last_rx_byte());
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART3_UART_Init();  // MCU1 link @1M
  MX_UART4_Init();        // UI/log @500k

  /* USER CODE BEGIN 2 */

  g_mcu2.init(&huart3, &huart4);

  uart4_write_str("\r\nMCU2 MVP0: init ok\r\n");
  uart4_write_str("Log format: t=.. rxB=.. ok=.. crcBad=.. tx=.. pong=.. vad=.. aRx=.. aTx=.. ackM=.. ackS=.. ackV=.. err=..\r\n");

  // Optional: choose stream (only if MCU1 supports CMD_SET_MODE/CMD_SET_STREAMS)
  // g_mcu2.set_stream(ncomm::StreamSelect::STREAM_MIC_RAW);

  /* USER CODE END 2 */

  while (1)
  {
    log_mcu2_stats_1s(g_mcu2);
    send_ping_every_2s(g_mcu2);
  }
}

/* SystemClock_Config / Error_Handler оставлены как сгенерировано Cube ... */

