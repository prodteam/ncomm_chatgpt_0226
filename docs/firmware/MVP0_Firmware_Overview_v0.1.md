Summary для текущей прошивки (MVP-0).
`MVP0_Firmware_Overview_v0.1.md`  в `/docs/firmware/`.

---

# NeuroComm Firmware – MVP-0 Overview

Version: 0.1
Status: Internal Validation Build

---

# 1. Firmware Architecture Overview

## System Partitioning (MVP-0)

### MCU1 (NASP Emulator – STM32H743)

Responsibilities in MVP-0:

* ADC 2-channel audio acquisition

  * CH1 → `MIC_RAW`
  * CH2 → `RX_RADIO_IN`
* VAD stub (energy threshold + N consecutive positives)
* Stream selection (single active stream at a time):

  * `STREAM_MIC_RAW`
  * `STREAM_RX_RAW`
* UART3 @ 1,000,000 baud communication to MCU2
* Transmission of:

  * `AUDIO_FRAME`
  * `VAD_STATUS`

Not included in MVP-0:

* VE (Voice Enhancement)
* KWS (Keyword Spotting)
* SR (Speaker Recognition)
* Compression
* DMA optimization

---

### MCU2 (SoC – STM32H743)

Responsibilities in MVP-0:

* UART3 @ 1M communication with MCU1
* UART Protocol v1.4.0 parser
* Command transmission to MCU1:

  * `SET_STREAM`
  * `PING`
* Receive and parse:

  * `AUDIO_FRAME`
  * `VAD_STATUS`
* Basic logging and state verification
* UART4 @ 500k link to MCU3 (bridge/UI)

Not included in MVP-0:

* I2S → DAC playback
* SR pipeline
* KWS
* Audio mixing
* Stream multiplexing

---

### MCU3 (Bridge/UI – STM32F407)

Responsibilities:

* UI display
* Logging
* Basic diagnostics
* Configuration interface

---

# 2. Audio Stream Model (MVP-0)

Only **one audio stream** is transmitted from MCU1 at a time.

This is required to stay within UART 1M bandwidth constraints.

## Available Streams

| Stream ID      | Source  | Description          |
| -------------- | ------- | -------------------- |
| STREAM_MIC_RAW | ADC CH1 | Microphone raw audio |
| STREAM_RX_RAW  | ADC CH2 | Radio speaker audio  |

---

## Data Flow Example (MIC_RAW Mode)

```
Headset MIC → ADC → MCU1
             → AUDIO_FRAME(STREAM_MIC_RAW)
             → UART3
             → MCU2 (parsed)
             → [future: I2S DAC → Radio mic]
```

---

# 3. UART Integration

## Physical Link

MCU1 ↔ MCU2

* Interface: USART3
* Baud rate: 1,000,000
* 8N1
* No DMA (MVP-0)
* No hardware flow control

Electrical wiring:

MCU1_TX → MCU2_RX
MCU1_RX ← MCU2_TX

(Validated via previous Keln firmware operation)

---

## Protocol (UART Protocol v1.4.0 – Simplified MVP subset)

Frame structure:

```
SOF:      0xAA 0x55
HEADER:   msg_type + length
PAYLOAD:  variable
CRC16:    trailing 2 bytes
```

### MCU2 → MCU1 Commands

* `SET_STREAM(stream_id)`
* `PING`

### MCU1 → MCU2 Messages

* `AUDIO_FRAME(stream_id, pcm_chunk)`
* `VAD_STATUS(count, vad_flag)`
* `PONG`

---

## Bandwidth Constraint

16kHz × 16bit mono ≈ 256 kbps raw.

Single stream fits safely inside 1M baud.
Dual simultaneous streams would exceed safe margin without compression.

MVP-0 enforces single-stream policy.

---

# 4. VAD Stub Behavior (MCU1)

## Parameters

* Sample rate: 16 kHz
* Chunk: 16 ms
* Threshold-based energy detection
* Configurable positive count
* Default: 3 consecutive positives

## Output

MCU1 transmits:

```
VAD_STATUS {
    count: uint8
    vad_flag: bool
}
```

---

# 5. Known Limitations (MVP-0)

### 1. No VE

No Voice Enhancement path exists yet.

### 2. No KWS / SR

Recognition pipeline not implemented.

### 3. No DMA

All UART and audio handling is interrupt or polling based.

### 4. Single Stream Only

Cannot send both MIC and RX simultaneously.

### 5. No Audio Playback on MCU2

Audio frames are parsed but not sent to DAC yet.

### 6. No Compression

UART bandwidth fully raw PCM.

---

# 6. Firmware Test Plan / Validation Procedure

## Stage 1 — UART Link Validation

### Objective:

Verify MCU1 ↔ MCU2 communication stability.

### Procedure:

1. Power both boards.
2. Send `PING` from MCU2.
3. Confirm `PONG` response.
4. Observe no CRC errors in logs.

Expected result:

* Stable bidirectional communication.
* No framing errors.

---

## Stage 2 — Stream Switching

### Objective:

Verify `SET_STREAM` command works.

### Procedure:

1. Send `SET_STREAM(STREAM_MIC_RAW)`.
2. Observe AUDIO_FRAME messages with correct stream_id.
3. Send `SET_STREAM(STREAM_RX_RAW)`.
4. Confirm stream_id changes.

Expected result:

* Immediate stream switch.
* No protocol corruption.

---

## Stage 3 — VAD Stub Validation

### Objective:

Verify VAD detection logic.

### Procedure:

1. Enable `STREAM_MIC_RAW`.
2. Speak into microphone.
3. Observe `VAD_STATUS` transitions.
4. Stay silent and verify VAD reset.

Expected result:

* VAD true only after configured N positives.
* VAD false after silence.

---

## Stage 4 — Bandwidth Stress Test

### Objective:

Verify 1M UART stability.

### Procedure:

1. Run continuous stream for 10–15 minutes.
2. Monitor:

   * CRC errors
   * UART overruns
   * Frame drops

Expected result:

* No desync
* No persistent CRC faults

---

## Stage 5 — UI Bridge Validation (MCU3)

### Objective:

Verify MCU2 → MCU3 bridge link.

### Procedure:

1. Observe forwarded VAD logs.
2. Confirm no UART4 congestion.

---

# 7. Exit Criteria for MVP-0

Firmware MVP-0 is considered stable when:

* UART stable at 1M for 30+ minutes
* Stream switching reliable
* VAD behavior predictable
* No hard faults or lockups
* No memory corruption

---

# Next Milestone (MVP-1)

* Add I2S → DAC playback on MCU2
* Add VE block stub
* Introduce dual-stream strategy with gating
* Evaluate DMA integration

---
