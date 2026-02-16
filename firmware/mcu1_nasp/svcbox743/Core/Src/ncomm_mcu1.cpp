#include "ncomm_mcu1.hpp"

#include "usart.h" // huart3 etc (Cube)
#include <cstring>

#include "ncomm/ncomm_protocol.hpp"
#include "ncomm/ncomm_streams.hpp"

namespace ncomm::mcu1 {

using ncomm::FrameHeader;
using ncomm::MsgType;
using ncomm::StreamId;

static UART_HandleTypeDef* g_uart = nullptr;
static Config g_cfg{};

static StreamId g_active_stream = StreamId::MIC_RAW; // default

static uint8_t g_seq = 0;

// -------- CRC16 CCITT-FALSE (poly 0x1021, init 0xFFFF, xorout 0x0000) --------
static uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc;
}

// -------- UART framing --------
static constexpr size_t NCOMM_MAX_PAYLOAD = 768; // safe for MVP (256 samples *2 + 2..)
static uint8_t tx_scratch[sizeof(FrameHeader) + NCOMM_MAX_PAYLOAD];

static void uart_send_frame(MsgType type, const uint8_t* payload, uint16_t payload_len) {
  if (!g_uart) return;
  if (payload_len > NCOMM_MAX_PAYLOAD) return;

  FrameHeader h{};
  h.sof = ncomm::SOF;
  h.ver = ncomm::PROTO_VER;
  h.type = static_cast<uint8_t>(type);
  h.seq = g_seq++;
  h.len = payload_len;

  // scratch = header + payload
  std::memcpy(tx_scratch, &h, sizeof(h));
  if (payload_len && payload) {
    std::memcpy(tx_scratch + sizeof(h), payload, payload_len);
  }

  const uint16_t crc = crc16_ccitt_false(tx_scratch, sizeof(h) + payload_len);
  uint8_t crc_bytes[2] = { static_cast<uint8_t>(crc & 0xFF),
                           static_cast<uint8_t>((crc >> 8) & 0xFF) };

  HAL_UART_Transmit(g_uart, tx_scratch, sizeof(h) + payload_len, 10);
  HAL_UART_Transmit(g_uart, crc_bytes, 2, 10);
}

// -------- RX command handling --------
struct SetStreamsPayload {
  uint8_t stream_id;
};

static bool uart_read_exact(uint8_t* dst, uint16_t len) {
  return (HAL_UART_Receive(g_uart, dst, len, 2) == HAL_OK);
}

static void handle_command(MsgType type, const uint8_t* payload, uint16_t len) {
  switch (type) {
    case MsgType::SET_STREAMS: {
      if (len < sizeof(SetStreamsPayload)) return;
      const auto* p = reinterpret_cast<const SetStreamsPayload*>(payload);
      g_active_stream = static_cast<StreamId>(p->stream_id);
    } break;

    case MsgType::PING: {
      uart_send_frame(MsgType::PONG, nullptr, 0);
    } break;

    default:
      // ignore in MVP
      break;
  }
}

static void poll_uart_commands() {
  if (!g_uart) return;

  // Non-blocking-ish: try to read SOF; if no data -> return
  uint8_t sof = 0;
  if (HAL_UART_Receive(g_uart, &sof, 1, 0) != HAL_OK) return;
  if (sof != ncomm::SOF) return;

  FrameHeader h{};
  h.sof = sof;
  if (!uart_read_exact(reinterpret_cast<uint8_t*>(&h) + 1, sizeof(FrameHeader) - 1)) return;

  if (h.ver != ncomm::PROTO_VER) return;
  if (h.len > NCOMM_MAX_PAYLOAD) return;

  uint8_t payload[NCOMM_MAX_PAYLOAD];
  if (h.len) {
    if (!uart_read_exact(payload, h.len)) return;
  }

  uint8_t crc_bytes[2];
  if (!uart_read_exact(crc_bytes, 2)) return;
  const uint16_t crc_rx = (uint16_t)crc_bytes[0] | ((uint16_t)crc_bytes[1] << 8);

  // validate CRC
  std::memcpy(tx_scratch, &h, sizeof(h));
  if (h.len) std::memcpy(tx_scratch + sizeof(h), payload, h.len);
  const uint16_t crc_calc = crc16_ccitt_false(tx_scratch, sizeof(h) + h.len);
  if (crc_calc != crc_rx) return;

  handle_command(static_cast<MsgType>(h.type), payload, h.len);
}

// -------- MVP VAD stub + audio chunk sender --------
static uint16_t vad_consecutive = 0;
static bool vad_state = false;

// TODO: replace with real MIC_RAW energy/VAD
static bool vad_stub_tick() {
  // simplest stub: toggles every ~200 loops
  static uint32_t ctr = 0;
  ctr++;
  const bool cur = ((ctr / 200) % 2) == 1;
  return cur;
}

static void send_vad_status(bool vad_now) {
  // payload: [0]=vad(0/1), [1]=consecutive_low8
  uint8_t p[2] = { static_cast<uint8_t>(vad_now ? 1 : 0),
                   static_cast<uint8_t>(vad_consecutive & 0xFF) };
  uart_send_frame(MsgType::VAD_STATUS, p, sizeof(p));
}

static void send_audio_chunk(StreamId sid) {
  // MVP: send dummy PCM. Replace later by real ADC samples (MIC_RAW / RX_RADIO_IN)
  // payload format (MVP):
  //   [0] stream_id
  //   [1..] int16 PCM LE, audio_samples_per_chunk samples
  const uint16_t n = g_cfg.audio_samples_per_chunk;
  const uint16_t bytes_pcm = n * 2;
  const uint16_t total = 1 + bytes_pcm;
  if (total > NCOMM_MAX_PAYLOAD) return;

  uint8_t p[NCOMM_MAX_PAYLOAD];
  p[0] = static_cast<uint8_t>(sid);

  int16_t* pcm = reinterpret_cast<int16_t*>(&p[1]);
  static int16_t phase = 0;
  for (uint16_t i = 0; i < n; i++) {
    // tiny sawtooth, so DAC path is testable
    pcm[i] = phase;
    phase += 150;
  }

  uart_send_frame(MsgType::AUDIO_CHUNK, p, total);
}

// -------- Public API --------
void Init(UART_HandleTypeDef* huart) {
  g_uart = huart;
  g_seq = 0;
  vad_consecutive = 0;
  vad_state = false;
  g_active_stream = StreamId::MIC_RAW;
}

void Loop() {
  poll_uart_commands();

  // VAD stub runs always on MIC_RAW
  const bool vad_now = vad_stub_tick();
  if (vad_now) {
    if (vad_consecutive < 0xFFFF) vad_consecutive++;
  } else {
    vad_consecutive = 0;
  }

  const bool new_state = (vad_consecutive >= g_cfg.vad_positive_required);
  if (new_state != vad_state) {
    vad_state = new_state;
    send_vad_status(vad_state);
  }

  // Send only selected stream (to fit 1M UART MVP)
  send_audio_chunk(g_active_stream);
}

} // namespace ncomm::mcu1
