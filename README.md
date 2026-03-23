# FSO Gateway POC - High-Performance Packet Recovery System

## Overview
The FSO Gateway is a specialized bridge designed to connect standard Ethernet networks over Free Space Optical (FSO) links. FSO communication is highly susceptible to atmospheric turbulence, which causes deep fading and significant "burst" packet losses.

This system mitigates these issues by implementing a robust pipeline of Fragmentation, Matrix Interleaving, and Forward Error Correction (FEC) using the Wirehair (Fountain Code) library.

---

## Project Status: March 2026
**Current Milestone:** Phase 5.1 (Receiver Robustness & State Machine) - **COMPLETED**.

The system is now production-ready at the logic level, featuring a deterministic state machine for block management and high resilience to jitter and out-of-order delivery.

---

## Completed Core Components

### 1. Infrastructure (Phases 0-1)
* **Linux Environment:** Isolated development environment.
* **Modern Build System:** Makefile supporting C/C++ with automatic dependency tracking.
* **Advanced Logging:** Multi-level logging system with CLI configuration.
* **Memory Safety:** Full integration with AddressSanitizer (ASan) for debugging.

### 2. Packet Processing & Fragmentation (Phase 2)
* Implements logic to break down large Ethernet frames (including 9000B Jumbo Frames) into fixed-size symbols.
* Ensures bit-exact reassembly at the receiver side.

### 3. Block Management & FEC Wrapper (Phases 3-4)
* **Block Builder:** Groups symbols into blocks of size $K$. Includes timeout logic for flushing partial blocks.
* **Wirehair Integration:** A professional C wrapper for the Wirehair library.
* **The "Cold Buffer" Fix:** Resolved a critical mathematical bug in GF(256) by implementing full-reconstruction semantics and strictly aligned memory buffers.

### 4. Matrix Interleaving & Burst Resilience (Phase 5)
* **Matrix Interleaver:** Disperses symbols from the same FEC block across the time domain.
* **Sparse Deinterleaver:** Assembles blocks from an interleaved stream, capable of handling "holes" (lost packets) without state corruption.

### 5. Receiver Robustness (Phase 5.1) - **NEW**
This sub-phase transformed the deinterleaver from a passive buffer into an active, robust state machine:

* **Block Lifecycle FSM:** Every block slot follows a strict `EMPTY` → `FILLING` → `READY` → `EMPTY` cycle.
* **Quiet Period (Stabilization):** Implemented a "Wait-for-Jitter" mechanism. Even if $K$ symbols are reached, the system waits (default 5ms) for late source symbols to arrive, reducing CPU load.
* **The Freeze Rule:** Once a block is marked `READY`, it is locked. Late-arriving symbols are dropped to prevent Race Conditions during FEC decoding.
* **Early Failure Detection:** Algorithms check if `holes > M` before attempting FEC, saving CPU cycles on irrecoverable blocks.
* **O(1) Duplicate Rejection:** Bitmap-based tracking prevents redundant symbol processing.

---

## Performance & Robustness Verification
The system was validated using two specialized stress test suites:

### A. The Burst Stress Test (Task 19)
* **Burst Length Sweep:** Verified 100% recovery for burst lengths up to **3,200** consecutive packets ($D=100, M=32$).
* **Exact Boundary Testing:** Mathematically verified recovery up to exactly $M$ losses and graceful failure at $M+1$.
* **Result:** 20/20 Success Rate.

### B. The Receiver Robustness Suite (Phase 5.1)
* **72 Unit Tests:** Covering FSM transitions, slot eviction under pressure, late arrival handling, and Wirehair integration.
* **Result:** 72/72 Success Rate.

---

## Build & Run Instructions

### 1. Running the Receiver Robustness Test (FSM Logic)
```bash
# Clean and compile the robustness suite
g++ -O3 -msse3 -march=native -Iinclude -Ithird_party/wirehair/include \
    src/deinterleaver.c src/fec_wrapper.c src/logging.c src/symbol.c src/stats.c \
    tests/receiver_robustness_test.c third_party/wirehair/*.cpp \
    -o build/bin/robustness_test -pthread -lstdc++

# Execute
./build/bin/robustness_test
2. Running the Burst Stress Test (Algorithm Logic)
Bash
# Compile the stress test (Excluding main.c)
g++ -O3 -msse3 -march=native -Iinclude -Ithird_party/wirehair/include \
    src/deinterleaver.c src/fec_wrapper.c src/interleaver.c \
    src/logging.c src/symbol.c src/stats.c \
    tests/burst_sim_test.c third_party/wirehair/*.cpp \
    -o build/burst_test -pthread -lstdc++

# Execute
./build/burst_test all
3. Running the Gateway (Real-time Sniffing)
Bash
# Build the full project via Makefile
make clean && make DEBUG=1

# Run with sudo for raw socket access
sudo ./build/fso_gateway --lan-iface eth0 --symbol-size 1500 --k 64 --m 32 --depth 100
Roadmap - Upcoming Milestones
Phase 6: End-to-End Integration: Connecting all modules into a single unified data plane.

Phase 7: Real-World Networking: Integrating libpcap and Raw Sockets for physical NIC testing (10Gbps target).

Phase 8: Atmospheric Simulation: Testing against real-world FSO scintillation models.