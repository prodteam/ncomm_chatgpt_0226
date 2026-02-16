#pragma once

#include <cstdint>
#include "usart.h"
#include "firmware/common/include/ncomm/ncomm_protocol.hpp"

// MCU2 MVP-0:
// - UART link to MCU1: USART3 @ 1M
// - UART link to MCU3/UI: UART4 @ 500k (optional now)
// - Parse UART v1.4.0 frames from MCU1
// - Send PING / SET_STREAM (mapped to CMD_SET_MODE + CMD_SET_STREAMS)

class NcommMcu2 {
public:
  void init(UART_HandleTypeDef* uart_mcu1, UART_HandleTypeDef* uart_ui = nullptr);

  // Call from HAL_UART_RxCpltCallback (byte-by-byte)
  void on_rx_byte(uint8_t b);

  // Commands to MCU1
  void send_ping();
  void set_stream(ncomm::StreamSelect sel); // MCU2 API requested: MIC_RAW vs RX_RAW

  // Optional: periodic housekeeping (timeouts, stats)
  void tick_1ms();

private:
  UART_HandleTypeDef* uart_mcu1_ = nullptr;
  UART_HandleTypeDef* uart_ui_   = nullptr;

  // RX state machine
  enum class RxState : uint8_t { SOF0, SOF1, HEADER, PAYLOAD, CRC0, CRC1 };
  RxState st_ = RxState::SOF0;

  uint8_t header_[ncomm::HEADER_SIZE]{};
  uint16_t hdr_pos_ = 0;

  uint8_t payload_[ncomm::MAX_PAYLOAD]{};
  uint16_t payload_len_ = 0;
  uint16_t payload_pos_ = 0;

  uint16_t crc_rx_ = 0;
  uint8_t msg_type_ = 0;
  uint8_t msg_id_ = 0;

  uint8_t rx_byte_ = 0;
  uint8_t tx_msg_id_ = 1;

  void arm_rx_it_();
  void handle_frame_(uint8_t msg_type, const uint8_t* payload, uint16_t len);

  // low-level send
  bool send_frame_(uint8_t msg_type, const uint8_t* payload, uint16_t len);

  // Mapping for MVP SET_STREAM → protocol CMD_SET_MODE + CMD_SET_STREAMS
  void send_cmd_set_mode_(ncomm::Mode mode, ncomm::Ptt ptt,
                          uint8_t rx_ve_enable, uint8_t tx_ve_enable);
  void send_cmd_set_streams_(uint8_t stream_rx_enable, uint8_t stream_tx_enable,
                             uint8_t mic_to_mcu2_source, uint8_t reserved0 = 0);
};
