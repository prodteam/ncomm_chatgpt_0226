#pragma once
#include <cstdint>

namespace ncomm {

// Link assumptions:
// - UART MCU1<->MCU2: USART3 @ 1'000'000 baud
// - Framing: [SOF][ver][type][seq][len_le16][payload...][crc_le16]

static constexpr uint8_t SOF = 0xA5;
static constexpr uint8_t PROTO_VER = 0x10; // 1.0

enum class MsgType : uint8_t {
  // Control
  PING            = 0x01,
  ACK             = 0x02,
  SET_CONFIG      = 0x03,
  SET_STREAMS     = 0x04,
  PTT_STATE       = 0x05,

  // Telemetry
  VAD_STATUS      = 0x20,
  STATS           = 0x21,
  ERROR           = 0x22,

  // Audio (payload = PCM frames)
  AUDIO_CHUNK     = 0x40,
};

#pragma pack(push, 1)
struct FrameHeader {
  uint8_t  sof;
  uint8_t  ver;
  uint8_t  type;
  uint8_t  seq;
  uint16_t len;   // payload bytes
};
#pragma pack(pop)

} // namespace ncomm
