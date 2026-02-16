#pragma once

#include <cstdint>
#include <cstddef>

namespace ncomm {

// ===== UART Protocol v1.4.0 framing =====
// SOF: 0xAA 0x55
// Header: msg_type (1), msg_id (1), payload_len (uint16 LE)
// CRC16: CRC16-CCITT-FALSE over header+payload (poly 0x1021, init 0xFFFF), LE in frame

static constexpr uint8_t SOF0 = 0xAA;
static constexpr uint8_t SOF1 = 0x55;

static constexpr size_t HEADER_SIZE = 4;
static constexpr size_t CRC_SIZE = 2;

static constexpr uint16_t CRC16_INIT = 0xFFFF;
static constexpr uint16_t CRC16_POLY = 0x1021;

// Keep enough for 16ms @16kHz mono: 256 samples *2 =512 bytes + 4 bytes meta + small headroom
static constexpr size_t MAX_PAYLOAD = 576;

// ---- Message types (subset for MVP-0) ----
enum class MsgType : uint8_t {
  CMD_PING          = 0x10,
  CMD_RESET         = 0x11,
  CMD_SET_MODE      = 0x12,
  CMD_SET_VAD_CFG   = 0x13,
  CMD_SET_STREAMS   = 0x14,

  EVT_PONG          = 0x80,
  EVT_RESET_ACK     = 0x85,
  EVT_VAD           = 0x84,
  EVT_MODE_ACK      = 0x82,
  EVT_STREAMS_ACK   = 0x83,
  EVT_VAD_CFG_ACK   = 0x86,
  EVT_ERROR         = 0x87,

  EVT_RX_AUDIO_FRAME = 0x90,
  EVT_TX_AUDIO_FRAME = 0x91,
};

// ---- Mode / Stream enums ----
enum class Mode : uint8_t {
  IDLE = 0,
  RX   = 1,
  TX   = 2,
};

enum class Ptt : uint8_t {
  OFF = 0,
  ON  = 1,
};

// MVP "SET_STREAM" abstraction requested by you (MCU2 local API)
enum class StreamSelect : uint8_t {
  STREAM_MIC_RAW = 1, // MIC -> TX_AUDIO_OUT direction
  STREAM_RX_RAW  = 2, // RX_RADIO_IN -> RX_STREAM_OUT direction
};

// ---- CRC16-CCITT-FALSE ----
inline uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
  uint16_t crc = CRC16_INIT;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ CRC16_POLY) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// ---- Frame builder helper ----
struct Frame {
  uint8_t  sof[2] = {SOF0, SOF1};
  uint8_t  msg_type = 0;
  uint8_t  msg_id   = 0;
  uint16_t payload_len = 0; // LE on wire
  // payload follows
  // crc follows (LE)
};

} // namespace ncomm
