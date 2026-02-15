# NeuroComm UART Protocol Spec — v1.4.0 (MCU2 master, MCU1 slave)

> **Базовый документ:** UART Protocol Spec v1.0  
> **Обновлено:** 2025-02-07  
> **Связанные документы:**  
> - `01_2026_NC_architecture_v1.4.0.md` (архитектура)  
> - `NeuroComm_State_Diagrams_v1.4.0.md` (state diagrams)

---

## 0. Scope

**Аббревиатуры:**
- **VAD** — Voice Activity Detection
- **VE** — Voice Enhancement
- **SR** — Speaker Recognition
- **KWS** — Keyword Spotting (Sensory)
- **PTT** — Push-To-Talk

This protocol defines:
- control messages MCU2 → MCU1
- events + optional audio streaming MCU1 → MCU2
- framing, sequencing, resync rules
- behavior on reset / reconnect

Non-goals:
- MCU2 ↔ MCU3 protocol (отдельный документ)
- BLE/UI/control plane beyond MCU2
- SR/KWS algorithm definitions (MCU2-internal)
- Beep generation (MCU2-internal)

---

## 1. Physical / Link Layer

### 1.1 Physical
- Interface: UART (MCU1 ↔ MCU2)
- Current demo wiring: MCU1 pin14 RX, pin15 TX ↔ MCU2 pin14 RX, pin15 TX
- USART: MCU1 uses USART3 as-built
- Baud rate: **1 000 000**
- Format: 8N1
- Flow control: none

> ⚠️ **Внимание (Keln legacy):** В текущей реализации Keln baud rates назначены нелогично: MCU1↔MCU2 (USART3) = 500k, MCU2↔MCU3 (UART4) = 1M. В новой архитектуре приоритеты инвертированы: критичный аудио-линк MCU1↔MCU2 получает **1M**, сервисный MCU2↔MCU3 — **500k**. Это зафиксировано в архитектуре v1.3.5 раздел 7.1.

### 1.2 Timing ownership
- MCU2 is the time master
- MCU1 is a deterministic slave accelerator
- MCU2 configures MCU1 and drives mode changes

---

## 2. Audio Format (logical)

- Sample rate: **16 000 Hz**
- Sample format: **signed int16, little-endian**
- Channels from ADC into MCU1: 2ch (In1=MIC, In2=RADIO_SPK)
- Transport to MCU2:
  - Control/events always
  - Audio frames only when enabled by config and gating rules

---

## 3. VE Contract (hard requirement)

If VE is disabled by MCU2:
- MCU1 VE output MUST equal input (bypass)
- No mute
- No stale buffer
- No undefined behavior

> Соответствует архитектуре v1.3.5 раздел 3.4.

---

## 4. High-level Operating Modes

MCU2 sets MCU1 mode via CMD_SET_MODE with parameters.

### 4.1 RX
- Process In2 (RADIO_SPK) through VE if RX_VE_ENABLE=1, else bypass
- MCU1 may still compute VAD on In1 and send EVT_VAD
- MCU1 may stream AUDIO_RX_FRAME to MCU2

### 4.2 TX
- Gate TX audio strictly by PTT=ON
- TX audio source: MIC (In1) routed to VE if TX_VE_ENABLE=1, else bypass
- In2 is ignored/blocked for TX path
- MCU1 streams AUDIO_TX_FRAME only while PTT=ON

### 4.3 STANDBY
- Like RX from standpoint of "no TX"
- Audio streaming optional (implementation choice)
- VAD may continue (recommended)

### 4.4 Mode ownership vs. product state machine

> **ВАЖНО:** MCU1 не знает о режимах AI-VOX / AI-VOX PRO / AI-VOX PRO + SR. Для MCU1 существуют только RX / TX / STANDBY. Продуктовая state machine (connect/disconnect, KWS, SR) живёт целиком на MCU2. MCU2 транслирует решения продуктовой логики в CMD_SET_MODE(mode, ptt) для MCU1.
>
> Пример: AI-VOX PRO + SR
> 1. MCU2 получает EVT_VAD(1) → запускает KWS → детектирует cmd 1–3 → запускает SR → SR=true
> 2. MCU2 отправляет CMD_SET_MODE(mode=TX, ptt=ON) на MCU1
> 3. MCU1 начинает стримить AUDIO_TX_FRAME
> 4. MCU2 получает EVT_VAD(0) → отправляет CMD_SET_MODE(mode=TX, ptt=OFF)
> 5. KWS детектирует cmd 4 → MCU2 отправляет CMD_SET_MODE(mode=STANDBY)

---

## 5. Framing

### 5.1 Packet structure (binary)

All messages (control, events, audio) use one framing format:

| Field   | Size (bytes) | Notes |
|---------|--------------|-------|
| SOF     | 2 | 0xAA 0x55 |
| VER     | 1 | Protocol version (0x01) |
| TYPE    | 1 | Message type |
| FLAGS   | 1 | Bit flags (see 5.3) |
| SEQ     | 1 | Sequence number modulo 256 |
| LEN     | 2 | Payload length, little-endian |
| PAYLOAD | LEN | Payload bytes |
| CRC16   | 2 | CRC-16/CCITT-FALSE over VER..PAYLOAD, little-endian |

Total overhead per packet: **10 bytes** (SOF=2 + header=4 + LEN=2 + CRC=2).

### 5.2 CRC specification
- Polynomial: 0x1021
- Init: 0xFFFF
- XorOut: 0x0000
- CRC field: **little-endian**

### 5.3 FLAGS field

| Bit | Name | Description |
|-----|------|-------------|
| 0   | ACK_REQ | Response requested (for commands) |
| 1   | URGENT  | Priority message (reserved for future) |
| 2–7 | reserved | Must be 0 |

### 5.4 Resync rule
Receiver scans byte stream for SOF (0xAA 0x55).
If CRC fails:
- Increment error counter
- Resync by scanning for next SOF
- Do NOT reset connection

---

## 6. Message Types

### 6.1 MCU2 → MCU1 (Commands: 0x01–0x7F)

#### 0x01 CMD_PING
- Payload: empty
- Response: EVT_PONG

#### 0x02 CMD_GET_INFO
- Payload: empty
- Response: EVT_INFO

#### 0x10 CMD_SET_MODE
Payload (8 bytes):

| Field | Offset | Size | Values |
|-------|--------|------|--------|
| mode  | 0 | 1 | 0=STANDBY, 1=RX, 2=TX |
| ptt   | 1 | 1 | 0=OFF, 1=ON |
| rx_ve_enable | 2 | 1 | 0/1 |
| tx_ve_enable | 3 | 1 | 0/1 |
| kws_src | 4 | 1 | 0=MIC_RAW, 1=MIC_VE (informational for MCU1) |
| reserved | 5 | 3 | 0x00 |

Behavior:
- mode=TX, ptt=OFF ⇒ TX audio MUST NOT stream
- mode=RX or STANDBY ⇒ ptt ignored, TX audio MUST NOT stream

Response: EVT_MODE_ACK

#### 0x11 CMD_SET_STREAMS
Payload (8 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| stream_rx_enable | 0 | 1 | 0/1 — MCU1→MCU2 RX audio |
| stream_tx_enable | 1 | 1 | 0/1 — MCU1→MCU2 TX audio (gated by PTT) |
| vad_evt_enable   | 2 | 1 | 0/1 |
| frame_samples    | 3 | 2 | Samples per frame per channel (e.g. 256=16ms VAD chunk, 240=15ms Sensory brick) |
| reserved         | 5 | 3 | 0x00 |

> **Примечание:** Отдельный raw MIC поток не нужен. Во время TX (PTT=ON) KWS/VAD не работают. При PTT=OFF поток MIC определяется настройкой VE: если VE=off — поток является raw MIC (bypass контракт, архитектура v1.3.5 раздел 3.4). Если VE=on (VE_always_on) — KWS работает на VE-обработанном потоке. TX и KWS-мониторинг — взаимоисключающие состояния.

Response: EVT_STREAMS_ACK

#### 0x12 CMD_RESET_STATE
- Payload: empty
- Behavior: MCU1 clears all internal buffers (including VAD pre-roll), resets sequence counters, returns to STANDBY
- Response: EVT_RESET_ACK

#### 0x13 CMD_SET_VAD_CONFIG *(новый в v1.1)*
Payload (8 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| start_marker | 0 | 1 | Число чанков VAD=true для VAD Start (e.g. 3 or 4) |
| stop_marker  | 1 | 1 | Число чанков VAD=false для VAD Stop (e.g. 3) |
| chunk_ms     | 2 | 2 | Длина рабочего чанка VAD в мс (default: 16; VAD-сеть: 8 мс вход, анализ каждого 2-го) |
| preroll_ms   | 4 | 2 | Размер pre-roll буфера в мс (default: 160 = 10 чанков × 16 мс) |
| reserved     | 6 | 2 | 0x00 |

Response: EVT_VAD_CONFIG_ACK

> ⚠️ **Обновлено в v1.3.3:** VAD Start Marker = 4 и VAD Stop Marker = 3 — **единые для всех режимов**. Параметры задаются однократно при инициализации MCU1 и не меняются при переключении режимов на MCU2. Конкретные значения подбираются тестами и фиксируются в конфигурации.

---

### 6.2 MCU1 → MCU2 (Events / Audio: 0x80–0xFF)

#### 0x80 EVT_PONG
- Payload: empty

#### 0x81 EVT_INFO
Payload (16 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| proto_ver | 0 | 1 | Protocol version |
| fw_major  | 1 | 1 | |
| fw_minor  | 2 | 1 | |
| fw_patch  | 3 | 1 | |
| features  | 4 | 4 | Bitmask: bit0=VAD, bit1=VE, bit2..31 reserved |
| reserved  | 8 | 8 | 0x00 |

#### 0x82 EVT_MODE_ACK
Payload (8 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| mode | 0 | 1 | Applied mode |
| ptt  | 1 | 1 | Applied PTT |
| rx_ve_enable | 2 | 1 | Applied |
| tx_ve_enable | 3 | 1 | Applied |
| applied_flags | 4 | 1 | bit0=rx_ve_applied, bit1=tx_ve_applied |
| reserved | 5 | 3 | 0x00 |

#### 0x83 EVT_STREAMS_ACK *(уточнён в v1.1)*
Payload (8 bytes): mirrors CMD_SET_STREAMS with applied values.

#### 0x84 EVT_VAD
Payload (8 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| vad_flag    | 0 | 1 | 0=OFF, 1=ON |
| vad_conf    | 1 | 1 | Confidence 0–255 (optional, 0 if not available) |
| hangover_ms | 2 | 2 | Remaining hangover in ms (optional) |
| chunk_index | 4 | 4 | Monotonic chunk counter since last reset |

> ⚠️ **Добавлено в v1.1:** `chunk_index` — для корректной привязки VAD-событий к аудио-фреймам на стороне MCU2. Критично для KWS буферизации.

#### 0x85 EVT_RESET_ACK
- Payload: empty

#### 0x86 EVT_VAD_CONFIG_ACK *(новый в v1.1)*
Payload: mirrors CMD_SET_VAD_CONFIG with applied values.

#### 0x87 EVT_ERROR *(новый в v1.1)*
Payload (8 bytes):

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| error_code | 0 | 2 | See error codes table |
| context    | 2 | 2 | Message TYPE that caused error, or 0 |
| reserved   | 4 | 4 | 0x00 |

Error codes:

| Code | Name | Description |
|------|------|-------------|
| 0x0001 | ERR_CRC | CRC mismatch on received packet |
| 0x0002 | ERR_SEQ | Sequence discontinuity detected |
| 0x0003 | ERR_OVERFLOW | Internal buffer overflow |
| 0x0004 | ERR_UNKNOWN_CMD | Unknown command TYPE received |
| 0x0005 | ERR_INVALID_PARAM | Invalid parameter in command |

#### 0x90 AUDIO_RX_FRAME
Payload:

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| frame_index | 0 | 4 | Monotonic frame counter |
| samples     | 4 | 2 | N samples in this frame |
| pcm         | 6 | 2×N | int16 LE mono (processed/bypass In2) |

Conditions:
- Only if stream_rx_enable=1
- Only in RX/STANDBY mode

#### 0x91 AUDIO_TX_FRAME
Payload:

| Field | Offset | Size | Notes |
|-------|--------|------|-------|
| frame_index | 0 | 4 | Monotonic frame counter |
| samples     | 4 | 2 | N samples in this frame |
| pcm         | 6 | 2×N | int16 LE mono (MIC→VE or bypass) |

Conditions:
- Only if stream_tx_enable=1
- **AI-VOX PRO / PRO+SR (KWS фаза):** MCU1 передаёт AUDIO_TX_FRAME **только при VAD=ON** (аудио не передаётся при VAD=OFF — экономия UART bandwidth). Первый фрейм включает pre-roll (4 чанка = 64 мс из кольцевого буфера)
- **AI-VOX / активная сессия (PTT фаза):** Only while (mode=TX AND ptt=ON). Первый фрейм после PTT ON включает pre-roll
- При VAD=OFF → MCU1 прекращает передачу AUDIO_TX_FRAME, шлёт только EVT_VAD(flag=0)

---

## 7. Pre-roll / VAD chunk buffer (MCU1 constraint)

MCU1 is allowed:
- Only a bounded pre-roll buffer sized to VAD chunk length

Config parameters (set via CMD_SET_VAD_CONFIG or fixed in FW):
- VAD_CHUNK_MS (default: 16 ms; VAD-сеть: 8 мс вход, анализ каждого 2-го блока)
- PREROLL_MS (default: 160 ms = 10 чанков)
- Buffer size = 16000 (sample_rate) × PREROLL_MS / 1000 × 2 bytes = 5120 bytes

No other long buffering on MCU1 is permitted. All long buffers reside on MCU2 (архитектура v1.3.5 раздел 4.3).

### 7.2 KWS буфер на MCU2 (Sensory API, справочно)

MCU2 принимает аудио-фреймы от MCU1 и передаёт их на Sensory KWS engine. Параметры Sensory (из Keln кода):

| Параметр | Значение | Описание |
|----------|----------|----------|
| Sensory brick | 240 сэмплов = 15 мс | Порция на один вызов `SensoryProcessData()` |
| AUDIO_BUFFER_MS | 630 мс | Внутренний кольцевой буфер Sensory |
| AUDIO_BUFFER_LEN | 10080 сэмплов = ~20 кБ | Размер буфера в сэмплах |
| BACKOFF_MS | 270 мс | Ретроспективный просмотр для endpoint detection |
| InputBuffer MCU2 | 32000 сэмплов = 2 сек | Промежуточный кольцевой буфер (float) |
| KWS timeout | 5 сек (конфигурируемый) | Таймаут ожидания команды |

> **Перепаковка:** MCU1 отправляет фреймы по 256 сэмплов (16 мс, VAD chunk). MCU2 накапливает их в промежуточный `sensoryBuffer` и перепаковывает по 240 сэмплов (15 мс, Sensory brick). Это **не 1:1** — MCU2 обязан обеспечить непрерывную подачу brick'ов.

> **Жизненный цикл KWS буфера (KWS фаза):**
> 1. MCU1 шлёт AUDIO_TX_FRAME **только при VAD=ON** (+ pre-roll)
> 2. MCU2 пишет всё полученное в Sensory (перепаковка 256→240)
> 3. VAD=OFF → MCU1 прекращает передачу, MCU2 отдаёт Sensory остаток
> 4. Sensory анализирует: trigger найден → Session Active; trigger не найден → Sensory restart, буфер сброшен, ожидание следующего VAD=ON

---
## 8. Streams, simultaneity rules, bandwidth budget (MCU1↔MCU2)

### 8.1 Links and baudrates (as-built on STM32H7 demo)

- **MCU1↔MCU2 audio/control link:** **1,000,000 baud** (UART4 on `svcbox743`).
- **MCU2↔MCU3 service link:** **500,000 baud** (USART3 on `svcbox743`).

> Note: the rest of this section is written for **MCU1↔MCU2** only.

### 8.2 Canonical stream names (unified terminology)

| Canonical name | Direction | Description |
|---|---:|---|
| **RX_STREAM_OUT** | MCU1 → MCU2 | Speaker path for headset playback: `SPK` processed by VE if enabled, otherwise BYPASS. |
| **TX_AUDIO_OUT** | MCU1 → MCU2 | Mic path for radio transmit (gated by PTT). Source = MIC → (optional VE) → output. |
| **MIC_RAW** | MCU1 → MCU2 | Raw mic stream (bypass) for KWS/SR on MCU2. |
| **MIC_VE** | MCU1 → MCU2 | Mic stream after VE for KWS/SR on MCU2 (VE disabled = BYPASS, VE enabled = denoise). |

**Rule:** `MIC_RAW` and `MIC_VE` are **mutually exclusive** (KWS/SR source selector).

### 8.3 Frame cadence (bandwidth model)

Assumptions (default):
- PCM: **16 kHz, 16-bit, mono** per stream.
- UART is **8N1**, so effective payload throughput is ~**baud/10 bytes/s**.

Per mono stream payload:
- 16k samples/s × 2 bytes = **32,000 B/s** (≈ 31.25 KiB/s)

At 1,000,000 baud:
- Effective = ~100,000 B/s payload budget
- 2 simultaneous mono streams ≈ 64,000 B/s (OK, with headroom for framing + control/events)

> If you use fixed-size “audio frame” packets (recommended), choose a frame length aligned with VAD chunking:
> - VAD_CHUNK = **16 ms** → 256 samples → 512 bytes payload per stream per chunk.

### 8.4 Allowed simultaneous stream combinations (MCU1↔MCU2)

| Use-case / mode | RX_STREAM_OUT | TX_AUDIO_OUT | MIC_RAW | MIC_VE | Notes |
|---|:---:|:---:|:---:|:---:|---|
| **STANDBY (RX monitoring)** | ✅ | ❌ | optional | optional | MIC_* only if KWS/SR enabled. Choose exactly one of MIC_RAW/MIC_VE. |
| **SERVICE (KWS/SR active)** | optional | ❌ | ✅ or ❌ | ✅ or ❌ | Exactly one mic source. RX_STREAM_OUT optional (headset monitoring). |
| **TX (PTT=ON)** | ❌ (recommended) | ✅ | ❌ (recommended) | ❌ (recommended) | To avoid exceeding bandwidth, do not stream RX + TX simultaneously. |
| **AI_VOX (VAD drives PTT)** | optional | ✅ (while PTT=ON) | optional | optional | During active TX, same restrictions as TX apply. |
| **Debug / lab** | ✅ | ✅ | ❌ | ❌ | Not for production: 2 streams already heavy; add more only at higher baud. |

**Hard constraint (recommended for v1):**
- Do **NOT** allow **RX_STREAM_OUT + TX_AUDIO_OUT** simultaneously (treat as unsupported in protocol/state-machine).
- Allow at most **2 mono streams** at once on 1,000,000 baud (plus small control/event traffic).

### 8.5 Runtime negotiation

MCU2 must explicitly enable required streams via config command(s).
MCU1 should reject invalid combinations and reply with an error code (e.g. `ERR_UNSUPPORTED_STREAM_COMBO`).

## 9. Sequence Numbering

- Separate SEQ counters for MCU1→MCU2 and MCU2→MCU1
- SEQ increments per packet (modulo 256)
- Audio frames and events share the same counter per direction
- On CMD_RESET_STATE: both counters reset to 0

Discontinuity handling:
- Receiver logs gap, does NOT hard reset
- For audio: treat gap as lost frames, continue playback

---

## 10. Reconnect / Reset Behavior

On MCU1 reset (power-on or watchdog):
1. MCU1 starts in STANDBY, PTT=OFF, VE=bypass (VE contract holds)
2. MCU1 sends nothing until first command from MCU2
3. MCU2 must send initialization sequence:
   - CMD_PING (verify link)
   - CMD_GET_INFO (verify firmware)
   - CMD_SET_VAD_CONFIG (set VAD parameters for current mode)
   - CMD_SET_STREAMS (enable required streams)
   - CMD_SET_MODE (set desired mode)
4. All buffers cleared on both sides
5. Sequence counters reset to 0

---

## 11. Typical Message Sequences

### 11.1 Startup / Init
```
MCU2 → MCU1: CMD_PING
MCU1 → MCU2: EVT_PONG
MCU2 → MCU1: CMD_GET_INFO
MCU1 → MCU2: EVT_INFO(proto=1, fw=x.y.z, features=0x03)
MCU2 → MCU1: CMD_SET_VAD_CONFIG(start=4, stop=3, chunk=16, preroll=160)
MCU1 → MCU2: EVT_VAD_CONFIG_ACK
MCU2 → MCU1: CMD_SET_STREAMS(rx=0, tx=1, vad=1, frame=160)
MCU1 → MCU2: EVT_STREAMS_ACK
MCU2 → MCU1: CMD_SET_MODE(STANDBY, ptt=0, rx_ve=0, tx_ve=0)
MCU1 → MCU2: EVT_MODE_ACK
```

### 11.2 AI-VOX: Voice detected → TX → Voice stops
```
MCU1 → MCU2: EVT_VAD(flag=1, conf=200, chunk=1234)
  [MCU2 decides PTT ON based on product logic]
MCU2 → MCU1: CMD_SET_MODE(TX, ptt=1, tx_ve=1)
MCU1 → MCU2: EVT_MODE_ACK
MCU1 → MCU2: AUDIO_TX_FRAME(frame=0, samples=160, pcm=...) ← includes pre-roll
MCU1 → MCU2: AUDIO_TX_FRAME(frame=1, ...)
MCU1 → MCU2: AUDIO_TX_FRAME(frame=2, ...)
  ...
MCU1 → MCU2: EVT_VAD(flag=0, chunk=1280)
  [MCU2 decides PTT OFF]
MCU2 → MCU1: CMD_SET_MODE(TX, ptt=0)
MCU1 → MCU2: EVT_MODE_ACK
  [TX audio stops]
```

### 11.3 AI-VOX PRO: KWS cmd → session → disconnect
```
  [MCU2 has stream_tx=1, vad=1 enabled; mode=STANDBY]
  [MCU1 анализирует MIC, но НЕ шлёт аудио пока VAD=OFF]

  --- Фаза 1: Ожидание команды ---
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← pre-roll + realtime (только при VAD=ON)
MCU1 → MCU2: AUDIO_TX_FRAME(...)
  ...
  [MCU2 пишет в Sensory, KWS анализирует]
  [MCU2 KWS detects cmd 2 in audio buffer]
MCU1 → MCU2: EVT_VAD(flag=0)
  [MCU1 прекращает передачу аудио]
  [MCU2 отдаёт Sensory остаток, Sensory находит trigger]
  [MCU2: beep → CMD_DETECTED → MCU3]

  --- Фаза 2: Session Active (PTT OFF, ожидание VAD) ---
  [MCU2: Enable Output = ON, PTT = OFF]
  [MCU2: SESSION_ACTIVE → MCU3]
  [Человек молчит... Session Timeout timer = 30 сек]

  --- Фаза 3: Передача ---
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← pre-roll + realtime
MCU2 → MCU1: CMD_SET_MODE(TX, ptt=1, tx_ve=1)
MCU1 → MCU2: EVT_MODE_ACK
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← now MIC through VE
  ...
  [VAD stop → PTT OFF (пауза, сессия не закрывается)]
MCU1 → MCU2: EVT_VAD(flag=0)
  [MCU1 прекращает передачу аудио]
  [VAD start → PTT ON, MCU1 возобновляет передачу]
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...)
  ...

  --- Фаза 4: Disconnect ---
  [MCU2 KWS detects cmd 4 (disconnect)]
  [MCU2: beep → SESSION_CLOSED → MCU3]
MCU2 → MCU1: CMD_SET_MODE(STANDBY, ptt=0)
MCU1 → MCU2: EVT_MODE_ACK
```

### 11.4 AI-VOX PRO + SR: KWS cmd → SR verify → session
```
  [mode=STANDBY, VE=off, stream_tx=1, vad=1]

  --- Фаза 1: Ожидание команды ---
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← pre-roll + realtime (только при VAD=ON)
  ...
  [MCU2 пишет в Sensory, KWS анализирует]
  [MCU2 KWS detects cmd 1 in audio buffer]
MCU1 → MCU2: EVT_VAD(flag=0)
  [MCU1 прекращает передачу аудио]

  --- Фаза 1.5: SR верификация ---
  [MCU2: beep → CMD_DETECTED → MCU3]
  [MCU2 runs SR on KWS buffer → SR=true]
  [MCU2: beep → SR_CONFIRMED → MCU3]

  --- Фаза 2: Session Active (PTT OFF) ---
  [MCU2: Enable Output = ON, PTT = OFF, SR выключен]
  [MCU2: SESSION_ACTIVE → MCU3]
  [Человек молчит... Session Timeout timer = 30 сек]

  --- Фаза 3: Передача ---
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← pre-roll + realtime
MCU2 → MCU1: CMD_SET_MODE(TX, ptt=1, tx_ve=1)
MCU1 → MCU2: EVT_MODE_ACK
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← now MIC through VE
  ...
  [VAD stop → PTT OFF, MCU1 прекращает передачу]
  [VAD start → PTT ON, MCU1 возобновляет]
  ...

  --- Фаза 4: Disconnect ---
  [KWS detects cmd 4 between TX pauses]
  [MCU2: beep → SESSION_CLOSED → MCU3]
MCU2 → MCU1: CMD_SET_MODE(STANDBY, ptt=0)
MCU1 → MCU2: EVT_MODE_ACK
```

### 11.5 AI-VOX PRO + SR: SR rejected
```
  [mode=STANDBY, VE=off, stream_tx=1, vad=1]
MCU1 → MCU2: EVT_VAD(flag=1)
MCU1 → MCU2: AUDIO_TX_FRAME(...) ← pre-roll + realtime (только при VAD=ON)
  ...
MCU1 → MCU2: EVT_VAD(flag=0)
  [MCU1 прекращает передачу аудио]
  [MCU2 KWS detects cmd 3]
  [MCU2: beep → CMD_DETECTED → MCU3]
  [MCU2 runs SR on KWS buffer → SR=false]
  [MCU2: beep (reject tone) → SR_REJECTED → MCU3]
  [MCU2 does NOT activate session]
  [Sensory restart, буфер сброшен, ожидание следующего VAD=ON]
```

---

## 12. Message Type Summary

| Code | Name | Direction | Payload size |
|------|------|-----------|--------------|
| 0x01 | CMD_PING | MCU2→MCU1 | 0 |
| 0x02 | CMD_GET_INFO | MCU2→MCU1 | 0 |
| 0x10 | CMD_SET_MODE | MCU2→MCU1 | 8 |
| 0x11 | CMD_SET_STREAMS | MCU2→MCU1 | 8 |
| 0x12 | CMD_RESET_STATE | MCU2→MCU1 | 0 |
| 0x13 | CMD_SET_VAD_CONFIG | MCU2→MCU1 | 8 |
| 0x80 | EVT_PONG | MCU1→MCU2 | 0 |
| 0x81 | EVT_INFO | MCU1→MCU2 | 16 |
| 0x82 | EVT_MODE_ACK | MCU1→MCU2 | 8 |
| 0x83 | EVT_STREAMS_ACK | MCU1→MCU2 | 8 |
| 0x84 | EVT_VAD | MCU1→MCU2 | 8 |
| 0x85 | EVT_RESET_ACK | MCU1→MCU2 | 0 |
| 0x86 | EVT_VAD_CONFIG_ACK | MCU1→MCU2 | 8 |
| 0x87 | EVT_ERROR | MCU1→MCU2 | 8 |
| 0x90 | AUDIO_RX_FRAME | MCU1→MCU2 | 6+2N |
| 0x91 | AUDIO_TX_FRAME | MCU1→MCU2 | 6+2N |

---

## Changelog

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | — | Initial draft |
| 1.1 | 2025-02-07 | See review notes below |
| 1.3 | 2025-02-07 | Унификация версии в рамках документ-пакета. Убран AUDIO_MIC_RAW_FRAME (не нужен). Добавлены аббревиатуры. Исправлен пример startup sequence. Кросс-ссылки синхронизированы. |

---

## Appendix: Review Notes (v1.0 → v1.1)

### Что исправлено

<a id="review-001"></a>
**REVIEW-001: Baud rate — Keln legacy inversion**
- Keln as-built: MCU1↔MCU2 (USART3) = 500k, MCU2↔MCU3 (UART4) = 1M
- Это нелогично: критичный аудио-линк получил меньшую полосу, сервисный — большую
- Architecture v1.3.5: инвертировано. MCU1↔MCU2 = **1 000 000**, MCU2↔MCU3 = **500 000**
- **Действие:** Spec обновлён до 1M. Bandwidth budget пересчитан. При реализации — изменить BRR для USART3 на обоих MCU.

**REVIEW-002: Bandwidth budget at 500k was too tight**
- v1.0 note "50 kB/s" was for 500k baud
- At 1M baud: ~90 kB/s usable — значительно лучше
- **Действие:** Полный пересчёт bandwidth budget (раздел 8), добавлена таблица concurrent scenarios.

**REVIEW-003: KWS bypass stream — NOT needed**
- v1.0 had no mechanism to get raw MIC separately from VE-processed TX
- Initial v1.1 draft added AUDIO_MIC_RAW_FRAME (0x92)
- **Отменено:** TX (PTT=ON) и KWS-мониторинг — взаимоисключающие состояния. Когда идёт TX, MCU2 не слушает MIC для KWS. Когда PTT=OFF, VE bypass контракт (раздел 3) гарантирует что поток = raw MIC при VE=off. Если VE_always_on — KWS обучен на VE-обработанном потоке. Отдельный stream не нужен.

**REVIEW-004: Missing VAD configuration command**
- State diagrams v1.3.5: VAD Start Marker = 4 и VAD Stop Marker = 3 — единые для всех режимов (задаются однократно, не меняются)
- v1.0 had no way for MCU2 to change VAD parameters
- **Действие:** Добавлен CMD_SET_VAD_CONFIG (0x13) + EVT_VAD_CONFIG_ACK (0x86).

**REVIEW-005: Missing frame_index in audio frames**
- v1.0 audio payloads had only samples + pcm, no frame counter
- Without frame_index, MCU2 cannot correlate audio frames with EVT_VAD chunk_index
- **Действие:** Добавлен frame_index (uint32) в начало всех AUDIO_*_FRAME payloads.

**REVIEW-006: Missing chunk_index in EVT_VAD**
- v1.0 EVT_VAD had vad_flag + conf + hangover, no temporal reference
- MCU2 needs to know which audio frame triggered VAD for KWS buffer alignment
- **Действие:** Добавлен chunk_index (uint32) в EVT_VAD.

**REVIEW-007: EVT_ERROR was marked "future" but is needed**
- v1.0: "v1 допускает без него"
- For any production code, error reporting is necessary
- **Действие:** Добавлен EVT_ERROR (0x87) с error codes.

**REVIEW-008: EVT_STREAMS_ACK was mentioned but not defined**
- v1.0: CMD_SET_STREAMS says "Ack: EVT_STREAMS_ACK" but no type code or payload
- **Действие:** Assigned 0x83, payload mirrors CMD_SET_STREAMS.

**REVIEW-009: Missing typical message sequences**
- v1.0 had no examples of actual message flow
- **Действие:** Добавлен раздел 11 с sequences для всех режимов (startup, AI-VOX, AI-VOX PRO, AI-VOX PRO+SR, SR rejected).

**REVIEW-010: No explicit section 4.4 on mode ownership**
- v1.0 implied MCU1 doesn't know about product modes, but never stated it explicitly
- **Действие:** Добавлен раздел 4.4 с explicit statement и примером.

### Что требует верификации по Keln коду

| ID | Question | Where to look in Keln |
|----|----------|----------------------|
| KELN-001 | USART3 BRR = 500k (known wrong, must change to 1M). Verify APB clock to calculate correct BRR. | MCU1 + MCU2 USART init code |
| KELN-002 | Existing packet format — does Keln use SOF/CRC or something else? | UART RX/TX handler on both MCUs |
| KELN-003 | How does Keln currently stream audio? Frame size? With/without header? | Audio DMA → UART code path |
| KELN-004 | Does Keln have any existing command/event protocol? | Command parser on MCU1, event sender |
| KELN-005 | VAD chunk size in current Keln implementation | VAD config / buffer allocation |
| KELN-006 | Does Keln use DMA for UART? Single buffer or double buffer? | DMA init for USART3 |
| KELN-007 | How does Keln handle VE bypass currently? | VE module enable/disable path |
| KELN-008 | Pre-roll buffer implementation in Keln (ring buffer? size?) | Audio buffer management near VAD |
