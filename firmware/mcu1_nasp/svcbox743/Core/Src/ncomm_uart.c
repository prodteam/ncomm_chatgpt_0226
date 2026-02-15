#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "usart.h"   // huart3
#include "main.h"

extern uint16_t ncomm_crc16_ccitt_false(const uint8_t* data, size_t len);

#define SOF0 0xAA
#define SOF1 0x55

// Packet fields per UART_Protocol_Spec_v1.4.0
// SOF(2) VER(1) TYPE(1) FLAGS(1) SEQ(1) LEN(2 LE) PAYLOAD LEN CRC16(2 LE)

static uint8_t g_tx_seq = 0;

// Blocking send (OK for MVP). Later we can switch to DMA.
static inline void uart_send_bytes(const uint8_t* p, uint16_t n) {
    (void)HAL_UART_Transmit(&huart3, (uint8_t*)p, n, 100);
}

void ncomm_uart_send(uint8_t ver, uint8_t type, uint8_t flags, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[2 + 1 + 1 + 1 + 1 + 2]; // SOF+VER+TYPE+FLAGS+SEQ+LEN
    hdr[0] = SOF0;
    hdr[1] = SOF1;
    hdr[2] = ver;
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = g_tx_seq++;
    hdr[6] = (uint8_t)(len & 0xFF);
    hdr[7] = (uint8_t)((len >> 8) & 0xFF);

    // CRC over VER..PAYLOAD (hdr[2]..hdr[7] + payload)
    uint16_t crc = 0;
    {
        uint8_t tmp[1 + 1 + 1 + 1 + 2]; // VER TYPE FLAGS SEQ LEN(2)
        tmp[0] = hdr[2];
        tmp[1] = hdr[3];
        tmp[2] = hdr[4];
        tmp[3] = hdr[5];
        tmp[4] = hdr[6];
        tmp[5] = hdr[7];

        crc = ncomm_crc16_ccitt_false(tmp, sizeof(tmp));
        if (payload && len) {
            // crc “continue”: easiest is compute over concatenated
            // for MVP (small), just compute in two steps by feeding a buffer:
            // We'll do a simple concat into a static buffer bounded by payload sizes we use.
            // For safety: cap at 1024.
            uint8_t buf[6 + 1024];
            memcpy(buf, tmp, 6);
            uint16_t plen = (len > 1024) ? 1024 : len;
            memcpy(buf + 6, payload, plen);
            crc = ncomm_crc16_ccitt_false(buf, 6 + plen);
        }
    }

    uint8_t crc_le[2];
    crc_le[0] = (uint8_t)(crc & 0xFF);
    crc_le[1] = (uint8_t)((crc >> 8) & 0xFF);

    uart_send_bytes(hdr, sizeof(hdr));
    if (payload && len) uart_send_bytes(payload, len);
    uart_send_bytes(crc_le, 2);
}
