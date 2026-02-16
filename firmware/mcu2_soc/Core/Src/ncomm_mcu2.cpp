#include "ncomm_mcu2.hpp"
#include "audio_i2s_out.hpp"

#include <cstring>

static inline uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline void wr_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }

void NcommMcu2::init(UART_HandleTypeDef* uart_mcu1, UART_HandleTypeDef* uart_ui) {
  uart_mcu1_ = uart_mcu1;
  uart_ui_   = uart_ui;

  st_ = RxState::SOF0;
  hdr_pos_ = 0;
  payload_len_ = 0;
  payload_pos_ = 0;
  crc_rx_ = 0;

  arm_rx_it_();
}

void NcommMcu2::arm_rx_it_() {
  if (uart_mcu1_) {
    HAL_UART_Receive_IT(uart_mcu1_, &rx_byte_, 1);
  }
}

void NcommMcu2::tick_1ms() {}

void NcommMcu2::on_rx_byte(uint8_t b) {
  stats_.rx_bytes++;

  switch (st_) {
    case RxState::SOF0:
      st_ = (b == ncomm::SOF0) ? RxState::SOF1 : RxState::SOF0;
      break;

    case RxState::SOF1:
      if (b == ncomm::SOF1) {
        st_ = RxState::HEADER;
        hdr_pos_ = 0;
      } else {
        st_ = RxState::SOF0;
      }
      break;

    case RxState::HEADER:
      header_[hdr_pos_++] = b;
      if (hdr_pos_ >= ncomm::HEADER_SIZE) {
        msg_type_ = header_[0];
        msg_id_   = header_[1];
        payload_len_ = le16(&header_[2]);

        if (payload_len_ > ncomm::MAX_PAYLOAD) {
          st_ = RxState::SOF0;
          break;
        }

        payload_pos_ = 0;
        st_ = (payload_len_ == 0) ? RxState::CRC0 : RxState::PAYLOAD;
      }
      break;

    case RxState::PAYLOAD:
      payload_[payload_pos_++] = b;
      if (payload_pos_ >= payload_len_) {
        st_ = RxState::CRC0;
      }
      break;

    case RxState::CRC0:
      crc_rx_ = b;
      st_ = RxState::CRC1;
      break;

    case RxState::CRC1: {
      crc_rx_ |= (uint16_t)b << 8;

      uint8_t tmp[ncomm::HEADER_SIZE + ncomm::MAX_PAYLOAD];
      memcpy(tmp, header_, ncomm::HEADER_SIZE);
      if (payload_len_ > 0) memcpy(tmp + ncomm::HEADER_SIZE, payload_, payload_len_);

      const uint16_t crc_calc = ncomm::crc16_ccitt_false(tmp, ncomm::HEADER_SIZE + payload_len_);

      if (crc_calc == crc_rx_) {
        stats_.rx_frames_ok++;
        handle_frame_(msg_type_, payload_, payload_len_);
      } else {
        stats_.rx_frames_bad_crc++;
      }

      st_ = RxState::SOF0;
      break;
    }
  }

  arm_rx_it_();
}

bool NcommMcu2::send_frame_(uint8_t msg_type, const uint8_t* payload, uint16_t len) {
  if (!uart_mcu1_) return false;
  if (len > ncomm::MAX_PAYLOAD) return false;

  uint8_t buf[2 + ncomm::HEADER_SIZE + ncomm::MAX_PAYLOAD + ncomm::CRC_SIZE];
  size_t pos = 0;

  buf[pos++] = ncomm::SOF0;
  buf[pos++] = ncomm::SOF1;

  buf[pos++] = msg_type;
  buf[pos++] = tx_msg_id_++;

  wr_le16(&buf[pos], len);
  pos += 2;

  if (len && payload) {
    memcpy(&buf[pos], payload, len);
    pos += len;
  }

  const uint16_t crc = ncomm::crc16_ccitt_false(&buf[2], ncomm::HEADER_SIZE + len);
  buf[pos++] = (uint8_t)(crc & 0xFF);
  buf[pos++] = (uint8_t)(crc >> 8);

  const bool ok = (HAL_UART_Transmit(uart_mcu1_, buf, (uint16_t)pos, 50) == HAL_OK);
  if (ok) stats_.tx_frames++;
  return ok;
}

void NcommMcu2::send_ping() {
  send_frame_((uint8_t)ncomm::MsgType::CMD_PING, nullptr, 0);
}

void NcommMcu2::send_cmd_set_mode_(ncomm::Mode mode, ncomm::Ptt ptt,
                                  uint8_t rx_ve_enable, uint8_t tx_ve_enable) {
  uint8_t p[8]{};
  p[0] = (uint8_t)mode;
  p[1] = (uint8_t)ptt;
  p[2] = rx_ve_enable;
  p[3] = tx_ve_enable;
  send_frame_((uint8_t)ncomm::MsgType::CMD_SET_MODE, p, sizeof(p));
}

void NcommMcu2::send_cmd_set_streams_(uint8_t stream_rx_enable, uint8_t stream_tx_enable,
                                     uint8_t mic_to_mcu2_source, uint8_t reserved0) {
  uint8_t p[8]{};
  p[0] = stream_rx_enable;
  p[1] = stream_tx_enable;
  p[2] = mic_to_mcu2_source;
  p[3] = reserved0;
  send_frame_((uint8_t)ncomm::MsgType::CMD_SET_STREAMS, p, sizeof(p));
}

void NcommMcu2::set_stream(ncomm::StreamSelect sel) {
  if (sel == ncomm::StreamSelect::STREAM_MIC_RAW) {
    send_cmd_set_mode_(ncomm::Mode::TX, ncomm::Ptt::ON, 0, 0);
    send_cmd_set_streams_(0, 1, 0);
  } else {
    send_cmd_set_mode_(ncomm::Mode::RX, ncomm::Ptt::OFF, 0, 0);
    send_cmd_set_streams_(1, 0, 0);
  }
}

void NcommMcu2::set_audio_monitor_source(AudioMonitorSource src) {
  audio_monitor_src_ = src;
}

// Payload format assumed:
// [0]=stream_id, [1]=reserved, [2..3]=frame_len_bytes(u16 LE), [4..]=PCM16 little-endian
static void push_pcm_from_audio_frame_(const uint8_t* payload, uint16_t len) {
  if (!payload || len < 4) return;
  const uint16_t frame_len = le16(payload + 2);
  if (frame_len == 0) return;
  if ((uint32_t)frame_len + 4u > len) return;
  if ((frame_len & 1) != 0) return; // must be even for PCM16

  const int16_t* pcm = reinterpret_cast<const int16_t*>(payload + 4);
  const size_t samples = frame_len / 2;
  audio_i2s_out::push_pcm16_mono(pcm, samples);
}

void NcommMcu2::handle_frame_(uint8_t msg_type, const uint8_t* payload, uint16_t len) {
  switch ((ncomm::MsgType)msg_type) {

    case ncomm::MsgType::EVT_PONG:
      stats_.pong++;
      break;

    case ncomm::MsgType::EVT_VAD:
      stats_.vad++;
      break;

    case ncomm::MsgType::EVT_RX_AUDIO_FRAME:
      stats_.audio_rx++;
      if (audio_monitor_src_ == AudioMonitorSource::RX) {
        push_pcm_from_audio_frame_(payload, len);
      }
      break;

    case ncomm::MsgType::EVT_TX_AUDIO_FRAME:
      stats_.audio_tx++;
      if (audio_monitor_src_ == AudioMonitorSource::MIC) {
        push_pcm_from_audio_frame_(payload, len);
      }
      break;

    case ncomm::MsgType::EVT_MODE_ACK:
      stats_.ack_mode++;
      break;

    case ncomm::MsgType::EVT_STREAMS_ACK:
      stats_.ack_streams++;
      break;

    case ncomm::MsgType::EVT_VAD_CFG_ACK:
      stats_.ack_vad_cfg++;
      break;

    case ncomm::MsgType::EVT_ERROR:
      stats_.evt_error++;
      break;

    default:
      break;
  }
}
