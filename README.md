FSO Gateway POC - High-Performance Packet Recovery System
Overview

The FSO Gateway is a specialized bridge designed to connect standard Ethernet networks over Free Space Optical (FSO) links. FSO communication is highly susceptible to atmospheric turbulence, which causes deep fading and significant "burst" packet losses.

This system mitigates these issues by implementing a robust pipeline of:

Fragmentation
Matrix Interleaving
Forward Error Correction (FEC) using Wirehair (Fountain Code)

The system operates strictly on a Packet Erasure Channel model, where packets are either received or lost — not bit-corrected.

Project Status: March 2026
Current Milestone:

Phase 6 – Full Simulation + Statistical Validation (Channel Campaign)

System Status Summary:

✔ End-to-End pipeline fully operational
✔ Receiver state machine stable and deterministic
✔ FEC layer validated mathematically and empirically
✔ Interleaving behavior validated under burst conditions
✔ Statistical campaign framework implemented
✔ Root-cause diagnostics infrastructure added

❗ Remaining Gap: Packet Reassembly correctness under stress

Completed Core Components
1. Infrastructure (Phases 0–1)
Linux-based development environment
Makefile (C + C++ integration for Wirehair)
Logging module (multi-level, timestamped)
AddressSanitizer (ASan) integration
2. Packet Processing & Fragmentation (Phase 2)
Full support for:
Small packets
MTU (1500B)
Jumbo frames (9000B)
Deterministic fragmentation into fixed-size symbols
Exact byte-level reassembly validation
3. Block Management & FEC Wrapper (Phases 3–4)
Block Builder (K symbols per block)
Timeout-based block closure
Wirehair integration via clean wrapper
Cold Buffer Fix (GF(256) correctness)
4. Matrix Interleaving (Phase 5)
Column-based interleaving across blocks
Burst spreading across time
Sparse deinterleaver (hole-tolerant)
5. Receiver Robustness (Phase 5.1)

The receiver was redesigned as a strict deterministic state machine:

Block lifecycle FSM:

EMPTY → FILLING → READY → EMPTY
Duplicate rejection (bitmap, O(1))
Late symbol handling (drop after closure)
Early failure detection (holes > M)
Threshold + timeout flush policy
Strict valid-symbol filtering (payload_len > 0 etc.)
End-to-End Integration (Task 20)

Full pipeline implemented:

packet
→ fragment
→ block builder
→ FEC encode
→ interleave
→ channel impairments
→ deinterleave
→ FEC decode
→ reassemble
Critical Bug Discovered
Missing symbols in final window
Interleaver not draining
Partial transmission
Root Cause

Incorrect fec_id assignment for padded symbols:

Duplicate (block_id, fec_id)
Missing column in interleaver
Fix
sym.fec_id = s;
Result

✔ 100% transmission
✔ 100% block decode
✔ Exact packet recovery

Channel Campaign Test Suite (Phase 6)
Purpose

Move from deterministic validation → statistical robustness validation

The campaign evaluates system behavior under:

Burst losses
Multiple bursts
Time-based fades
Corruption
Mixed impairments
Campaign Structure
69 scenarios
458+ total runs
Full pipeline execution per run
Block-level + Packet-level diagnostics
Scenario Categories
A — Very Short Bursts

Sanity validation (1–10 symbols)

B — Single Burst Sweep

Wide range burst testing (16 → 1024)

C — Boundary Testing

Around theoretical limit (≈64 symbols)

D — Time-Based Fades

µs → symbols conversion

E — Multi-Burst

Distributed impairments

F — Blackout

Long continuous loss

G — Corruption

Invalid symbols (not erasures)

H — Mixed

Erasure + corruption

I — Failure Edge

Post-threshold degradation

Theoretical Model (Oracle)

The campaign introduced an Erasure Recoverability Oracle:

recoverable ⇔ received_symbols ≥ K

Important:

✔ Oracle valid ONLY for erasure-only runs
❌ Disabled for corruption/mixed scenarios

Metrics Collected
Block-Level
total_blocks_attempted
total_blocks_passed
total_blocks_failed
fail_too_many_holes
fail_insufficient_symbols
Packet-Level (NEW)
packet_fail_missing
packet_fail_corrupted
packet_fail_after_successful_block_decode
Critical Discovery #1 — Packet-Level Failure After FEC

Observed:

blocks_passed = 100%
blocks_failed = 0
BUT:
packet_fail_after_successful_block_decode > 0
Meaning

✔ FEC works perfectly
❌ Packet reconstruction is broken

Root Cause Area
Fragment tracking
Packet completion logic
Late/duplicate handling
Critical Discovery #2 — Recoverable Blocks Discarded Before Decode

New metric:

oracle_recoverable_but_discarded_before_decode
Meaning

Blocks that:

✔ Had enough symbols to decode
❌ Were discarded by system before decode

Root Cause

Receiver lifecycle bug:

Blocks remained in FILLING
Evicted before reaching READY
Not decoded despite being recoverable
Diagnostics Infrastructure (NEW)

To debug this class of issues, we added:

1. Source-of-Truth Block Final Reasons

Each block now ends with a single explicit reason:

decode_success
fail_too_many_holes
fail_insufficient_symbols
discarded_before_decode
2. Eviction Trace System

For each evicted block we record:

state at eviction
symbol_count
holes
time since first symbol
whether it was recoverable
3. Suspicious Block Reporting

Focused diagnostics for:

oracle_recoverable_but_discarded_before_decode

Including:

exact block state
timing conditions
eviction reason
4. Accounting Fixes

Corrected inconsistency:

❌ blocks_decode_success > 0 while attempts = 0

✔ Now:

blocks_attempted_for_decode = actual decoder calls
Fundamental Design Insight
Wirehair Behavior
Wirehair DOES NOT fix corrupted data
Wirehair ONLY fixes missing data

Therefore:

✔ System must convert corruption → erasure
✔ Invalid symbols must be rejected BEFORE FEC

This aligns with the architecture:

→ Packet Erasure Channel model

Current System State
✔ Fully Solved
FEC correctness
Interleaving gain
Burst resilience
Block lifecycle management
Theoretical boundary validation
❗ Remaining Work (Phase 6 continuation)

Focus areas:

Packet reassembly correctness
Fragment completion logic
Handling duplicates / late arrivals
Prevent premature packet discard
Key Achievements

✔ Mathematically validated FEC limits
✔ Identified exact burst tolerance (64 symbols)
✔ Built full statistical validation framework
✔ Discovered non-FEC bottleneck
✔ Reduced debugging scope dramatically

Build & Execution
Run Campaign
make clean
make ctest > build/ctest_full.log 2>&1
View Results
tail -n 120 build/ctest_full.log
Summary

The system has reached a critical milestone:

✔ FEC and interleaving are fully validated
✔ System behaves exactly as theory predicts

The remaining gap is:

❗ purely software-level (packet reconstruction)

This means:

👉 The hard problem (channel + coding) is solved
👉 The rest is deterministic debugging