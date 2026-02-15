# NeuroComm Speaker Recognition Pipeline Specification
Version: 1.4.0
Status: Frozen Architecture Baseline
Date: 2026-01

---

# 1. Scope

This document defines the complete Speaker Recognition (SR) pipeline implemented on MCU2 (External SoC).

SR is strictly NOT implemented on MCU1.

MCU1 responsibilities:
- VAD
- VE
- TX gating
- Short VAD buffers only

SR operates only on MCU2.

---

# 2. Audio Source for SR

SR input stream is configurable.

Two possible modes:

Mode A (Default):
    MIC_RAW → MCU2

Mode B (VE_ALWAYS_ON mode):
    MIC_VE → MCU2

Configuration controlled by:
    SR_INPUT_MODE register

Allowed values:
    0 = MIC_RAW
    1 = MIC_VE

Rationale:
- Allows evaluation of VE impact on SR accuracy
- VE improvements may increase recognition robustness

---

# 3. SR Pipeline Overview

SR runs only when:

- System mode requires identity verification
- VAD reports positive speech
- Audio chunk is available from MCU1

Pipeline:

1. VAD_TRUE received
2. Audio frames buffered on MCU2
3. Feature extraction
4. Embedding generation
5. Similarity scoring
6. Decision (ACCEPT / REJECT)

---

# 4. Buffer Ownership Rules

MCU1:
- May only keep VAD chunk buffer (16 ms)
- No long-term audio memory

MCU2:
- Owns all SR buffers
- Owns rolling speech window
- Owns enrollment samples
- Owns embedding database

No SR state stored on MCU1.

---

# 5. Audio Format

From MCU1 via UART:

- Sample rate: 16 kHz
- Bit depth: 16-bit signed
- Mono
- Little endian

Frames aligned with VAD_CHUNK (16 ms = 256 samples)

---

# 6. Speech Buffering Strategy (MCU2)

When VAD_TRUE:

- Start collecting frames
- Maintain rolling speech buffer

Recommended limits:

- Minimum speech duration: 500 ms
- Maximum speech duration: 3000 ms
- Silence timeout: 400 ms

Buffer type:
    Circular buffer in RAM

No buffering on MCU1.

---

# 7. Feature Extraction

Feature type (Taiwan baseline compatible):

- Log-Mel Spectrogram
- 25 ms window
- 10 ms stride
- 40 mel bins (configurable)

Preprocessing:
- Pre-emphasis (optional)
- Energy normalization
- CMVN (configurable)

---

# 8. Embedding Model

Model type:
    Lightweight speaker embedding network

Execution:
    Runs entirely on MCU2

Constraints:
- Must fit in available RAM
- Inference < 100 ms typical

Output:
    Fixed-length embedding vector (e.g. 128 or 192 dimensions)

---

# 9. Enrollment Procedure

Enrollment steps:

1. Enter ENROLL mode
2. Collect N utterances (default: 3)
3. Generate embedding per utterance
4. Average embeddings
5. Store final template

Storage:
    Non-volatile memory on MCU2

Configurable:
    ENROLL_COUNT
    ENROLL_MIN_DURATION

---

# 10. Verification Procedure

On verification request:

1. Collect speech window
2. Generate embedding
3. Compute similarity score against stored template

Similarity metric:
    Cosine similarity

Decision threshold:
    SR_THRESHOLD (configurable register)

Output:
    SR_ACCEPT
    SR_REJECT

---

# 11. Decision Timing

Total latency target:

- VAD detection: ≤ 32 ms
- Buffer accumulation: 500–1500 ms
- Feature extraction: < 50 ms
- Embedding inference: < 100 ms
- Similarity computation: < 5 ms

Target total:
    < 1.8 s worst case
    < 1.2 s typical

---

# 12. Interaction With System Modes

## AI VOX Mode

- VAD triggers TX
- SR may be used as identity filter before enabling TX

Optional policy:
    TX only if SR_ACCEPT

## PRO Mode

- SR required before executing commands
- PTT controlled by MCU2

---

# 13. Registers and Configuration

SR configurable registers:

- SR_ENABLE (bool)
- SR_INPUT_MODE (0=MIC_RAW, 1=MIC_VE)
- SR_THRESHOLD (float)
- SR_MIN_DURATION_MS
- SR_MAX_DURATION_MS
- SR_SILENCE_TIMEOUT_MS
- ENROLL_COUNT
- ENROLL_MIN_DURATION_MS

---

# 14. Memory Constraints

SR memory usage target:

- Feature buffer: < 100 KB
- Model weights: target < 1 MB
- Embedding DB: < 20 KB

Must fit target low-power STM variant.

---

# 15. Error Handling

On:

- Insufficient speech length → SR_ABORT
- Model inference error → SR_ERROR
- No enrollment data → SR_NOT_ENROLLED

All errors reported via UART to host.

---

# 16. Reset Behavior

On system RESET:

- Clear rolling buffers
- Keep enrolled templates (unless full wipe)
- Reset SR state machine

---

# 17. Future Optimization (Product Version)

Planned improvements:

- Quantized embedding model (INT8)
- Fixed-point mel extraction
- Hardware DSP acceleration
- Reduced embedding dimension

Goal:
    Fit into low-power STM for product version.

---

# 18. Architectural Guarantees

- SR never blocks MCU1
- SR never modifies TX_AUDIO_OUT directly
- SR decisions only influence MCU2 state machine
- No hidden coupling to VE logic

This ensures MCU1 remains NASP-compatible.
