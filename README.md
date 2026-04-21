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

**Phase 8 — Two-Machine End-to-End Hardware Validation (UDP throughput validated ✅)**

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
✔ Two-machine hardware validation: bidirectional ICMP ping confirmed working (Phase 8)  
✔ UDP throughput: 42.8 Mbits/sec at 50 Mbps offered load, 13% loss, jitter 1ms (Phase 8)  
⏳ TCP throughput test — under investigation  
⏳ Pipeline "cold start" warmup bug — under investigation

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
---

## Phase 7.5 — Hardware End-to-End Validation: Status & Open Problems (April 15, 2026)

### Goal

The goal of this phase is to **prove**, on real hardware, that two Linux machines can communicate through the FSO Gateway software — meaning that L3 traffic (e.g. ICMP ping) originating on machine A reaches machine B **exclusively** via the gateway pipeline. No direct L2 cable between the LAN interfaces should be present, otherwise it is impossible to prove that traffic traverses the gateway.

---

### Physical Setup

Two Linux machines:

| Machine | Hostname | LAN NIC | FSO NIC |
|---------|----------|---------|---------|
| A | c4net-13g | enp1s0f0np0 (08:c0:eb:62:34:98) | enp1s0f1np1 |
| B | c4net-10g5th | enp1s0f0np0 (08:c0:eb:62:34:50) | enp1s0f1np1 |

FSO link: direct Ethernet cable between enp1s0f1np1 on A and enp1s0f1np1 on B (simulating FSO). LAN interfaces are Mellanox/ConnectX NICs — they go to NO-CARRIER/DOWN when no cable is connected.

---

### What We Proved Works

1. Full simulation pipeline — zero decode failures across 458+ runs ✅
2. Gateway TX pipeline — captures, fragments, FEC-encodes, interleaves, sends over FSO ✅
3. Gateway RX pipeline — receives, deinterleaves, FEC-decodes (success=YES), calls pcap_sendpacket ✅
4. Full duplex — both directions show ATTEMPTING SEND and sent N bytes ✅
5. pcap_setdirection(PCAP_D_IN) on FSO RX handle works correctly ✅

---

### The Core Problem: Proving End-to-End with Ping

Despite the pipeline working correctly, we have **not yet achieved a successful ping reply**. The obstacle is a loop/routing problem at the network layer, not in the gateway code itself.

#### Attempt 1: Direct cable between LAN NICs

**Result:** Ping succeeds but with massive DUP! (30-40 duplicates per packet, growing every ~33ms).

**Root cause:** The direct LAN cable provides a second path (0.3ms) alongside the gateway path (33ms). The gateway runs in promiscuous mode, so it captures traffic that already arrived via the direct path and re-forwards it, creating an infinite loop. The existing MAC filter in tx_pipeline.c:281 (drop if src MAC == own LAN MAC) is insufficient to stop this.

**Attempted fix 1:** `pcap_setdirection(PCAP_D_IN)` on ctx_lan_rx — broke the TX pipeline entirely because locally-originated packets are egress from the kernel's perspective and are no longer captured.

**Attempted fix 2:** Remove the direct LAN cable — enp1s0f0np0 goes to NO-CARRIER/DOWN, pcap cannot function.

**Conclusion:** Cannot use the direct LAN cable setup for a clean end-to-end proof.

#### Attempt 2: veth pairs as virtual LAN interfaces

Create virtual Ethernet pairs (veth-a/veth-a-gw on A, veth-b/veth-b-gw on B). The gateway listens on the -gw side, the user pings from the non-gw side. No physical LAN cable needed.

- A: veth-a = 10.1.0.1/24, veth-a-gw = 10.1.0.254/24, gateway on --lan-iface veth-a or veth-a-gw
- B: veth-b = 10.1.0.2/24, veth-b-gw = 10.1.0.253/24, gateway on --lan-iface veth-b or veth-b-gw
- Static ARP entries manually configured on both sides

**Partial result:** Pipeline runs correctly. Both machines show ATTEMPTING SEND and sent N bytes in logs. tcpdump on veth-b of B confirms ICMP echo requests arrive with correct MAC addressing. ✅

**Remaining problem:** No ICMP reply is ever observed. The following sub-problems were encountered:

**a) MAC filter blocks locally-originated traffic:** When the gateway LAN interface is veth-a and the ping originates from veth-a, the src MAC matches the gateway's own MAC filter and the packet is silently dropped. Workaround: ping from veth-a-gw instead.

**b) pcap_sendpacket on veth does not deliver to peer:** When gateway LAN interface is veth-a-gw, pcap_inject sends the frame as egress on veth-a-gw — but this does NOT cause the frame to appear as ingress on the peer veth-a. This is a known Linux kernel limitation: pcap_inject on a veth interface goes into the TX path and is not looped back to the peer. Switching to --lan-iface veth-a partially resolves this (pcap_sendpacket on veth-a DOES deliver to veth-a-gw), but re-introduces the MAC filter problem.

**c) dst MAC addressing:** The ping from A must have dst MAC = MAC of veth-b (c6:f8:30:83:42:2f), not the MAC of enp1s0f0np0 of B. Required manual ip neigh replace entries pointing to the correct veth MAC.

**d) Reply never arrives:** Even with all the above resolved, ICMP echo requests are confirmed to arrive at veth-b on B (tcpdump verified), but no reply is ever seen on A. The exact failure point of the reply path has not been isolated.

---

### Current State (April 15, 2026)

- Gateway code is correct and the pipeline is fully functional.
- veth setup is configured on both machines with correct IPs, MACs, and static ARP entries.
- ICMP echo requests confirmed to arrive at veth-b on B via tcpdump.
- No ICMP reply observed anywhere.
- Gateway logs show sent N bytes in both directions for every ping interval.

---

### Recommended Next Steps

**Option A (cleanest for production):** Implement gateway LAN TX using AF_PACKET raw socket with SO_MARK + iptables rules to prevent the loop. This is how production transparent bridges are implemented on Linux and avoids all the veth complexity.

**Option B (quickest for validation):** Accept the direct LAN cable, but add a TTL-based or custom EtherType filter to the BPF filter on the LAN RX pcap handle, so the gateway only forwards "fresh" packets and not packets it already injected.

**Option C (simplest test):** Use a third machine connected to both A and B on their LAN interfaces as a real external traffic source, eliminating the self-injection problem entirely.

---

## Phase 8 — Two-Machine End-to-End Hardware Validation (April 2026)

### Goal

Prove bidirectional L2 transparency using two real Windows endpoints connected to the two gateway LAN ports, with the FSO link simulated by a direct cable between the gateway FSO NICs.

---

### Physical Setup

```
Win1 ──────── GW-A ══════════ GW-B ──────── Win2
(192.168.50.1)  LAN    FSO cable   LAN   (192.168.50.2)
```

| Machine | Role     | LAN NIC         | FSO NIC         | LAN IP         | LAN MAC               |
|---------|----------|-----------------|-----------------|----------------|-----------------------|
| GW-A    | Gateway  | enp1s0f0np0     | enp1s0f1np1     | —              | 08:c0:eb:62:34:98     |
| GW-B    | Gateway  | enp1s0f0np0     | enp1s0f1np1     | —              | 08:c0:eb:62:34:50     |
| Win1    | Endpoint | Ethernet (→GW-A)| —               | 192.168.50.1   | 90:2e:16:d6:96:ba     |
| Win2    | Endpoint | Ethernet (→GW-B)| —               | 192.168.50.2   | c4:ef:bb:5f:cd:5c     |

FSO link: direct Ethernet cable between `enp1s0f1np1` on GW-A and `enp1s0f1np1` on GW-B.  
Win1 connected to `enp1s0f0np0` on GW-A. Win2 connected to `enp1s0f0np0` on GW-B.

---

### Bugs Fixed in This Phase

#### Bug 1 — Self-loop on FSO RX handle (MAC corruption)

**Symptom:** Packets forwarded by the gateway were captured again by the FSO RX pcap handle (the gateway was reading its own transmitted symbols), causing garbage MACs to be injected into the pipeline and ultimately corrupting destination MACs seen by Win1.

**Fix in `gateway.c`:** After opening `ctx_fso_rx`, call `packet_io_ignore_outgoing()` (sets `PACKET_IGNORE_OUTGOING` socket option, Linux ≥ 4.20) so self-sent frames are never delivered to the FSO RX handle.

**Fix in `gateway.c`:** After opening `ctx_lan_rx`, call `packet_io_set_direction_in()` (sets `pcap_setdirection(PCAP_D_IN)`) so only ingress frames are captured on the LAN side, preventing the gateway's own injected LAN frames from re-entering the TX pipeline.

#### Bug 2 — Reassembly packet boundary detection

**Symptom:** `reassemble_packet: num_symbols=2 does not match expected total_symbols=1` — two consecutive single-fragment original packets that happened to land in the same FEC block (k=2) were incorrectly grouped together, causing reassembly failure and dropped packets.

**Root cause:** All symbols belonging to a single FEC block share the same `packet_id` (= block_id). The original code flushed the symbol accumulator only when `packet_id` changed, so when two distinct original packets both had the same `packet_id`, the boundary between them was not detected.

**Fix in `src/rx_pipeline.c` — `drain_ready_blocks()`:** Replaced the single `packet_id != cur_packet_id` flush condition with two conditions, either of which triggers a flush:

1. `symbol_index == 0` — the incoming symbol is the first fragment of a new original packet.
2. `pkt_sym_count >= total_symbols` of the first buffered symbol — all expected fragments for the current packet have been collected.

Checking both conditions covers all boundary cases:
- Two single-fragment packets in the same FEC block (condition 2 fires after first, condition 1 fires at start of second).
- A complete single-fragment packet followed by a continuation fragment (symbol_index > 0) of a different packet that spans blocks (condition 2 fires for the first; condition 1 would not fire for the continuation).

---

### Build

Always use `make runner` (not plain `make`) to build the gateway binary:

```bash
make clean && make runner
```

The output binary is `build/bin/fso_gw_runner`.  
(`make` alone builds only `fso_gateway`, which is an older single-machine test tool, **not** the full two-machine gateway.)

---

### Run Commands

Run the following on **GW-A** and **GW-B** simultaneously (in separate terminals or screens):

```bash
# GW-A
sudo ./build/bin/fso_gw_runner \
  --lan-iface enp1s0f0np0 \
  --fso-iface enp1s0f1np1 \
  --k 2 --m 1 --depth 2 --symbol-size 750

# GW-B  (identical command — same interface names on both machines)
sudo ./build/bin/fso_gw_runner \
  --lan-iface enp1s0f0np0 \
  --fso-iface enp1s0f1np1 \
  --k 2 --m 1 --depth 2 --symbol-size 750
```

Stop with **Ctrl+C**.

---

### ARP Reset (required after each gateway restart)

Previous gateway sessions with the MAC-corruption bug (now fixed) may have left corrupted ARP entries in the Windows ARP cache. Windows ARP TTL is ~10 minutes, so without a manual reset, pings will fail silently until the cache expires.

Run these commands on **Win2** (PowerShell / cmd as Administrator) after every gateway restart:

```cmd
netsh interface ip delete neighbors "Ethernet" 192.168.50.1
netsh interface ip add neighbors "Ethernet" 192.168.50.1 90-2e-16-d6-96-ba
```

Run these commands on **Win1** after every gateway restart:

```cmd
netsh interface ip delete neighbors "Ethernet" 192.168.50.2
netsh interface ip add neighbors "Ethernet" 192.168.50.2 c4-ef-bb-5f-cd-5c
```

> **Note:** This manual reset is only needed during the transition period from the old buggy binary. Once the deployment has been running with the fixed binary long enough for all Windows ARP entries to have been refreshed organically (one ARP request/reply cycle per endpoint), the reset will no longer be necessary.

---

### Verification

After starting both gateways and resetting ARP, run from **Win1**:

```cmd
ping 192.168.50.2 -t
```

And from **Win2**:

```cmd
ping 192.168.50.1 -t
```

Expected: continuous replies with ~1–5ms RTT over the simulated FSO cable.

### Current Status (April 2026)

✔ Bidirectional ICMP ping Win1↔Win2 confirmed working  
✔ Both TX and RX pipelines stable under continuous traffic  
✔ MAC corruption bug resolved  
✔ Reassembly packet boundary bug resolved  
⏳ iperf3 throughput validation pending
