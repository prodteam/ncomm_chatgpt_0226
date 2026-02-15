#pragma once

namespace ncomm {

// Canonical stream names (MUST be used everywhere)
enum class StreamId : uint8_t {
  MIC_RAW = 0,
  MIC_VE  = 1,
  RX_STREAM_OUT = 2,
  TX_AUDIO_OUT  = 3,
};

} // namespace ncomm
