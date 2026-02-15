#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Forward-declare STM32 HAL handles to avoid heavy includes in headers.
// In .cpp we'll include "main.h" / "usart.h" / "i2s.h" as needed.
typedef struct UART_HandleTypeDef UART_HandleTypeDef;
typedef struct I2S_HandleTypeDef  I2S_HandleTypeDef;

#ifdef __cplusplus
}
#endif

namespace ncomm {

// ---- Streams (MVP) ----
enum class StreamId : uint8_t {
    STREAM_MIC_RAW = 0x01,   // MIC_RAW (ADC CH1 / I2S Left by default)
    STREAM_RX_RAW  = 0x02,   // RX_RADIO_IN (ADC CH2 / I2S Right by default)
};

// ---- UART message types (MVP protocol v0) ----
enum class MsgType : uint8_t {
    // Host/MCU2 -> MCU1
    SET_STREAM          = 0x01, // payload: [stream_id]
    SET_VAD_N_POSITIVE  = 0x02, // payload: [N] (default 3)
    SET_VAD_THRESHOLD   = 0x03, // payload: uint16_t threshold (little-endian), default 500
    PING                = 0x04, // payload: none

    // MCU1 -> Host/MCU2
    PONG                = 0x84, // payload: none
    VAD_STATUS          = 0x81, // payload: [vad_flag][vad_consecutive]
    AUDIO_FRAME         = 0x82, // payload: [stream_id][seq_lo][seq_hi][nsamp_lo][nsamp_hi] + PCM16 mono
    ERROR               = 0xE0, // payload: [err_code]
};

// ---- Error codes ----
enum class Err : uint8_t {
    BAD_FRAME   = 0x01,
    BAD_CRC     = 0x02,
    BAD_CMD     = 0x03,
};

// ---- Config ----
struct Config {
    StreamId active_stream = StreamId::STREAM_MIC_RAW;

    // VAD stub policy
    uint8_t  vad_n_positive = 3;     // "N подряд" (default 3)
    uint16_t vad_threshold  = 500;   // simple energy threshold on MIC_RAW (abs avg)

    // Audio chunking (16ms at 16kHz => 256 samples)
    static constexpr uint32_t sample_rate_hz = 16000;
    static constexpr uint16_t chunk_samples  = 256; // 16ms
};

// Public API
void NCommMcu1_Init(UART_HandleTypeDef* uart, I2S_HandleTypeDef* i2s, const Config& cfg = {});
void NCommMcu1_Loop();   // call from while(1)

} // namespace ncomm
