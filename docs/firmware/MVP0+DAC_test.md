# MCU2 Audio DAC Test Guide (MVP0+)

## Overview

This build enables real-time audio monitoring on **MCU2** via external DAC (PCM5102A over I2S2, 16 kHz).  
Audio source can be switched between:

- **RX stream** (received audio)
- **MIC/TX stream** (microphone / transmit path)

Monitoring and diagnostics are performed **only via UART4 logs** (no oscilloscope required).

---

# Hardware / Interface Setup

1. Flash updated firmware to:
   - MCU1
   - MCU2

2. Connect terminal to:


3. Power the system.

You should see:

MCU2 MVP0+ DAC: init ok
Commands: m=monitor MIC(TX), r=monitor RX, h=?


---

# Log Output (1 Hz)

Every second MCU2 prints status:

Example:



t=12345 rxB=24000 ok=120 crcBad=0 tx=6 pong=3 vad=1 aRx=50 aTx=0 fifo=820 und=0 ovf=0


### Relevant Audio Fields

| Field | Meaning |
|--------|----------|
| `aRx` | Number of RX audio frames received |
| `aTx` | Number of TX/MIC audio frames received |
| `fifo` | Current audio FIFO fill level (samples) |
| `und` | Underrun counter (DAC needed data but FIFO empty) |
| `ovf` | FIFO overflow counter (audio dropped) |

---

# Runtime Commands (via UART4)

Send single characters in terminal:

| Command | Action |
|----------|--------|
| `r` | Monitor **RX** stream |
| `m` | Monitor **MIC/TX** stream |
| `h` or `?` | Show help |

---

# Expected Behavior

## Listening to RX

Press:



r


Expected:

- `aRx` increases continuously
- `fifo` stabilizes at a non-zero value
- `und ≈ 0`
- `ovf ≈ 0`
- Audio audible from DAC

---

## Listening to MIC / TX

Press:



m


Expected:

- `aTx` increases continuously
- `fifo` stabilizes
- `und ≈ 0`
- `ovf ≈ 0`
- Microphone audio audible

---

# Correct Operation Indicators

✔ `aRx` or `aTx` steadily increasing  
✔ `fifo` fluctuates around mid-range  
✔ `und` stays near zero  
✔ `ovf` stays near zero  
✔ Clean audio output  

---

# Fault Diagnosis (Interface-Only)

## Case 1: No Audio, Counters Not Increasing

- `aRx = 0` and `aTx = 0`
- Likely MCU1 not sending audio
- Check stream mode / protocol

---

## Case 2: Audio Counters Increase but No Sound

- `aRx/aTx` increases
- `fifo` = 0 most of the time
- `und` increases constantly

Possible causes:
- Sample rate mismatch
- I2S not started correctly
- FIFO too small
- Audio stream not continuous

---

## Case 3: Overflow Growing

- `ovf` increases
- `fifo` near maximum

Meaning:
- Audio arriving faster than consumed
- Sample rate mismatch
- Incorrect frame size assumption

---

# Stability Target for MVP0+

Healthy system example:



aRx=80 fifo=700-1200 und=0 ovf=0


or



aTx=75 fifo=600-1100 und=0 ovf=0


---

# Notes

- Audio format: 16-bit PCM mono
- Output: I2S2 → PCM5102A
- Sample rate: 16 kHz
- I2S data format: 32-bit slot (16-bit sample left-shifted)

---

# Summary

This test validates:

- UART protocol integrity
- Audio frame parsing
- PCM extraction
- FIFO buffering
- I2S transmission stability
- External DAC operation

All without oscilloscope — using only interface-level diagnostics.


Если во время тестов появится что-то нестандартное (например fifo уходит в 0 или в максимум), скинь лог строки — разберём глубоко по таймингу и протоколу.

выведи плз в единый файл *.md
# Neurocomm – MCU2 Audio DAC Test Guide (MVP0+)

## Overview

This firmware build enables real-time audio monitoring on **MCU2** via the external DAC **PCM5102A** connected over **I2S2 (16 kHz)**.

MCU2 can monitor and output:

- **RX stream** (received audio path)
- **MIC / TX stream** (microphone / transmit path)

All validation is performed using **UART4 logs only** (no oscilloscope required).

---

# Hardware / Interface Setup

1. Flash updated firmware to:
   - MCU1
   - MCU2

2. Connect terminal to:



UART4
Baudrate: 500000
8N1


3. Power the system.

On boot you should see:



MCU2 MVP0+ DAC: init ok
Commands: m=monitor MIC(TX), r=monitor RX, h=?


---

# Runtime Logs (1 Hz)

MCU2 prints one status line per second.

Example:



t=12345 rxB=24000 ok=120 crcBad=0 tx=6 pong=3 vad=1 aRx=50 aTx=0 fifo=820 und=0 ovf=0


---

## Field Description

| Field | Meaning |
|--------|----------|
| `t` | System time (ms) |
| `rxB` | Total UART bytes received from MCU1 |
| `ok` | Valid protocol frames parsed |
| `crcBad` | Frames rejected due to CRC mismatch |
| `tx` | Frames sent from MCU2 to MCU1 |
| `pong` | PONG responses received |
| `vad` | VAD events received |
| `aRx` | RX audio frames received |
| `aTx` | TX/MIC audio frames received |
| `fifo` | Current audio FIFO fill level (samples) |
| `und` | DAC underrun counter |
| `ovf` | FIFO overflow counter |

---

# Runtime Commands (UART4)

Send single-character commands:

| Command | Function |
|----------|-----------|
| `r` | Monitor **RX** audio stream |
| `m` | Monitor **MIC/TX** audio stream |
| `h` or `?` | Show help |

---

# Test Procedure (Interface Only)

## Step 1 – Verify Link Alive

After boot:

- `rxB` should increase
- `ok` should increase
- `crcBad` should remain 0 or near 0

If not:
- UART configuration mismatch
- Protocol framing issue
- MCU1 not transmitting

---

## Step 2 – Monitor RX Audio

Send:



r


Expected:

- `aRx` increases steadily
- `fifo` stabilizes at a non-zero value
- `und ≈ 0`
- `ovf ≈ 0`
- Audio audible from DAC output

---

## Step 3 – Monitor MIC/TX Audio

Send:



m


Expected:

- `aTx` increases steadily
- `fifo` stabilizes
- `und ≈ 0`
- `ovf ≈ 0`
- Microphone audio audible from DAC

---

# Correct Operation Indicators

✔ Audio frame counters increase continuously  
✔ FIFO level remains stable (not stuck at 0 or max)  
✔ Underrun counter stays near zero  
✔ Overflow counter stays near zero  
✔ Clean, stable audio output  

---

# Fault Diagnosis (Interface-Level Only)

## Case A – No Audio Frames

- `aRx = 0` and `aTx = 0`
- MCU1 not sending audio
- Stream mode not enabled
- Protocol mismatch

---

## Case B – Frames Increase but No Sound

- `aRx` or `aTx` increases
- `fifo` near 0
- `und` increases continuously

Possible causes:
- Sample rate mismatch
- I2S not started correctly
- Frame size mismatch
- FIFO too small

---

## Case C – Overflow Growing

- `ovf` increases
- `fifo` near maximum

Meaning:
- Audio produced faster than consumed
- Incorrect sample rate configuration
- Wrong PCM format assumption

---

# Stability Reference (Healthy System Example)

RX monitoring:



aRx=80 fifo=700-1200 und=0 ovf=0


MIC monitoring:



aTx=75 fifo=600-1100 und=0 ovf=0


---

# Audio Path Technical Parameters

- Format: 16-bit PCM mono
- Sample Rate: 16 kHz
- Output Interface: I2S2
- DAC: PCM5102A
- I2S Slot Width: 32-bit
- PCM Mapping: 16-bit sample left-shifted into 32-bit slot
- Buffering: FIFO + ping-pong interrupt-based I2S transmission

---

# What This Validates

This test confirms:

- UART protocol stability
- Frame parsing and CRC validation
- Audio frame extraction
- FIFO buffering correctness
- I2S timing stability
- PCM5102A output operation
- End-to-end MCU1 → MCU2 → DAC audio path

All verification is done via interface-level logs only.

---

# MVP0+ Completion Criteria

The system is considered stable when:

- No persistent underruns
- No growing overflow
- Audio remains continuous
- No CRC errors accumulating
- Link counters behave as expected

---

End of Document
