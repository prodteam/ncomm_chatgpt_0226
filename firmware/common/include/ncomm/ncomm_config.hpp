#pragma once
#include <cstdint>

namespace ncomm {

// Config registers (MCU2 -> MCU1)
struct Mcu1Config {
  // VE contract: when disabled, output = bypass input (copy)
  bool ve_rx_enable = false;
  bool ve_tx_enable = false;

  // SR/KWS source selection for MCU2:
  // false -> MIC_RAW, true -> MIC_VE
  bool mic_to_mcu2_use_ve = false;

  // TX gate policy:
  // true -> send TX_AUDIO_OUT only when PTT=ON
  bool tx_gate_ptt_only = true;

  // VAD policy
  uint8_t vad_positive_count = 3; // configurable, default 3
};

} // namespace ncomm
