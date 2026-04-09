# FSO Gateway POC — High-Performance Packet Recovery System

## Overview

The FSO Gateway is a specialized bridge designed to connect standard Ethernet networks over Free Space Optical (FSO) links. FSO communication is highly susceptible to atmospheric turbulence, which causes deep fading and significant "burst" packet losses.

This system mitigates these issues by implementing a robust pipeline of:

- Fragmentation
- Matrix Interleaving
- Forward Error Correction (FEC) using Wirehair (Fountain Code)

The system operates strictly on a **Packet Erasure Channel** model, where packets are either received or lost — not bit-corrected.

---

## Project Status: April 2026

### Current Milestone

**Phase 7 — Real NIC Integration (complete, pending hardware validation)**

### System Status Summary

✔ End-to-End simulation pipeline fully validated (Phase 6)  
✔ FEC layer validated mathematically and empirically  
✔ Interleaving behavior validated under burst conditions  
✔ Statistical campaign framework: 69 scenarios, 458+ runs, zero regressions  
✔ CRC integrity layer fully integrated  
✔ Real NIC integration layer implemented (Phase 7)  
✔ TX pipeline module implemented and code-reviewed  
✔ RX pipeline module implemented and code-reviewed  
✔ Full-duplex gateway module implemented and code-reviewed  
⏳ Hardware validation pending (requires Linux machine with real NICs)

---

## Repository Structure

```
fso_gateway/
├── include/              # All header files
├── src/                  # All implementation files
├── tests/                # Automated simulation tests
├── tools/                # Manual test tools (require real NICs)
└── third_party/wirehair/ # Wirehair Fountain Code FEC library
```

---

## Completed Core Components

### 1. Infrastructure (Phases 0–1)
- Linux-based development environment
- Makefile (C + C++ integration for Wirehair)
- Logging module (multi-level, timestamped, thread-safe)
- AddressSanitizer (ASan) integration

### 2. Packet Processing & Fragmentation (Phase 2)
- Full support for small packets, MTU (1500B), and jumbo frames (9000B)
- Deterministic fragmentation into fixed-size symbols
- Exact byte-level reassembly validation

### 3. Block Management & FEC Wrapper (Phases 3–4)
- Block Builder (K symbols per block, timeout-based closure)
- Wirehair integration via clean wrapper
- Cold Buffer Fix (GF(256) correctness)

### 4. Matrix Interleaving (Phase 5)
- Column-based interleaving across blocks
- Burst spreading across time
- Sparse deinterleaver (hole-tolerant)

### 5. Receiver Robustness (Phase 5.1)
The receiver was redesigned as a strict deterministic state machine:
- Block lifecycle FSM: `EMPTY → FILLING → READY → EMPTY`
- Duplicate rejection (bitmap, O(1))
- Late symbol handling (drop after closure)
- Early failure detection (holes > M)
- Threshold + timeout flush policy

### 6. Channel Campaign Test Suite (Phase 6)
- 69 scenarios, 458+ total runs
- Scenario categories: Very Short Bursts, Single Burst Sweep, Boundary Testing, Time-Based Fades, Multi-Burst, Blackout, Corruption, Mixed, Failure Edge
- Erasure Recoverability Oracle for theoretical validation
- Result: `oracle_recoverable_but_not_decoded_successfully = 0` across all runs

### 6.1 CRC Integrity Layer (Phase 6 Extension)
- Per-symbol CRC-32C (Castagnoli) covering 14-byte header + payload
- Corruption → erasure conversion before FEC
- `crc_dropped_symbols` and `packet_fail_crc_drop` metrics
- Validated: no false decode successes, no oracle mismatches

### 7. Real NIC Integration (Phase 7) — NEW
See full details below.

---

## Phase 7 — Real NIC Integration

### Architecture

The gateway opens **four** libpcap handles and runs two pipelines in parallel threads:

```
NIC_LAN (promisc=1) ──→ TX pipeline ──→ NIC_FSO (promisc=0)
                         fragment → FEC encode → interleave → serialize

NIC_FSO (promisc=1) ──→ RX pipeline ──→ NIC_LAN (promisc=0)
                         deserialize → deinterleave → FEC decode → reassemble
```

### Wire Format

Every symbol is serialized as an 18-byte header followed by the payload:

| Offset | Size | Field         | Byte Order |
|--------|------|---------------|------------|
| 0      | 4    | packet_id     | big-endian |
| 4      | 4    | fec_id        | big-endian |
| 8      | 2    | symbol_index  | big-endian |
| 10     | 2    | total_symbols | big-endian |
| 12     | 2    | payload_len   | big-endian |
| 14     | 4    | crc32         | big-endian |
| 18     | N    | data[]        | —          |

### New Source Files

| File | Description |
|------|-------------|
| `include/packet_io.h` + `src/packet_io.c` | libpcap wrapper — non-blocking RX/TX, jumbo frame support (snaplen 9200) |
| `include/tx_pipeline.h` + `src/tx_pipeline.c` | TX pipeline: receive from LAN NIC → fragment → FEC encode → interleave → transmit to FSO NIC |
| `include/rx_pipeline.h` + `src/rx_pipeline.c` | RX pipeline: receive from FSO NIC → deserialize → deinterleave → FEC decode → reassemble → transmit to LAN NIC |
| `include/gateway.h` + `src/gateway.c` | Full-duplex gateway: runs TX and RX pipelines simultaneously in two pthreads |

### New Tool Files

| File | Description |
|------|-------------|
| `tools/packet_io_test.c` | Validates Task 22: send/receive raw frames on a real NIC |
| `tools/echo_test.c` | Task 23 tool: raw Ethernet forwarding between two NICs (no FEC) |
| `tools/echo_verify.c` | Validates Task 23: injects known frames and verifies they arrive |
| `tools/tx_pipeline_test.c` | Validates Task 24: confirms symbols appear on FSO NIC after TX pipeline |
| `tools/rx_pipeline_test.c` | Validates Task 25: confirms packets reconstructed on LAN NIC after RX pipeline |
| `tools/gateway_test.c` | Validates Task 26: full-duplex test with injector, observer, and timer threads |

---

## Build

### Prerequisites (Linux only)

```bash
sudo apt-get install libpcap-dev build-essential
```

### Build all

```bash
make clean && make
```

### Build with AddressSanitizer

```bash
make clean && make DEBUG=1
```

---

## Automated Tests (Simulation — no NIC required)

These run on any machine (including WSL/Windows).

```bash
# Full channel campaign (recommended)
make clean
make ctest DEBUG=1 > build/ctest_full.log 2>&1
tail -n 120 build/ctest_full.log

# Individual test suites
make itest    # interleaver
make dtest    # deinterleaver
make btest    # burst simulation
make rtest    # receiver robustness
make ftest    # FEC stress
make e2etest  # end-to-end simulation (Task 20)
make alltest  # all of the above
```

### Expected Campaign Results

```
blocks_decode_failed = 0
oracle_recoverable_but_not_decoded_successfully = 0
oracle_recoverable_but_discarded_before_decode = 0
suspicious_blocks_total = 0
longest_recoverable_burst = 64 symbols (76.80 µs)
first_failing_burst = 65 symbols (78.00 µs)
```

---

## Hardware Validation (Linux machine with real NICs required)

Run these in order. Each must pass before proceeding to the next.

> **All tools require `sudo` (libpcap needs root for raw socket access).**  
> Replace `eth0` / `eth1` with your actual interface names.

---

### Task 22 — Validate packet_io module

**TX test** (sends 10 broadcast frames):
```bash
sudo ./build/packet_io_test --iface eth0 --mode tx --count 10
```

**RX test** (waits to receive 10 frames, run in parallel terminal):
```bash
sudo ./build/packet_io_test --iface eth0 --mode rx --count 10
```

**Pass criterion:** both print `PASSED`.

---

### Task 23 — Validate echo forwarding (no FEC)

**Terminal 1 — start echo forwarder:**
```bash
sudo ./build/echo_test --lan-iface eth0 --fso-iface eth1 --duration 30
```

**Terminal 2 — RX observer:**
```bash
sudo ./build/echo_verify --iface eth1 --mode rx --count 20
```

**Terminal 3 — TX sender:**
```bash
sudo ./build/echo_verify --iface eth0 --mode tx --count 20
```

**Pass criterion:** RX observer prints `RX TEST PASSED`.

---

### Task 24 — Validate TX pipeline

**Terminal 1 — RX observer (start first):**
```bash
sudo ./build/tx_pipeline_test \
  --lan-iface eth0 --fso-iface eth1 \
  --mode rx --duration 15
```

**Terminal 2 — TX pipeline:**
```bash
sudo ./build/tx_pipeline_test \
  --lan-iface eth0 --fso-iface eth1 \
  --mode tx --duration 10
```

**Pass criterion:** RX observer prints `TX PIPELINE TEST PASSED`.

---

### Task 25 — Validate RX pipeline

**Terminal 1 — observer (start first):**
```bash
sudo ./build/rx_pipeline_test \
  --lan-iface eth0 --fso-iface eth1 \
  --mode obs --duration 20
```

**Terminal 2 — RX pipeline:**
```bash
sudo ./build/rx_pipeline_test \
  --lan-iface eth0 --fso-iface eth1 \
  --mode rx --duration 15
```

**Terminal 3 — TX pipeline:**
```bash
sudo ./build/rx_pipeline_test \
  --lan-iface eth0 --fso-iface eth1 \
  --mode tx --duration 10
```

**Pass criterion:** observer prints `RX PIPELINE TEST PASSED`.

---

### Task 26 — Validate full-duplex gateway

```bash
sudo ./build/gateway_test \
  --lan-iface eth0 \
  --fso-iface eth1 \
  --duration 20
```

**Pass criterion:** prints `FULL DUPLEX TEST PASSED`.

---

### Recommended FEC parameters for hardware testing

| Parameter | Value | Notes |
|-----------|-------|-------|
| `--k` | 8 | Source symbols per block |
| `--m` | 4 | Repair symbols (50% overhead) |
| `--depth` | 2 | Interleave depth |
| `--symbol-size` | 1500 | Bytes per symbol (MTU) |

Example with explicit parameters:
```bash
sudo ./build/gateway_test \
  --lan-iface eth0 --fso-iface eth1 \
  --k 8 --m 4 --depth 2 --symbol-size 1500 \
  --duration 30
```

---

## Design Notes

### Memory management
All large buffers (`source_buf`, `block_buf`, `recon_buf`, `pkt_syms_buf`, `frag_syms`, `repair_syms`) are heap-allocated to avoid stack overflow. `sizeof(block_t)` ≈ 2.3 MB and `sizeof(symbol_t[256])` ≈ 2.3 MB — both too large for stack frames.

### Threading model
`gateway_run()` creates two pthreads (TX and RX). Each loops its respective `*_pipeline_run_once()`. A fatal error in either thread sets `running = 0`, causing the other to exit cleanly. Signal handlers call `gateway_stop()` which sets the same flag.

### CRC
CRC-32C (Castagnoli) is computed on TX **after** FEC encode (so CRC covers the final transmitted payload). On RX, CRC is verified **before** the symbol enters the deinterleaver. Failing symbols are counted as erasures.

---

## Next Steps — Phase 8

- Task 27: Memory validation (Valgrind + AddressSanitizer under real traffic)
- Task 28: Load testing (sustained traffic, no crashes)
- Task 29: Packet size sweep (small, MTU, jumbo, mixed)
- Task 30: Real FSO link trial — baseline vs interleaving vs interleaving+FEC