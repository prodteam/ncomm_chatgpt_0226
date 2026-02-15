#include "ncomm_mcu1.hpp"

// HAL includes: adjust paths depending on Cube structure
#include "main.h"
#include "usart.h"
#include "i2s.h"
#include "stm32h7xx_hal.h"

#include <cstring>

namespace ncomm {

static UART_HandleTypeDef* s_uart = nullptr;
static I2S_HandleTypeDef*  s_i2s  = nullptr;
static Config s_cfg{};

static uint16_t s_seq = 0;

// ---- UART framing: MVP v0 ----
// Frame: [0xA5][type][len_lo][len_hi][payload...][crc16_lo][crc16_hi]
// crc16 = CCITT-FALSE over (type + len + payload), not including 0xA5 and not including crc bytes.
static constexpr uint8_t  SOF = 0xA5;

static uint16_t crc16_ccitt_false(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc = (crc << 1);
        }
    }
    return crc;
}

// Small RX parser (byte-by-byte)
enum class RxState { WAIT_SOF, TYPE, LEN0, LEN1, PAYLOAD, CRC0, CRC1 };

static RxState s_rx_state = RxState::WAIT_SOF;
static uint8_t  s_rx_type = 0;
static uint16_t s_rx_len  = 0;
static uint16_t s_rx_pos  = 0;
static uint16_t s_rx_crc  = 0;
static uint8_t  s_crc0    = 0;

static constexpr size_t RX_PAYLOAD_MAX = 512;
static uint8_t s_rx_payload[RX_PAYLOAD_MAX];

static void uart_send_frame(MsgType type, const uint8_t* payload, uint16_t len) {
    // Build in stack buffer (small + payload). For audio we send separate header + PCM.
    uint8_t hdr[4] = { (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8), 0 };
    // CRC over type+len+payload
    uint16_t crc = 0;
    {
        // temp: type+len bytes in array
        uint8_t tl[3] = { (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
        crc = crc16_ccitt_false(tl, sizeof(tl));
        if (payload && len) {
            crc = crc16_ccitt_false(payload, len) ^ crc16_ccitt_false(nullptr, 0); // keep compiler happy
            // ^ this line is wrong if taken literally; we'll compute properly below
        }
    }

    // Proper CRC calc (single pass)
    // We avoid allocating a big buffer: do CRC in 2 parts.
    uint8_t tl2[3] = { (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    crc = crc16_ccitt_false(tl2, sizeof(tl2));
    if (payload && len) {
        // continue CRC with payload
        // CCITT doesn't have "continue" in our helper, so emulate by concatenation in a small buffer if needed.
        // For MVP sizes, easiest is to compute in one buffer for control frames.
        // Audio frames use a dedicated sender below anyway.
        uint8_t tmp[3 + RX_PAYLOAD_MAX];
        if (len <= RX_PAYLOAD_MAX) {
            memcpy(tmp, tl2, 3);
            memcpy(tmp + 3, payload, len);
            crc = crc16_ccitt_false(tmp, 3 + len);
        }
    }

    uint8_t sof = SOF;
    uint8_t len_lo = (uint8_t)(len & 0xFF);
    uint8_t len_hi = (uint8_t)(len >> 8);
    uint8_t crc_lo = (uint8_t)(crc & 0xFF);
    uint8_t crc_hi = (uint8_t)(crc >> 8);

    HAL_UART_Transmit(s_uart, &sof, 1, 10);
    HAL_UART_Transmit(s_uart, (uint8_t*)&type, 1, 10);
    HAL_UART_Transmit(s_uart, &len_lo, 1, 10);
    HAL_UART_Transmit(s_uart, &len_hi, 1, 10);
    if (payload && len) HAL_UART_Transmit(s_uart, (uint8_t*)payload, len, 50);
    HAL_UART_Transmit(s_uart, &crc_lo, 1, 10);
    HAL_UART_Transmit(s_uart, &crc_hi, 1, 10);
}

static void uart_send_error(Err e) {
    uint8_t p[1] = { (uint8_t)e };
    uart_send_frame(MsgType::ERROR, p, 1);
}

static void handle_msg(uint8_t type, const uint8_t* payload, uint16_t len) {
    switch ((MsgType)type) {
        case MsgType::SET_STREAM:
            if (len != 1) { uart_send_error(Err::BAD_CMD); return; }
            if (payload[0] == (uint8_t)StreamId::STREAM_MIC_RAW) s_cfg.active_stream = StreamId::STREAM_MIC_RAW;
            else if (payload[0] == (uint8_t)StreamId::STREAM_RX_RAW) s_cfg.active_stream = StreamId::STREAM_RX_RAW;
            else { uart_send_error(Err::BAD_CMD); return; }
            break;

        case MsgType::SET_VAD_N_POSITIVE:
            if (len != 1 || payload[0] == 0 || payload[0] > 20) { uart_send_error(Err::BAD_CMD); return; }
            s_cfg.vad_n_positive = payload[0];
            break;

        case MsgType::SET_VAD_THRESHOLD:
            if (len != 2) { uart_send_error(Err::BAD_CMD); return; }
            s_cfg.vad_threshold = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            break;

        case MsgType::PING:
            uart_send_frame(MsgType::PONG, nullptr, 0);
            break;

        default:
            uart_send_error(Err::BAD_CMD);
            break;
    }
}

static void uart_rx_poll() {
    uint8_t b = 0;
    // Non-blocking-ish: timeout 0 means "return immediately" in many HALs; on H7 it may be HAL_TIMEOUT.
    // We'll use small timeout 1ms to keep it simple.
    if (HAL_UART_Receive(s_uart, &b, 1, 1) != HAL_OK) return;

    switch (s_rx_state) {
        case RxState::WAIT_SOF:
            if (b == SOF) s_rx_state = RxState::TYPE;
            break;

        case RxState::TYPE:
            s_rx_type = b;
            s_rx_state = RxState::LEN0;
            break;

        case RxState::LEN0:
            s_rx_len = b;
            s_rx_state = RxState::LEN1;
            break;

        case RxState::LEN1:
            s_rx_len |= (uint16_t)b << 8;
            if (s_rx_len > RX_PAYLOAD_MAX) {
                s_rx_state = RxState::WAIT_SOF;
                uart_send_error(Err::BAD_FRAME);
                break;
            }
            s_rx_pos = 0;
            s_rx_state = (s_rx_len == 0) ? RxState::CRC0 : RxState::PAYLOAD;
            break;

        case RxState::PAYLOAD:
            s_rx_payload[s_rx_pos++] = b;
            if (s_rx_pos >= s_rx_len) s_rx_state = RxState::CRC0;
            break;

        case RxState::CRC0:
            s_crc0 = b;
            s_rx_state = RxState::CRC1;
            break;

        case RxState::CRC1: {
            uint16_t rx_crc = (uint16_t)s_crc0 | ((uint16_t)b << 8);

            // compute CRC
            uint8_t tl[3] = { s_rx_type, (uint8_t)(s_rx_len & 0xFF), (uint8_t)(s_rx_len >> 8) };
            uint16_t crc = 0;
            if (s_rx_len <= RX_PAYLOAD_MAX) {
                uint8_t tmp[3 + RX_PAYLOAD_MAX];
                memcpy(tmp, tl, 3);
                if (s_rx_len) memcpy(tmp + 3, s_rx_payload, s_rx_len);
                crc = crc16_ccitt_false(tmp, 3 + s_rx_len);
            } else {
                crc = crc16_ccitt_false(tl, 3);
            }

            if (crc != rx_crc) {
                uart_send_error(Err::BAD_CRC);
            } else {
                handle_msg(s_rx_type, s_rx_payload, s_rx_len);
            }
            s_rx_state = RxState::WAIT_SOF;
            break;
        }
    }
}

// ---- Audio / VAD ----

// Expect stereo I2S 16-bit samples: [L, R] repeated.
// L = MIC_RAW, R = RX_RADIO_IN by default.
// If your wiring swaps channels, swap indices in extract below.
static int16_t s_i2s_stereo[Config::chunk_samples * 2]; // 512 samples (stereo pairs)
static int16_t s_mono[Config::chunk_samples];           // selected mono stream

static uint8_t s_vad_consecutive = 0;
static bool    s_vad_flag = false;

// Very simple VAD: avg(abs(samples)) > threshold
static bool vad_stub_is_voice(const int16_t* mic, size_t n, uint16_t thr) {
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = mic[i];
        if (v < 0) v = -v;
        acc += (uint32_t)v;
    }
    uint32_t avg = acc / (n ? n : 1);
    return avg > thr;
}

static void send_vad_status() {
    uint8_t p[2] = { (uint8_t)(s_vad_flag ? 1 : 0), s_vad_consecutive };
    uart_send_frame(MsgType::VAD_STATUS, p, sizeof(p));
}

// For audio we send:
// payload header = [stream_id][seq_lo][seq_hi][nsamp_lo][nsamp_hi] then PCM16LE mono
static void send_audio_frame(StreamId sid, const int16_t* pcm, uint16_t nsamp) {
    uint8_t hdr[5];
    hdr[0] = (uint8_t)sid;
    hdr[1] = (uint8_t)(s_seq & 0xFF);
    hdr[2] = (uint8_t)(s_seq >> 8);
    hdr[3] = (uint8_t)(nsamp & 0xFF);
    hdr[4] = (uint8_t)(nsamp >> 8);

    // total len = 5 + nsamp*2
    uint16_t len = (uint16_t)(5 + nsamp * 2);

    // Build a contiguous buffer for simplicity (<= 5+512 = 517 bytes)
    uint8_t buf[5 + Config::chunk_samples * 2];
    memcpy(buf, hdr, 5);
    memcpy(buf + 5, pcm, nsamp * 2);

    uart_send_frame(MsgType::AUDIO_FRAME, buf, len);
    s_seq++;
}

static bool read_i2s_chunk() {
    // Receive stereo words: length in 16-bit halfwords.
    // For 256 stereo frames => 512 halfwords.
    const uint16_t halfwords = Config::chunk_samples * 2;

    // Blocking receive; timeout in ms ~ 20-30ms for safety.
    HAL_StatusTypeDef st = HAL_I2S_Receive(s_i2s, (uint16_t*)s_i2s_stereo, halfwords, 30);
    return st == HAL_OK;
}

static void select_mono(StreamId sid) {
    // Extract either Left or Right channel from stereo interleaved
    const bool mic_left = true; // if wiring swapped, set to false or swap cases below

    for (uint16_t i = 0; i < Config::chunk_samples; i++) {
        int16_t L = s_i2s_stereo[i * 2 + 0];
        int16_t R = s_i2s_stereo[i * 2 + 1];

        if (sid == StreamId::STREAM_MIC_RAW) s_mono[i] = mic_left ? L : R;
        else                                 s_mono[i] = mic_left ? R : L;
    }
}

void NCommMcu1_Init(UART_HandleTypeDef* uart, I2S_HandleTypeDef* i2s, const Config& cfg) {
    s_uart = uart;
    s_i2s  = i2s;
    s_cfg  = cfg;

    s_rx_state = RxState::WAIT_SOF;
    s_seq = 0;
    s_vad_consecutive = 0;
    s_vad_flag = false;

    // Optional: send initial PONG to show "alive"
    uart_send_frame(MsgType::PONG, nullptr, 0);
}

void NCommMcu1_Loop() {
    // 1) poll UART commands
    uart_rx_poll();

    // 2) read one audio chunk
    if (!read_i2s_chunk()) {
        // if I2S read fails, don't spam; but send an error once in a while if needed
        return;
    }

    // 3) VAD stub always computed on MIC_RAW
    select_mono(StreamId::STREAM_MIC_RAW);
    bool voice = vad_stub_is_voice(s_mono, Config::chunk_samples, s_cfg.vad_threshold);

    if (voice) {
        if (s_vad_consecutive < 255) s_vad_consecutive++;
    } else {
        s_vad_consecutive = 0;
    }
    s_vad_flag = (s_vad_consecutive >= s_cfg.vad_n_positive);

    send_vad_status();

    // 4) send selected stream only (MVP bandwidth constraint)
    select_mono(s_cfg.active_stream);
    send_audio_frame(s_cfg.active_stream, s_mono, Config::chunk_samples);
}

} // namespace ncomm
