#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "main.h"
#include "usart.h"

void ncomm_uart_send(uint8_t ver, uint8_t type, uint8_t flags, const uint8_t* payload, uint16_t len);

// ===== Protocol constants (match UART_Protocol_Spec_v1.4.0) =====
#define NCOMM_VER 0x10

// Commands (MCU2->MCU1)
#define CMD_PING            0x01
#define CMD_GET_INFO        0x02
#define CMD_SET_MODE        0x10
#define CMD_SET_STREAMS     0x11
#define CMD_RESET_STATE     0x12
#define CMD_SET_VAD_CONFIG  0x13

// Events (MCU1->MCU2)
#define EVT_PONG            0x80
#define EVT_INFO            0x81
#define EVT_MODE_ACK        0x90
#define EVT_STREAMS_ACK     0x91
#define EVT_RESET_ACK       0x92
#define EVT_VAD_CONFIG_ACK  0x93
#define EVT_VAD_STATUS      0xA0
#define EVT_AUDIO_FRAME     0xA1

// stream_id in EVT_AUDIO_FRAME payload (we keep it simple for MVP)
#define STREAM_ID_RX_STREAM_OUT  0x01
#define STREAM_ID_TX_AUDIO_OUT   0x02

// ===== MVP runtime config =====
typedef enum {
    MODE_STANDBY = 0,
    MODE_RX      = 1,
    MODE_TX      = 2,
} ncomm_mode_t;

static struct {
    ncomm_mode_t mode;
    uint8_t ptt;
    uint8_t rx_ve_enable;
    uint8_t tx_ve_enable;

    uint8_t stream_rx_enable;
    uint8_t stream_tx_enable;
    uint8_t vad_evt_enable;

    uint16_t frame_samples; // default 256 (16ms@16k)

    // VAD config
    uint8_t vad_start_marker; // consecutive true
    uint8_t vad_stop_marker;  // consecutive false
    uint16_t vad_chunk_ms;    // 16
    uint16_t vad_preroll_ms;  // unused in MVP

    // counters
    uint32_t audio_frame_index;
    uint8_t  vad_true_run;
    uint8_t  vad_false_run;
    uint8_t  vad_state; // 0/1
} g;

// ===== UART RX minimal parser (SOF resync) =====
#define RX_BUF_SZ 1024
static uint8_t rxbuf[RX_BUF_SZ];
static uint16_t rxlen = 0;

static bool uart_rx_poll_byte(uint8_t* out) {
    // non-blocking read 1 byte
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE) != RESET) {
        *out = (uint8_t)(huart3.Instance->RDR & 0xFF);
        return true;
    }
    return false;
}

// CRC helper (declared in ncomm_crc16.c)
uint16_t ncomm_crc16_ccitt_false(const uint8_t* data, size_t len);

// Try parse one packet from rxbuf; on success consume and return true.
static bool try_parse_packet(uint8_t* out_type, uint8_t* out_flags, uint8_t* out_seq,
                             uint8_t* payload, uint16_t* out_len) {
    // Need at least SOF + fixed header + CRC
    if (rxlen < 10) return false;

    // find SOF
    uint16_t i = 0;
    while (i + 1 < rxlen) {
        if (rxbuf[i] == 0xAA && rxbuf[i+1] == 0x55) break;
        i++;
    }
    if (i > 0) {
        memmove(rxbuf, rxbuf + i, rxlen - i);
        rxlen -= i;
        if (rxlen < 10) return false;
    }
    if (!(rxbuf[0] == 0xAA && rxbuf[1] == 0x55)) return false;

    uint8_t ver   = rxbuf[2];
    uint8_t type  = rxbuf[3];
    uint8_t flags = rxbuf[4];
    uint8_t seq   = rxbuf[5];
    uint16_t len  = (uint16_t)rxbuf[6] | ((uint16_t)rxbuf[7] << 8);

    if (ver != NCOMM_VER) {
        // drop SOF and resync
        memmove(rxbuf, rxbuf + 2, rxlen - 2);
        rxlen -= 2;
        return false;
    }

    uint16_t total = 2 + 1 + 1 + 1 + 1 + 2 + len + 2;
    if (rxlen < total) return false;

    // CRC over VER..PAYLOAD
    uint16_t crc_rx = (uint16_t)rxbuf[total-2] | ((uint16_t)rxbuf[total-1] << 8);

    // Build contiguous buffer for CRC (bounded for MVP)
    uint8_t crcbuf[6 + 512];
    uint16_t crc_plen = (len > 512) ? 512 : len;
    crcbuf[0] = rxbuf[2];
    crcbuf[1] = rxbuf[3];
    crcbuf[2] = rxbuf[4];
    crcbuf[3] = rxbuf[5];
    crcbuf[4] = rxbuf[6];
    crcbuf[5] = rxbuf[7];
    if (crc_plen) memcpy(&crcbuf[6], &rxbuf[8], crc_plen);

    uint16_t crc_calc = ncomm_crc16_ccitt_false(crcbuf, 6 + crc_plen);
    if (crc_calc != crc_rx) {
        // CRC fail -> resync by dropping first byte
        memmove(rxbuf, rxbuf + 1, rxlen - 1);
        rxlen -= 1;
        return false;
    }

    *out_type = type;
    *out_flags = flags;
    *out_seq = seq;
    *out_len = len;
    if (payload && len) memcpy(payload, &rxbuf[8], len);

    // consume packet
    memmove(rxbuf, rxbuf + total, rxlen - total);
    rxlen -= total;
    return true;
}

// ===== Audio generator (MVP) =====
// 16kHz, 16-bit mono
static int16_t gen_sample_sine(uint32_t n) {
    // 1 kHz sine
    const float fs = 16000.0f;
    const float f  = 1000.0f;
    float t = (float)n / fs;
    float s = sinf(2.0f * 3.1415926f * f * t);
    return (int16_t)(s * 12000.0f);
}

static uint8_t compute_vad_stub(const int16_t* pcm, uint16_t n) {
    // simple energy threshold
    uint32_t acc = 0;
    for (uint16_t i = 0; i < n; i++) {
        int32_t x = pcm[i];
        acc += (uint32_t)(x < 0 ? -x : x);
    }
    uint32_t avg = acc / (n ? n : 1);
    return (avg > 800) ? 1 : 0;
}

static void send_info(void) {
    // Minimal INFO payload: ascii, keep short
    const char msg[] = "MCU1 NASP-EMUL MVP0";
    ncomm_uart_send(NCOMM_VER, EVT_INFO, 0x00, (const uint8_t*)msg, (uint16_t)sizeof(msg)-1);
}

static void send_vad_status(uint8_t vad_now) {
    // EVT_VAD_STATUS payload (example: 4 bytes)
    // [0]=vad_state [1]=true_run [2]=false_run [3]=reserved
    uint8_t p[4] = { vad_now, g.vad_true_run, g.vad_false_run, 0 };
    ncomm_uart_send(NCOMM_VER, EVT_VAD_STATUS, 0x00, p, sizeof(p));
}

static void send_audio_frame(uint8_t stream_id, const int16_t* pcm, uint16_t samples) {
    // EVT_AUDIO_FRAME payload (per spec section 6.2 audio)
    // We'll use:
    // u8 stream_id
    // u8 channels (1)
    // u16 frame_samples
    // u32 frame_index
    // PCM16LE data...
    uint16_t header_sz = 1 + 1 + 2 + 4;
    uint16_t pcm_bytes = (uint16_t)(samples * 2);
    uint16_t total = header_sz + pcm_bytes;

    // MVP cap
    static uint8_t buf[1 + 1 + 2 + 4 + 512];
    if (total > sizeof(buf)) return;

    buf[0] = stream_id;
    buf[1] = 1; // mono
    buf[2] = (uint8_t)(samples & 0xFF);
    buf[3] = (uint8_t)((samples >> 8) & 0xFF);
    uint32_t idx = g.audio_frame_index++;
    buf[4] = (uint8_t)(idx & 0xFF);
    buf[5] = (uint8_t)((idx >> 8) & 0xFF);
    buf[6] = (uint8_t)((idx >> 16) & 0xFF);
    buf[7] = (uint8_t)((idx >> 24) & 0xFF);

    memcpy(&buf[8], pcm, pcm_bytes);

    ncomm_uart_send(NCOMM_VER, EVT_AUDIO_FRAME, 0x00, buf, total);
}

// ===== Public API called from main.c =====
void ncomm_app_init(void) {
    memset(&g, 0, sizeof(g));
    g.mode = MODE_STANDBY;
    g.frame_samples = 256;
    g.vad_start_marker = 3; // default (your policy)
    g.vad_stop_marker  = 3;
    g.vad_chunk_ms     = 16;
    g.vad_preroll_ms   = 0;
}

void ncomm_app_tick(void) {
    // RX bytes into rxbuf
    uint8_t b;
    while (uart_rx_poll_byte(&b)) {
        if (rxlen < RX_BUF_SZ) rxbuf[rxlen++] = b;
        else { rxlen = 0; } // overflow -> drop
    }

    // parse packets
    uint8_t type=0, flags=0, seq=0;
    uint16_t plen=0;
    uint8_t payload[64];

    while (try_parse_packet(&type, &flags, &seq, payload, &plen)) {
        switch (type) {
            case CMD_PING:
                ncomm_uart_send(NCOMM_VER, EVT_PONG, 0x00, NULL, 0);
                break;
            case CMD_GET_INFO:
                send_info();
                break;
            case CMD_SET_MODE:
                if (plen >= 8) {
                    g.mode = (ncomm_mode_t)payload[0];
                    g.ptt  = payload[1];
                    g.rx_ve_enable = payload[2];
                    g.tx_ve_enable = payload[3];
                }
                ncomm_uart_send(NCOMM_VER, EVT_MODE_ACK, 0x00, NULL, 0);
                break;
            case CMD_SET_STREAMS:
                if (plen >= 8) {
                    g.stream_rx_enable = payload[0];
                    g.stream_tx_enable = payload[1];
                    g.vad_evt_enable   = payload[2];
                    g.frame_samples    = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
                    if (g.frame_samples == 0) g.frame_samples = 256;
                }
                ncomm_uart_send(NCOMM_VER, EVT_STREAMS_ACK, 0x00, NULL, 0);
                break;
            case CMD_RESET_STATE:
                ncomm_app_init();
                ncomm_uart_send(NCOMM_VER, EVT_RESET_ACK, 0x00, NULL, 0);
                break;
            case CMD_SET_VAD_CONFIG:
                if (plen >= 8) {
                    g.vad_start_marker = payload[0];
                    g.vad_stop_marker  = payload[1];
                    g.vad_chunk_ms     = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
                    g.vad_preroll_ms   = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
                }
                ncomm_uart_send(NCOMM_VER, EVT_VAD_CONFIG_ACK, 0x00, NULL, 0);
                break;
            default:
                // ignore unknown
                break;
        }
    }

    // ===== Produce one 16ms chunk worth of audio each tick (simple pacing) =====
    // MVP pacing: crude delay by SysTick time.
    static uint32_t last_ms = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_ms) < 16) return;
    last_ms = now;

    // generate frame
    uint16_t N = g.frame_samples;
    if (N > 256) N = 256; // MVP cap for buffer
    static int16_t pcm[256];
    for (uint16_t i = 0; i < N; i++) {
        pcm[i] = gen_sample_sine(g.audio_frame_index * N + i);
    }

    // VAD stub always computed on "MIC_RAW conceptual"
    uint8_t vad_now = compute_vad_stub(pcm, N);
    if (vad_now) { g.vad_true_run++; g.vad_false_run = 0; }
    else         { g.vad_false_run++; g.vad_true_run = 0; }

    // latch state with markers
    if (!g.vad_state && g.vad_true_run >= g.vad_start_marker) g.vad_state = 1;
    if ( g.vad_state && g.vad_false_run >= g.vad_stop_marker) g.vad_state = 0;

    if (g.vad_evt_enable) send_vad_status(g.vad_state);

    // audio streaming rule:
    // RX: send RX_STREAM_OUT if enabled
    // TX: send TX_AUDIO_OUT only if stream_tx_enable && ptt==ON
    if (g.mode == MODE_RX && g.stream_rx_enable) {
        send_audio_frame(STREAM_ID_RX_STREAM_OUT, pcm, N);
    } else if (g.mode == MODE_TX && g.stream_tx_enable && g.ptt) {
        send_audio_frame(STREAM_ID_TX_AUDIO_OUT, pcm, N);
    } else {
        // standby: no audio frames
    }
}
