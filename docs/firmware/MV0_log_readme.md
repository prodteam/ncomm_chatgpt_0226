## MCU2 MVP0 Logs (UART4 @ 500000)

### Transport
MCU2 prints a single status line once per second to **UART4 (500k)**.  
This is intended as the primary “interface-only” validation method for MVP0 (no oscilloscope).

### Log line format
Example:

`t=12345 rxB=12000 ok=52 crcBad=0 tx=6 pong=3 vad=1 aRx=0 aTx=0 ackM=0 ackS=0 ackV=0 err=0`

Fields:
- `t` — HAL_GetTick() in ms
- `rxB` — total received bytes from MCU1 link (USART3)
- `ok` — number of successfully parsed frames (CRC OK)
- `crcBad` — frames dropped due to CRC mismatch
- `tx` — frames transmitted by MCU2 to MCU1 (CMD_PING / CMD_SET_* etc.)
- `pong` — received `EVT_PONG` counter (proof of alive link response)
- `vad` — received `EVT_VAD` counter (voice activity events)
- `aRx` — received `EVT_RX_AUDIO_FRAME` counter (audio frames from RX stream)
- `aTx` — received `EVT_TX_AUDIO_FRAME` counter (audio frames from TX stream)
- `ackM` — `EVT_MODE_ACK` counter
- `ackS` — `EVT_STREAMS_ACK` counter
- `ackV` — `EVT_VAD_CFG_ACK` counter
- `err` — `EVT_ERROR` counter

### MVP0 “Link Alive” criteria
Minimum:
1. `rxB` increases steadily after boot
2. `ok` increases (frames are parsed)
3. `crcBad` stays at 0 (or extremely rare)
4. `pong` increases if MCU2 sends periodic pings (2s period)

Optional (if MCU1 supports):
- `vad` increments when speaking into microphone
- `aRx`/`aTx` increment when a stream is enabled and audio frames are produced

### Failure patterns
- `rxB` = 0 always: wrong UART/pins/baud or RX callback not firing
- `rxB` increases but `ok` = 0 and `crcBad` grows: protocol framing mismatch / wrong baud / wrong link
- `ok` increases but `pong` never increases: MCU1 does not respond to ping (logic missing or disabled)
