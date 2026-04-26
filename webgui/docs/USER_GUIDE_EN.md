# FSO Gateway Web GUI — User Guide

This document describes every screen, every panel, and every parameter in the FSO Gateway Web GUI in detail. For each field, it documents:

- **Meaning** — what the value represents in terms of network / FEC / system behavior.
- **Source** — where the value comes from: the C function, file, and line number (where relevant).
- **Units** — rate (Mbps / pps), time (ms / seconds), count, ratio, etc.
- **How to interpret** — what is normal, what thresholds apply, when to worry.

This document covers `webgui-v1.3.0` (Bridge `1.3.0`, telemetry schema `fso-gw-stats/2`).

---

## Table of Contents

1. [Architecture — three layers](#architecture--three-layers)
2. [Top Bar](#top-bar)
3. [Sidebar](#sidebar)
4. [Dashboard](#dashboard)
5. [Link Status](#link-status)
6. [Traffic](#traffic)
7. [FEC Analytics](#fec-analytics)
8. [Interleaver](#interleaver)
9. [Packet Inspector](#packet-inspector)
10. [Channel](#channel)
11. [Configuration](#configuration)
12. [System Logs](#system-logs)
13. [Alerts](#alerts)
14. [Topology](#topology)
15. [Analytics — recordings & history](#analytics--recordings--history)
16. [About](#about)
17. [Appendix A — Glossary](#appendix-a--glossary)
18. [Appendix B — Counter source map](#appendix-b--counter-source-map)

---

## Architecture — three layers

The GUI does not measure anything itself. Every number you see comes from C code running as a daemon on the gateway machine. The data flow:

```
[Linux NIC] → [C Daemon: fso_gw_runner]
                ├─ stats_container         (atomic counters)
                ├─ deinterleaver_t          (RX FEC stats)
                ├─ arp_cache_t              (IP↔MAC table)
                └─ control_server           (AF_UNIX socket @ 10 Hz)
                          │
                          ▼  /tmp/fso_gw.sock  (ndjson, schema "fso-gw-stats/2")
                  [FastAPI Bridge: webgui/server/main.py + gateway_source.py]
                          ├─ converts C structs → browser-friendly JSON
                          ├─ computes derived values (Mbps, pps, qualityPct, alerts)
                          └─ persists history to SQLite (runs/samples)
                          │
                          ▼  WebSocket  /ws/live  (1 Hz snapshots)
                          ▼  REST       /api/*    (config, channel, runs, daemon)
                          ▼  WebSocket  /ws/logs  (stderr log stream)
                  [Next.js GUI: webgui/src/]
                          └─ browser at http://<host>:3100
```

**Practical implications:**

- *The daemon measures everything in C atomics* — no value is fabricated in the GUI. If a counter doesn't exist in C, the field doesn't exist in the GUI (or is explicitly flagged as Mock).
- *The Bridge computes deltas over time* — rates (Mbps, pps, blk/s) are computed in Python from differences between consecutive snapshots.
- *The Frontend just paints* — each hook (`useTelemetry`, `useChannel`, etc.) receives JSON, keeps a small in-memory history, and renders with ECharts/Tailwind.
- *Mock vs. Live* — if `/tmp/fso_gw.sock` is unavailable, the Bridge falls back to `SimState` (Python simulation) which produces synthetic values in the same schema. The Connection Pill in the top-left indicates "live" or "demo".

**Key C files referenced throughout this guide:**

| Role | File |
|---|---|
| Global atomic counters | [src/stats.c](../../src/stats.c), [include/stats.h](../../include/stats.h) |
| Telemetry server (UNIX socket) | [src/control_server.c](../../src/control_server.c) |
| FEC RX (deinterleave + decode) | [src/deinterleaver.c](../../src/deinterleaver.c) |
| FEC TX (fragment + encode + interleave) | [src/tx_pipeline.c](../../src/tx_pipeline.c), [src/interleaver.c](../../src/interleaver.c) |
| ARP cache (proxy-ARP) | [src/arp_cache.c](../../src/arp_cache.c) |
| Raw frame I/O | [src/packet_io.c](../../src/packet_io.c) |
| Per-symbol CRC-32C | [src/symbol.c](../../src/symbol.c) |
| Configuration (CLI args) | [src/config.c](../../src/config.c) |

**Bridge files (Python):**

| Role | File |
|---|---|
| FastAPI app + WebSocket /ws/live | [webgui/server/main.py](../server/main.py) |
| control_server reader, derived values, alerts | [webgui/server/gateway_source.py](../server/gateway_source.py) |
| `tc netem` controller | [webgui/server/channel.py](../server/channel.py) |
| Configuration management (YAML) | [webgui/server/config_store.py](../server/config_store.py) |
| Tail of `/tmp/fso_gw.log` | [webgui/server/log_source.py](../server/log_source.py) |
| Recorder + CSV export | [webgui/server/run_store.py](../server/run_store.py) |
| Daemon supervisor | [webgui/server/daemon.py](../server/daemon.py) |

---

## Top Bar

Source: [webgui/src/components/layout/TopBar.tsx](../src/components/layout/TopBar.tsx)

The top bar shows real-time status independent of the current page. It is sticky at the top of the screen.

### Title

| Field | Source | Notes |
|---|---|---|
| **FSO Gateway · Control Center** | hardcoded | Fixed text |
| **v3.1 · BUILD a202b70** | hardcoded | Placeholder; **does not** reflect the actual Bridge version. The real version is in `/health` and on the About page. |

### Connection Pill

| Value | Meaning |
|---|---|
| **LIVE** green | The `/ws/live` WebSocket is open and the Bridge is receiving data from the C daemon's socket. |
| **DEMO** blue | The WebSocket is open but the Bridge cannot read `/tmp/fso_gw.sock` — showing simulation data (`SimState` in `main.py`). |
| **CONNECTING** amber | The WebSocket is connecting / reconnecting (with backoff). |

The state is managed in `useTelemetry.ts` based on `WebSocket.readyState` and the `source` field in the payload.

### System

Shows the overall link state — not just throughput but FEC quality.

| Label | Condition | Meaning |
|---|---|---|
| **OPERATIONAL** green | `qualityPct > 99.5%` | Healthy. Almost all blocks recover successfully. |
| **DEGRADED** amber | `95% < qualityPct ≤ 99.5%` | Noticeable block losses — investigate the FSO channel. |
| **OFFLINE** red | `qualityPct ≤ 95%` | Severe state — recovery is failing. May indicate a physical disconnect. |

**Computation:** `qualityPct = 100 × blocks_recovered / blocks_attempted`. If `blocks_attempted == 0` (no blocks have flowed yet), the value defaults to `100%`.

Data sources:
- `blocks_attempted`: [src/stats.c](../../src/stats.c) — `stats_inc_block_attempt()`. Called from [src/deinterleaver.c](../../src/deinterleaver.c) when a block enters `FILLING`.
- `blocks_recovered`: `stats_inc_block_success()` — called when a block successfully decodes.
- The threshold logic is in [gateway_source.py](../server/gateway_source.py) `snapshot()`.

### Daemon (new in v1.3.0)

Shows the daemon state per the DaemonSupervisor in the Bridge.

| Label | Meaning |
|---|---|
| **STOPPED** gray | No active process. Click Start in the Configuration page. |
| **STARTING** breathing amber | Subprocess launched; the Bridge is verifying it didn't immediately exit. |
| **RUNNING** green | Process is alive. `pid != null`. |
| **STOPPING** breathing amber | SIGTERM sent; waiting up to 4 seconds before SIGKILL. |
| **FAILED** red | Process died unexpectedly, or the binary was not found. The `lastError` is shown on the Configuration page. |

**Important note:** the state reflects the supervisor in the Bridge — not the C daemon itself. If the daemon crashes without the supervisor noticing (theoretical), the watcher task will update the state to `failed` via `proc.wait()`.

Source: [webgui/server/daemon.py](../server/daemon.py) — `DaemonSupervisor.status()`. The hook is [useDaemon](../src/lib/useDaemon.ts) which polls `/api/daemon` every 2 seconds.

### Uptime

Time elapsed since the daemon opened the telemetry server.

- **Source:** `link.uptimeSec` straight from `uptime_sec` in the snapshot. The computation uses `CLOCK_MONOTONIC` in [src/control_server.c](../../src/control_server.c) — `elapsed_seconds()`.
- **Format:** `formatUptime()` in [utils.ts](../src/lib/utils.ts) — `HhMMm` or `MMm SSs`.
- **Not** the host machine / Bridge uptime — only the daemon process.

### Time (UTC)

Plain browser clock (`new Date().toISOString().slice(11, 19)`) updating every second. Unrelated to the daemon.

### Right-edge icons (Search / Maximize / Bell)

Cosmetic buttons. The Bell shows a red dot constantly — placeholder, **not** wired to the alerts feed yet.

---

## Sidebar

Source: [webgui/src/components/layout/Sidebar.tsx](../src/components/layout/Sidebar.tsx)

A fixed sidebar on the left with 13 navigation items. Each is a Next.js Link.

| Item | Path | Short description |
|---|---|---|
| Dashboard | `/` | Overview: status, throughput, errors. |
| Link Status | `/link-status` | Quality history + fade events. |
| Traffic | `/traffic` | Detailed traffic: TX/RX, peak/avg, average packet size. |
| FEC Analytics | `/fec-analytics` | Block outcomes, burst histogram, dil_stats. |
| Interleaver | `/interleaver` | K×M matrix visualization, burst recoverability. |
| Packet Inspector | `/packet-inspector` | On-wire symbol header, CRC events. |
| Channel | `/channel` | `tc netem` control — inject loss to test FEC. |
| Configuration | `/configuration` | FEC parameters + Runtime Controls. |
| System Logs | `/logs` | Live daemon log stream. |
| Alerts | `/alerts` | Alert history + filtering. |
| Topology | `/topology` | FSO node map + ARP cache. |
| Analytics | `/analytics` | Recorded run list + CSV export. |
| About | `/about` | System status and version info. |

### Health bar at the bottom

`Health 98.7%` — **placeholder** (hardcoded). Not connected to any API. Reserved for future wiring.

---

## Dashboard

Source: [webgui/src/app/page.tsx](../src/app/page.tsx)

Five panels updating every second via the `/ws/live` WebSocket.

### LinkStatusHero — Gateway diagram

Source: [webgui/src/components/dashboard/LinkStatusHero.tsx](../src/components/dashboard/LinkStatusHero.tsx)

A panel with a diagram: `Win-1 ── GW-A ══ GW-B ── Win-2`.

| Field | Source | Notes |
|---|---|---|
| **LAN/FSO iface names** | `configEcho.lanIface`, `configEcho.fsoIface` | Echoed straight from CLI args the daemon was launched with, via [src/control_server.c](../../src/control_server.c) (`configEcho`). |
| **"Win-1 / Win-2" labels** | hardcoded | Fixed text for the Phase 8 fixture (192.168.50.1, 192.168.50.2). |
| **Status Dot green/red** | derived from `link.state` | Green if `state != "offline"`. |
| **STATE (large text)** | `link.state` | "ONLINE" / "DEGRADED" / "OFFLINE". |
| **Quality %** | `link.qualityPct` | Overall block-decode success rate. |
| **K=N · M=N · depth=N** | `configEcho.k`, `.m`, `.depth` | Active daemon configuration. |

### ThroughputCards — throughput tiles

Source: [webgui/src/components/dashboard/ThroughputCards.tsx](../src/components/dashboard/ThroughputCards.tsx)

Four cards: TX, RX, Packets/sec, Utilization.

#### TX Throughput

- **Large value** — `throughput[-1].txBps` formatted as Mbps/Gbps.
- **Sparkline** — last 60 samples of `txBps`.
- **Sub-text** — `txPps` (packets per second).

**How the Bridge computes this:**
```
txBps = (transmitted_bytes_now - transmitted_bytes_prev) × 8 / dt_seconds
txPps = (transmitted_packets_now - transmitted_packets_prev) / dt_seconds
```

C source:
- `transmitted_packets`, `transmitted_bytes` in [src/stats.c](../../src/stats.c) — `stats_inc_transmitted(size_t bytes)`.
- Called in [src/tx_pipeline.c:554](../../src/tx_pipeline.c) when a symbol hits the FSO wire.

> **Note:** `transmitted` counts *per symbol*, not per original packet. Each LAN packet is fragmented into K source symbols + M parity, and each is counted. This is the rate on the *FSO wire*.

#### RX Throughput

- **Large value** — `throughput[-1].rxBps`.
- **Sub-text** — `rxPps`.

**Computed identically** from `recovered_bytes` / `recovered_packets`.

Source:
- `stats_inc_recovered(size_t bytes)` in [src/stats.c](../../src/stats.c).
- Called in [src/rx_pipeline.c](../../src/rx_pipeline.c) (~lines 463, 502, 529) when a packet has been reassembled and forwarded out the LAN.

> **Note:** `recovered` counts *whole reassembled packets*, not symbols. If a block fails to decode, none of its packets are counted. This means `rxBps` measures *traffic delivered to the customer* on the other side, not just bytes ingested by the daemon.

#### Packets/sec

- **Large value** — `txPps`.
- **Sub-text** — `rxPps`.

#### Link Utilization

- **Large value** — `(latestTxBps + latestRxBps) / (10 Gbps × 2) × 100`.
- **Client-side computation only**, assumes a 10 Gbps link in each direction.
- **Tone:** green <80%, amber ≥80%.
- **Not** based on actual NIC physical capacity — purely a conventional ratio against an assumed cap.

### ErrorMetrics — error metric cards

Source: [webgui/src/components/dashboard/ErrorMetrics.tsx](../src/components/dashboard/ErrorMetrics.tsx)

Five cards across the row.

#### Symbol Loss Ratio

- **Meaning:** percentage of symbols that were lost on the FSO channel and never reached the deinterleaver.
- **Computation:** `lost_symbols / total_symbols`.
- **C source:** `stats_record_symbol(bool lost)` in [src/stats.c](../../src/stats.c) lines ~441-476. Called in [src/rx_pipeline.c](../../src/rx_pipeline.c) once per symbol the daemon attempts to parse.
- **Display:** scientific notation when ratio < 1e-4 (e.g. `5.2e-5`); percentage otherwise.
- **Thresholds:**
  - `< 0.01%` — clean channel.
  - `0.01–1%` — moisture/particles. Still within FEC budget.
  - `> 5%` — failures will start.

#### Block Fail Rate

- **Meaning:** percentage of blocks that failed to decode entirely.
- **Computation:** `blocks_failed / blocks_attempted`.
- **Source:** `stats_inc_block_failure()` is called from [src/deinterleaver.c](../../src/deinterleaver.c) for: timeout (insufficient symbols arrived in time), too-many-holes (more than M holes).
- **Tone:** green at 0%, amber >0.1%, red >0.1%.

#### CRC Drops

- **Meaning:** number of symbols rejected by the per-symbol CRC-32C check (when enabled).
- **Source:** `stats_inc_crc_drop_symbol()` in [src/stats.c](../../src/stats.c) line ~485. Called in [src/rx_pipeline.c](../../src/rx_pipeline.c) or [src/symbol.c](../../src/symbol.c) when CRC mismatch.
- **Use:** a CRC-failed symbol is treated as an unknown erasure (FEC may substitute it if enough redundancy exists).
- **When to worry:** if the count climbs rapidly, the link is suffering corruption (not just loss).

#### Recovered Packets

- **Meaning:** cumulative count of LAN packets reassembled and forwarded out.
- **Source:** `recovered_packets` in [src/stats.c](../../src/stats.c), `stats_inc_recovered()`.
- **Call sites:** [src/rx_pipeline.c](../../src/rx_pipeline.c) lines 463, 502, 529 (three reassembly success paths).

#### FEC Success Rate

- **Computation:** `(blocksRecovered / blocksAttempted) × 100`.
- **Computed client-side** (from the same `errors` fields in the snapshot).
- **Tone:** green >99.9%, cyan >99%, amber otherwise.

### LiveCharts — real-time charts

Source: [webgui/src/components/dashboard/LiveCharts.tsx](../src/components/dashboard/LiveCharts.tsx)

#### Throughput Over Time

- Two series: TX Mbps (cyan) and RX Mbps (blue dashed).
- Last 180 samples (3 minutes at 1 Hz).
- Y-axis in Mbps; X-axis is relative time.

#### Packet Rate (pps)

- Two series: TX pps, RX pps.
- Last 180 samples.

#### Burst-Length Distribution

- Histogram of 7 bins: `1`, `2-5`, `6-10`, `11-50`, `51-100`, `101-500`, `501+`.
- **C source:** `burst_len_1` ... `burst_len_501_plus` in [src/stats.c](../../src/stats.c) lines 50-56.
- **How a burst is computed:** `stats_record_symbol()` counts a run of consecutive lost symbols. When a non-lost symbol arrives, the burst closes and is classified into the appropriate bin in `stats_close_current_burst()` ([src/stats.c](../../src/stats.c) lines 99-115).
- **Colors:** cyan/blue for bins recoverable within `M × depth`, amber/red for those exceeding.

#### Cumulative Counters

- **Blocks with loss** — `decoderStress.blocksWithLoss` ← `blocks_with_loss` in [src/stats.c](../../src/stats.c) line 66. Blocks that had at least one hole post-deinterleave.
- **Total holes** — `decoderStress.totalHolesInBlocks` ← sum of all holes across all blocks.
- **Recoverable bursts** — `recoverable_bursts` in [src/stats.c](../../src/stats.c) line 60. Bursts whose length ≤ `M × depth`.
- **Critical bursts** — `critical_bursts` in [src/stats.c](../../src/stats.c) line 61. Bursts that exceeded the span and could not have been recovered.

---

## Link Status

Source: [webgui/src/app/link-status/page.tsx](../src/app/link-status/page.tsx)

### Hero Tiles

| Field | Source | Notes |
|---|---|---|
| **STATE** | `link.state` | "ONLINE" / "DEGRADED" / "OFFLINE" — derived in the Bridge. |
| **Quality %** | `link.qualityPct` | `(blocks_recovered / blocks_attempted) × 100`. |
| **Symbol Loss** | `errors.symbolLossRatio` | Ratio; shown as percentage or `1e-X`. |
| **Block Fail Rate** | `errors.blockFailRatio` | `blocks_failed / blocks_attempted`. |
| **Blocks Attempted** | `errors.blocksAttempted` | Cumulative since daemon launch. |
| **Session Uptime** | `link.uptimeSec` | Same source as the TopBar. |

### Quality Over Time

A line chart of `qualityPct` over time. Y-axis range 80–100% to amplify resolution near the top.

- **Source:** [useLinkHistory](../src/lib/useLinkHistory.ts) reads each snapshot and stores history client-side (not persisted server-side).
- **Window:** up to several hundred samples; lives in browser memory only.

### Symbol Loss Ratio

A logarithmic chart of symbol-loss ratio over time.

- **Log Y-axis** to span a wide range (1e-6 to 1.0).
- Same data source as the Quality sample.

### Stability Panel

| Field | Source | Note |
|---|---|---|
| **Session Uptime %** | client computation | Ratio of time spent in `online`/`degraded` vs. total observed time. |
| **Observed For** | client computation | How long the page has been open. |
| **In Current State** | client computation | Time since the last state transition. |
| **Fade Events** | client computation | Number of `online↔degraded↔offline` transitions detected. |

⚠️ **The entire panel is client-side analysis** of the WebSocket history. There is no "fade detection" mechanism in the C daemon — it's analytics done in the browser.

### Fade Timeline

A table with one row per fade event:

- **Timestamp** — `tStart` in ms.
- **From → To** — state transition (`online → degraded`).
- **Lowest Quality** — minimum `qualityPct` observed during the fade.
- **Duration** — `tEnd - tStart`, or "ongoing".

⚠️ Synthetic in the same way.

---

## Traffic

Source: [webgui/src/app/traffic/page.tsx](../src/app/traffic/page.tsx)

A more detailed traffic page — uses the same fields as the Dashboard.

### TX Card / RX Card

Per card:

| Field | Source | Notes |
|---|---|---|
| **Current Rate** | `throughput[-1].txBps` / `.rxBps` | Current rate in Mbps/Gbps. |
| **Current PPS** | `throughput[-1].txPps` / `.rxPps` | Packets per second. |
| **Utilization** | `(latestBps / 10e9) × 100` | Relative to 10 Gbps (arbitrary assumption). |
| **Peak Rate** | `max(history.txBps)` | Maximum across the available history. |
| **Avg Rate** | `avg(history.txBps)` | Average. |
| **Peak PPS** | `max(history.txPps)` | Peak packet rate. |
| **Avg Packet Size** | `latestBps / 8 / latestPps` | Bytes-per-packet computation. |
| **Sparkline** | `throughput.slice(-60)` | 60 samples (1 minute). |

History is held in the client (`useTelemetry` keeps up to 300 samples). There is no separate Traffic endpoint — everything comes in the single snapshot.

### Charts

- **Throughput** — two series `txBps`/`rxBps` over 300 samples.
- **PPS** — two series `txPps`/`rxPps`.

### Summary Metrics

| Field | Computation |
|---|---|
| **Combined Throughput** | `latestTxBps + latestRxBps`. |
| **Combined PPS** | `latestTxPps + latestRxPps`. |
| **Avg TX Packet** | `txBps / 8 / txPps`. |
| **Peak Utilization** | `max(peakTxBps, peakRxBps) / 10e9 × 100`. |

---

## FEC Analytics

Source: [webgui/src/app/fec-analytics/page.tsx](../src/app/fec-analytics/page.tsx)

This is the most important page for understanding FEC behavior.

### Hero Metrics

| Field | Source | Notes |
|---|---|---|
| **Recovery Rate** | `(blocksRecovered / blocksAttempted) × 100` | Same as Quality elsewhere; shown in FEC context. |
| **Blocks/sec** | average of last 5 samples | Block decode rate. Sub-text: recovered/sec and failed/sec. |
| **Failed Blocks** | `errors.blocksFailed` | Cumulative. Tone by failure rate (>0.1% = warning). |
| **Symbol Loss** | `errors.symbolLossRatio` | Same as elsewhere. |
| **Critical Bursts** | sum of `101-500` and `501+` bins of `burstHistogram` | Cases the FEC could not have covered. |

### FEC Config Panel

Shows the daemon's active configuration.

| Field | Source |
|---|---|
| **K (Source Symbols)** | `configEcho.k` — origin: daemon CLI flag `--k`, echoed back in the snapshot. |
| **M (Repair Symbols)** | `configEcho.m` — `--m`. |
| **Overhead** | `m / (k + m) × 100` — client-side. |
| **Code Rate** | `k / (k + m)` — client-side. |
| **LAN Interface** | `configEcho.lanIface` — `--lan-iface`. |
| **FSO Interface** | `configEcho.fsoIface` — `--fso-iface`. |
| **Symbol Size** | `configEcho.symbolSize` — `--symbol-size`. |
| **Symbol CRC** | `configEcho.internalSymbolCrc` — "CRC-32C" if true, "Disabled" otherwise. |

> **Why surface the config on every page?** Because these parameters affect the *meaning* of every other number. A burst of length 50 is critical when `M × depth = 32`, but harmless when `M × depth = 100`.

### Block Outcomes Over Time

A chart with two stacked series:
- **Recovered blocks/sec** — per-second delta of `blocks_recovered`.
- **Failed blocks/sec** — per-second delta of `blocks_failed`.

Source: [useFecHistory](../src/lib/useFecHistory.ts) reads each snapshot and stores history.

### Burst-Length Distribution

Same as Dashboard, but with a logarithmic Y-axis (wider range).

### Deinterleaver Panel (important!)

This is the only place in the GUI showing the deinterleaver's internal `dil_stats` — added in the `fso-gw-stats/2` schema.

| Field | C source | Meaning |
|---|---|---|
| **Active** | `dil_stats.active_blocks` in [src/deinterleaver.c](../../src/deinterleaver.c) | Blocks in `FILLING` or `READY_TO_DECODE` — current occupancy. |
| **Ready** | `dil_stats.ready_count` | Blocks completed and waiting on `deinterleaver_get_ready_block()`. |
| **Blocks ready (cumulative)** | `dil_stats.blocks_ready` | Cumulative since launch. |
| **Failed · timeout** | `dil_stats.blocks_failed_timeout` | Blocks discarded because not enough symbols arrived in time (timeout = 50 ms by default). |
| **Failed · holes** | `dil_stats.blocks_failed_holes` | Blocks that could not be decoded due to more than M holes. |
| **Dropped · duplicate** | `dil_stats.dropped_symbols_duplicate` | A symbol arrived twice (same fec_id). |
| **Dropped · frozen** | `dil_stats.dropped_symbols_frozen` | A symbol arrived for a block that has already left `FILLING` (too late). |
| **Dropped · erasure** | `dil_stats.dropped_symbols_erasure` | Symbols marked `is_erasure=1` (lost). |
| **Dropped · CRC fail** | `dil_stats.dropped_symbols_crc_fail` | Symbols rejected by CRC-32C. |
| **Evicted · filling** | `dil_stats.evicted_filling_blocks` | Slot was evicted while still in FILLING (out of buffer space). |
| **Evicted · done** | `dil_stats.evicted_done_blocks` | Slot was evicted from `READY_TO_DECODE` without being drained. |

> **What causes eviction?** The deinterleaver is implemented as a circular buffer. When more new blocks arrive than the capacity (`depth × 4`), older blocks are pushed out. This shouldn't happen in production — if it does, there is RX backpressure.

### Block Lifecycle Event Feed

A table of recent block events. Shows every transition in the deinterleaver FSM.

| Column | Source |
|---|---|
| **Timestamp** | `blockEvents[].t` — `now_ms()` in [src/control_server.c](../../src/control_server.c). |
| **Reason** | `blockEvents[].reason` — enum mapped from `deinterleaver_block_final_reason_t`. Possible values: `SUCCESS`, `DECODE_FAILED`, `TIMEOUT`, `TOO_MANY_HOLES`, `EVICTED_FILLING`, `EVICTED_READY`. |
| **Block ID** | `blockEvents[].blockId` — from the block itself. |
| **Evicted** | `blockEvents[].evicted` — bool, true if the block closed via eviction. |

Behind the scenes: the deinterleaver registers a callback in the control_server, and every final transition is pushed to a list (`block_events` ring buffer of ~128 entries). The Bridge surfaces this in the snapshot.

### Decoder Stress Panel

| Field | Source |
|---|---|
| **Blocks with Loss** | `blocks_with_loss` — counted when a block finalized with at least one hole. |
| **Worst Holes / Block** | `worst_holes_in_block` — maximum holes observed in any single block. |
| **Total Holes** | `total_holes_in_blocks` — sum across all blocks. |
| **Recoverable Bursts** | `recoverable_bursts` — burst length ≤ `M × depth`. |
| **Critical Bursts** | `critical_bursts` — bursts that exceeded. |
| **Exceeding FEC Span** | `bursts_exceeding_fec_span` — usually identical to Critical (differs in edge cases). |

General source: [src/stats.c](../../src/stats.c) lines 58-68. The classification happens in `stats_close_current_burst()` (lines 137-164).

---

## Interleaver

Source: [webgui/src/app/interleaver/page.tsx](../src/app/interleaver/page.tsx)

Largely a documentation page — illustrates the matrix structure.

### Metric Cards

| Field | Computation | Notes |
|---|---|---|
| **Matrix (depth × K+M)** | `configEcho.depth × (k + m)` | Matrix dimensions. |
| **Cell count** | `depth × (k + m)` | Total matrix cells. |
| **Matrix Size** | `cell_count × symbolSize` | Byte size. |
| **Recovery Span** | `m × depth` | Maximum number of consecutive lost symbols still recoverable. |
| **Burst Coverage** | `(burstWithinSpan / totalBursts) × 100` | Percentage of observed bursts that were recoverable. |
| **Exceeding Span** | `decoderStress.burstsExceedingFecSpan` | Bursts too long to recover. |

### Matrix Layout (visual)

An interactive visualization of the matrix (`depth` rows × `K+M` columns):
- The first K columns are colored cyan (data).
- The last M columns are colored amber (parity).

### Derivations Panel

Repeats the same parameters as a list:

| Field | Computation |
|---|---|
| **Depth (rows)** | `configEcho.depth` |
| **Block width** | `k + m` |
| **Total symbols / block** | `k + m` |
| **Max burst recoverable** | `m × depth` |
| **Symbol size** | `configEcho.symbolSize` |
| **Block size** | `(k + m) × symbolSize` |

---

## Packet Inspector

Source: [webgui/src/app/packet-inspector/page.tsx](../src/app/packet-inspector/page.tsx)

A page explaining the on-wire symbol format plus activity metrics.

### Activity Cards

| Field | Source | Notes |
|---|---|---|
| **Packets/sec (TX)** | `throughput[-1].txPps` | Same as Traffic. |
| **RX packets/sec** | `throughput[-1].rxPps` | Sub-text. |
| **Avg Packet Size** | `latestBps / 8 / latestPps` | Bytes. |
| **Symbols each (approx)** | `avgPacketSize / symbolSize` | How many symbols per packet. |
| **Symbols Processed** | `blocksAttempted × k` | Estimate of total symbols processed. |
| **CRC Drops** | `errors.crcDrops` | Total CRC failures. |
| **CRC enabled/disabled** | `configEcho.internalSymbolCrc` | Descriptive text. |

### Wire Format Diagram

Shows the symbol header layout — 18 fixed bytes:

| Field | Bytes | C source |
|---|---|---|
| `packet_id` | 4 | [include/symbol.h](../../include/symbol.h) |
| `fec_id` | 4 | The symbol's index within the block (0..K-1 = source, K..K+M-1 = parity). |
| `symbol_index` | 2 | Position within the original block. |
| `total_symbols` | 2 | `K+M`. |
| `payload_len` | 2 | Data length. |
| `crc32` | 4 | CRC-32C over the payload (when enabled). |
| **payload** | symbolSize | Actual data (typically 800-1500 bytes). |

### Recent Symbol Events

A logs table filtered to symbol events (keywords: `pkt_id`, `block_id`, `symbols`, `fragment`, `reassemble`).

Source: WebSocket `/ws/logs` connected via [useLogs](../src/lib/useLogs.ts).

⚠️ When the daemon is not running, the Bridge generates synthetic logs — not real.

---

## Channel

Source: [webgui/src/app/channel/page.tsx](../src/app/channel/page.tsx) and [useChannel.ts](../src/lib/useChannel.ts)

A page that uses Linux `tc netem` to inject packet loss into the FSO channel for FEC testing.

### Presets

Six presets that load potential slider values. They are **not** applied automatically — you must click Apply.

| Preset | enterPct | exitPct | lossPct | Description |
|---|---|---|---|---|
| **Clear** | 0 | 0 | 0 | Disables `netem`, clean channel. |
| **Drizzle** | 1 | 70 | 0.5 | Sparse single-symbol loss. |
| **Weather** | 5 | 50 | 5 | Moderate fades, still within FEC budget. |
| **Haze** | 8 | 30 | 8 | Sustained low-level loss. |
| **Storm** | 12 | 25 | 20 | Heavy bursts, stresses FEC. |
| **Blackout** | 30 | 10 | 50 | Pathological loss, expected to fail. |

### Sliders

| Slider | Meaning | Units |
|---|---|---|
| **Enter % (Burst Entry)** | Probability of transitioning from "good" to "bad" Gilbert-Elliott state. | Percent 0-100. |
| **Exit % (Burst Exit)** | Probability of leaving the "bad" state. | Percent 0-100. |
| **Loss % (in bad state)** | Packet loss rate while in the "bad" state. | Percent 0-100. |

### Buttons

- **Apply** — sends a POST to `/api/channel/netem` with the values. The Bridge runs:
  ```
  tc qdisc replace dev <FSO_iface> root netem loss gemodel <enter>% <exit>% <loss>%
  ```
  Source: [webgui/server/channel.py](../server/channel.py) — `apply_gemodel()`.

- **Clear** — sends a POST with `{clear: true}`. The Bridge runs `tc qdisc del`.

### Status Display

| Field | Source | Notes |
|---|---|---|
| **iface** | local state, default `enp1s0f1np1` | Editable. |
| **available** | result of `tc qdisc show` | Whether the `tc` binary is reachable (checked on the iface). |
| **active** | Whether a netem qdisc is currently active. | |
| **model** | `"gemodel"` / `"uniform"` / `null` | Active loss model. |
| **lossPct / enterPct / exitPct** | parsed from `tc` output | Current values from the system. |

### Permission requirements

`tc qdisc add/del` requires `CAP_NET_ADMIN`. Either the Bridge runs as root, or sudoers grants `tc`. If neither, the field will show `available=false` and a warning.

---

## Configuration

Source: [webgui/src/app/configuration/page.tsx](../src/app/configuration/page.tsx)

Daemon configuration editor. Changes are persisted to `webgui/server/config.yaml` and require a *restart* to take effect.

### FEC Parameters Panel

| Parameter | Range | Meaning | Derived value |
|---|---|---|---|
| **K — Source Symbols** | 4–256 | How many original symbols per block. Larger K = less overhead, more latency. | — |
| **M — Repair Symbols** | 1–256 | How many extra (parity) symbols. Larger M = better recovery capability. | — |
| **Overhead** | derived | `m / (k + m) × 100` — redundancy percentage. | M=4, K=8 → 33% overhead. |
| **Code Rate** | derived | `k / (k + m)` — information efficiency. | M=4, K=8 → 0.667. |
| **Block Size** | derived | `(k + m) × symbolSize` bytes. | K=8, M=4, sym=1500 → 18000 bytes. |
| **Burst Recovery ~** | derived | `m × depth` — maximum recoverable burst length. | M=4, depth=16 → 64 symbols. |

### Interleaver Panel

| Parameter | Range | Meaning |
|---|---|---|
| **Depth (rows)** | 1–256 | Number of rows in the interleaver matrix. Higher depth = better burst spread, but more latency and memory. |

### Symbol Panel

| Parameter | Range | Meaning |
|---|---|---|
| **Symbol Size** | 64–4096 | Payload bytes per symbol. Larger = less overhead, but higher exposure to single-symbol loss. |

### Network Interfaces Panel

| Parameter | Meaning |
|---|---|
| **LAN Interface** | The interface for client traffic. The daemon will `pcap_open` and capture frames. |
| **FSO Interface** | The interface for the FSO link. Encoded symbols are transmitted through it. |

### Symbol Integrity Panel

| Parameter | Meaning |
|---|---|
| **Internal Symbol CRC-32C** | When enabled, every symbol carries a CRC-32C over the payload. On RX, mismatched CRC drops the symbol before the FEC layer sees it. Effect: higher robustness against in-channel corruption; small throughput cost (4 bytes/symbol). |

### Action Bar

| Button | Action |
|---|---|
| **Apply Changes** | Sends a POST `/api/config` with the draft. The Bridge validates via `config_store.validate()` and writes YAML. A daemon restart is required to take effect. |
| **Revert** | Reverts the client-side draft. |

### Live Derived

The same derived values shown in the right column — read-only.

### Runtime Controls (Phase 3B)

Source: [useDaemon](../src/lib/useDaemon.ts) and [webgui/server/daemon.py](../server/daemon.py)

| Button | Action |
|---|---|
| **Start Gateway** | POST `/api/daemon/start` — the supervisor performs `subprocess.Popen` with `argv` built from config.yaml. |
| **Restart** | POST `/api/daemon/restart` — Stop then Start. |
| **Stop** | POST `/api/daemon/stop` — sends SIGTERM, then SIGKILL after 4s. |

Detailed status display:

| Field | Notes |
|---|---|
| **PID** | Subprocess identifier. |
| **Uptime** | Time since start. |
| **Sudo** | Whether the supervisor prepends `sudo -n` (per `FSO_DAEMON_SUDO=1`). |
| **Binary** | "found"/"missing" per `os.access(path, X_OK)`. |
| **binary path** | Path to the binary (`FSO_DAEMON_BINARY` or default `fso_gw_runner`). |
| **log** | Log file (`FSO_DAEMON_LOG`, default `/tmp/fso_gw.log`). |

### Daemon State Badge

See [TopBar > Daemon](#daemon-new-in-v130) — same logic.

### Relevant environment variables

| Variable | Default | Meaning |
|---|---|---|
| `FSO_DAEMON_BINARY` | `fso_gw_runner` | Path to the binary to launch. Can point to `control_server_demo` for development. |
| `FSO_DAEMON_SUDO` | `0` | If `1`, prepends `sudo -n` to argv (typically required in production for raw pcap). |
| `FSO_DAEMON_LOG` | `/tmp/fso_gw.log` | Where stdout+stderr are redirected. |
| `FSO_DAEMON_KILL_ON_EXIT` | `0` | If `1`, the Bridge stops the daemon on its own shutdown (otherwise the daemon outlives the bridge). |

---

## System Logs

Source: [webgui/src/app/logs/page.tsx](../src/app/logs/page.tsx) and [useLogs.ts](../src/lib/useLogs.ts)

Live log stream from the daemon.

### Log Console

| Field | Source |
|---|---|
| **Live stream** | WebSocket `/ws/logs`. |
| **Timestamp** | `event.ts_ms` — from daemon stderr, or `time.time()` if generated. |
| **Level** | `event.level` — `DEBUG`/`INFO`/`WARN`/`ERROR` per [src/logging.c](../../src/logging.c). |
| **Module** | `event.module` — subsystem (e.g. `tx_pipeline`, `deinterleaver`). |
| **Message** | `event.message` — text. |

### Controls

| Control | Purpose |
|---|---|
| **Level filter** | Filter by severity. |
| **Module filter** | Filter to a subsystem. |
| **Search** | Substring search in any field. |
| **Pause / Resume** | Halts rendering (the server keeps streaming). |
| **Export** | ⚠️ Not implemented yet. |

### Data source

The Bridge tails `/tmp/fso_gw.log` (or `FSO_LOG_FILE`) in [log_source.py](../server/log_source.py). If there is no file, it generates synthetic logs (e.g. "Mock log entry from sim_runner.c") — flagged with `source: "mock"` in each event.

### Log levels in C

Source: [include/logging.h](../../include/logging.h):
```c
typedef enum {
    LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR
} log_level_t;
```

Filtering happens in `log_set_level()`. Production default: `INFO`.

---

## Alerts

Source: [webgui/src/app/alerts/page.tsx](../src/app/alerts/page.tsx) and [useAlerts.ts](../src/lib/useAlerts.ts)

### Alert tabs

- **Active** — alerts the user has not acknowledged.
- **Acknowledged** — alerts the user has dismissed (stored in `localStorage`).
- **All** — everything.

### Filtering

- **Severity checkboxes** — `critical` / `warning` / `info`.
- **Module dropdown** — `LINK` / `FEC` / `CRC` / `BURST` / `ARP` / `CONFIG` / `*`.
- **Search** — substring.

### Alert Event Details

| Field | Source |
|---|---|
| **ID** | `alert.id` — `{ts}-{module}-{counter}`, generated in the Bridge. |
| **Timestamp** | `alert.t` — `int(time.time() * 1000)` in [gateway_source.py](../server/gateway_source.py). |
| **Severity** | `alert.severity` — one of `critical`/`warning`/`info`. |
| **Module** | `alert.module` — subsystem. |
| **Message** | `alert.message` — human-readable text. |
| **Acknowledged** | `localStorage` — client-side only. |

### Alert generation rules (Live)

Source: [gateway_source.py](../server/gateway_source.py) — `_detect_alerts()`. Called for each incoming snapshot, comparing to the previous one.

| Condition | Severity | Module | Message |
|---|---|---|---|
| `delta(blocks_failed) >= 3` | **critical** | FEC | "{N} FEC blocks failed this tick" |
| `delta(symbols_dropped_crc) >= 20` | **warning** | CRC | "{N} symbols dropped on CRC" |
| `max_burst_length` increased and exceeded 20 | **warning** | BURST | "Max burst length climbed to {N}" |
| `delta(bursts_exceeding_fec_span) > 0` | **critical** | BURST | "{N} burst(s) exceeded FEC span this tick" |

### Mock alerts (when the daemon is not running)

In [main.py](../server/main.py), `_maybe_emit_alert()` produces alerts at ~15% probability per tick, drawn from arbitrary templates. Demo only.

### Ring buffer

The list in the Bridge is capped at `ALERT_RING_MAX` (~200 entries). The GUI receives only the most recent in each snapshot.

---

## Topology

Source: [webgui/src/app/topology/page.tsx](../src/app/topology/page.tsx)

### Physical Layout

A fixed diagram (Phase 8 fixture):

```
Win-1 (192.168.50.1) ──[LAN]── GW-A ══[FSO cable]══ GW-B ──[LAN]── Win-2 (192.168.50.2)
```

### Static Labels

- **GW-A / GW-B** — fixed.
- **Win-1 · 192.168.50.1**, **Win-2 · 192.168.50.2** — fixed.
- **LAN/FSO iface names** — from `configEcho.lanIface`/`.fsoIface`.

### FsoBeam Visualization

An animated component (Framer Motion) simulating the light beam.

| Property | Source | Computation |
|---|---|---|
| **Beam color** | `link.state` | green/amber/red. |
| **Quality % label** | `link.qualityPct` | Shown inside the beam. |
| **TX intensity** | `min(1, txPps / 5000)` | Particle density (animation). |
| **RX intensity** | `min(1, rxPps / 5000)` | Same. |
| **TX/RX pps labels** | `throughput[-1].txPps`/`.rxPps` | "TX 1234 pps". |

### ARP Cache Panel

A table of entries learned by the daemon's arp_cache.

| Column | Source |
|---|---|
| **IP** | `arpEntries[].ip` — from [src/arp_cache.c](../../src/arp_cache.c). |
| **MAC** | `arpEntries[].mac`. |
| **Last Seen** | `now - arpEntries[].lastSeenMs` — client-side, e.g. "5s ago". |

Who writes to the table?
- In `fso_gw_runner` — `arp_cache_learn()` is called in [src/rx_pipeline.c](../../src/rx_pipeline.c) when an ARP request's `src_ip` is recognized from the LAN side.
- In `control_server_demo` — `seed_arp_cache()` calls `arp_cache_learn()` with the fixed Phase 8 entries every 5 seconds (because the TTL is 5 minutes).

The snapshot includes the table via `arp_cache_get_entries()`, which returns `struct arp_entry[]`.

⚠️ In mock mode (Bridge with no daemon), the table is a fixed list of 2 entries.

### Recent Link Events

A table identical to the one in Alerts — but filtered to the most recent events.

---

## Analytics — recordings & history

Source: [webgui/src/app/analytics/page.tsx](../src/app/analytics/page.tsx) and [useRuns.ts](../src/lib/useRuns.ts)

A page for recording and comparing historical runs. Data is persisted in SQLite (`webgui/server/runs.db`).

### Runs Tab

#### Active Run

| Field | Source |
|---|---|
| **activeRunId** | `useRuns().activeRunId` ← `/health` or `/api/runs`. |
| **New Run** | POST `/api/runs/new` → `run_store.create_run()`. Creates a new record and starts sampling at 1 Hz. |

#### Runs List

A table of all runs:

| Column | Source |
|---|---|
| **ID** | `run.id` — auto-increment in SQLite. |
| **Name** | `run.name` — optional, editable. |
| **Start time** | `run.start_ms` — `int(time.time() * 1000)` at creation. |
| **End time** | `run.end_ms` — null if active. |
| **Duration** | `(end_ms - start_ms) / 1000`. |
| **Sample count** | `run.sample_count`. |

#### Run Detail

You can pick a run and view:

| Chart | Source |
|---|---|
| **Throughput over time** | `samples[].txBps`/`rxBps` — sampled at 1 Hz. |
| **Quality (FEC) over time** | `samples[].qualityPct`. |
| **FEC block outcome rate** | `recovered`/`failed` per second. |

Sampled by `_recorder_loop()` in [main.py](../server/main.py) — a loop that takes a snapshot every second and writes to SQLite.

#### Summary Stats

| Field | Computation |
|---|---|
| **Peak/Avg throughput** | `max`/`avg` of `txBps`/`rxBps` across all samples. |
| **Min/Max/Avg quality** | Statistics over `qualityPct`. |
| **Total/Recovered/Failed blocks** | From the run's last snapshot. |

#### Export CSV

GET `/api/runs/{runId}/export.csv` — returns a CSV with columns `t,txBps,rxBps,txPps,rxPps,qualityPct,...`. Built in [run_store.py](../server/run_store.py) — `export_csv()`.

### Experiments Tab

A placeholder page. The API exists (`/api/experiments` in [experiments.py](../server/experiments.py)) — reads `.txt` files from `/tmp/fso_experiments/` and parses them. The UI does not yet render them in detail.

---

## About

Source: [webgui/src/app/about/page.tsx](../src/app/about/page.tsx)

A meta page about the system.

### System Status Cards

| Card | Field | Source | Notes |
|---|---|---|---|
| **Bridge** | "Connected" / "Unreachable" | `useHealth()` — GET `/health`. | Whether FastAPI responds. |
| | "Tick {Hz}" | `health.tick_hz` | Bridge sampling rate (default 1 Hz). |
| **Gateway** | "Live (control_server)" / "Mock data" | `health.source` | "gateway" if the socket is up, "mock" otherwise. |
| | "Streaming via UNIX socket" | `health.source == "gateway"` | Status indicator. |
| **Recording** | "Run #{id}" / "Not recording" | `health.active_run_id` | Recorder state. |
| | logs mode | `health.logs_mode` | "live"/"tail"/"file"/"idle". |

### Architecture Diagram

A static diagram (not dynamic) of the three layers:
```
Browser  ─[WS/REST :8000]─→  FastAPI Bridge  ─[UNIX socket]─→  C Daemon
```

### Tech Stack

A fixed list of libraries and versions.

### Roadmap

A table of phases and status:

| Phase | Status |
|---|---|
| Phase 1 — Shell + design | Done |
| Phase 2 — FastAPI bridge + control_server | Done |
| Phase 3A — Config persistence | Done |
| Phase 3B — Daemon supervision | Done (v1.3.0) |
| Phase 4 — Feature pages | Done |
| Phase 5 — Analytics + CSV | Done |
| Polish — Cosmetic pass | Pending |

---

## Appendix A — Glossary

| Term | Definition |
|---|---|
| **FEC (Forward Error Correction)** | Sending more data than strictly required so the receiver can correct losses without asking for retransmission. |
| **Wirehair** | A specific fountain code library — takes K source symbols and produces K+M output symbols. The decoder can reconstruct from any K of them. |
| **Block** | A logical unit of K source + M parity symbols = K+M total. |
| **Symbol** | The atomic unit on the wire. Each symbol carries an 18-byte header + payload (`symbolSize`). |
| **Burst** | A run of consecutive lost symbols. The FEC withstands bursts up to `M × depth`. |
| **Interleaver** | A matrix (`depth` rows × `K+M` columns) that scrambles symbols during transmission so symbols from the same block do not appear consecutively on the wire — a physical loss burst spreads across multiple blocks rather than ruining one. |
| **Deinterleaver** | The receiver-side counterpart — it collects symbols off the wire and restores them to their original blocks. Responsible for evicting late symbols. |
| **CRC-32C (Castagnoli)** | Polynomial `0x82F63B78`. A 4-byte check over each symbol's payload. When enabled, mismatched symbols are dropped as erasures. |
| **AF_UNIX socket** | A local file-based socket. The daemon's telemetry server opens one at `/tmp/fso_gw.sock`. |
| **WebSocket** | A bidirectional protocol layered over HTTP. The GUI connects to the Bridge's `/ws/live` and receives snapshots. |
| **netem** | The Linux Network Emulator. A qdisc component that injects latency, loss, reorder, etc. The GUI uses it via `tc` to inject loss. |
| **Gilbert-Elliott model** | A two-state loss model ("good"/"bad") with transition probabilities between them. Produces more realistic bursts than uniform random loss. |
| **proxy-ARP** | A technique where the daemon answers ARP requests for IPs across the link, allowing client LANs to communicate without explicit routing. |

---

## Appendix B — Counter source map

A quick reference: every value visible in the GUI, mapped to its origin in C.

### TX side

| GUI field | C counter | File:line | Function |
|---|---|---|---|
| TX throughput (Mbps/pps) | `transmitted_packets`, `transmitted_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_transmitted()` |
| | | [src/tx_pipeline.c:554](../../src/tx_pipeline.c) | call site at FSO TX |
| ingress (not directly displayed; part of stats) | `ingress_packets`, `ingress_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_ingress()` |
| | | [src/tx_pipeline.c:311](../../src/tx_pipeline.c) | call site when packet enters from LAN |

### RX side

| GUI field | C counter | File:line | Function |
|---|---|---|---|
| RX throughput | `recovered_packets`, `recovered_bytes` | [src/stats.c](../../src/stats.c) | `stats_inc_recovered()` |
| Recovered Packets | same | [src/rx_pipeline.c:463,502,529](../../src/rx_pipeline.c) | after successful reassembly |
| Failed Packets | `failed_packets` | [src/stats.c](../../src/stats.c) | `stats_inc_failed_packet()` |

### Symbols

| GUI field | C counter | File:line |
|---|---|---|
| Symbol Loss Ratio | `lost_symbols / total_symbols` | [src/stats.c:441-476](../../src/stats.c) — `stats_record_symbol(bool lost)` |
| CRC Drops | `symbols_dropped_crc` | [src/stats.c:485](../../src/stats.c) — `stats_inc_crc_drop_symbol()` |

### Blocks (FEC)

| GUI field | C counter | File:line |
|---|---|---|
| Blocks Attempted | `blocks_attempted` | [src/stats.c:470-474](../../src/stats.c) — `stats_inc_block_attempt()` |
| Blocks Recovered | `blocks_recovered` | [src/stats.c:475-479](../../src/stats.c) — `stats_inc_block_success()` |
| Blocks Failed | `blocks_failed` | [src/stats.c:480-484](../../src/stats.c) — `stats_inc_block_failure()` |
| Quality % | computed: `blocks_recovered / blocks_attempted × 100` | [gateway_source.py snapshot()](../server/gateway_source.py) |

### Bursts

| GUI field | C counter | File:line |
|---|---|---|
| Burst histogram (7 bins) | `burst_len_1`, `_2_5`, `_6_10`, `_11_50`, `_51_100`, `_101_500`, `_501_plus` | [src/stats.c:50-56](../../src/stats.c) |
| Recoverable Bursts | `recoverable_bursts` | [src/stats.c:60](../../src/stats.c), line 162 |
| Critical Bursts | `critical_bursts` | [src/stats.c:61](../../src/stats.c), line 157 |
| Exceeding FEC Span | `bursts_exceeding_fec_span` | [src/stats.c:58](../../src/stats.c), line 158 |
| Max Burst Length | `max_burst_length` | [src/stats.c](../../src/stats.c) — updated in `stats_close_current_burst()` |
| Total Bursts | `total_bursts` | [src/stats.c](../../src/stats.c) |

### Decoder Stress

| GUI field | C counter | File:line |
|---|---|---|
| Blocks with Loss | `blocks_with_loss` | [src/stats.c:66](../../src/stats.c) — `stats_record_block(holes)` |
| Total Holes | `total_holes_in_blocks` | [src/stats.c:68](../../src/stats.c) |
| Worst Holes / Block | `worst_holes_in_block` | [src/stats.c:68](../../src/stats.c) — atomic max |

### Deinterleaver Stats (`dil_stats`)

Source: [src/deinterleaver.c](../../src/deinterleaver.c), struct in [include/deinterleaver.h](../../include/deinterleaver.h).

| GUI field | C field | When updated |
|---|---|---|
| Active | `active_blocks` | Blocks in FILLING + READY_TO_DECODE. |
| Ready | `ready_count` | Blocks in READY_TO_DECODE only. |
| Blocks ready (cumulative) | `blocks_ready` | Cumulative. |
| Failed timeout | `blocks_failed_timeout` | Block left because not enough symbols arrived in 50 ms. |
| Failed holes | `blocks_failed_holes` | Block left because >M holes. |
| Dropped duplicate | `dropped_symbols_duplicate` | Same `fec_id` arrived twice. |
| Dropped frozen | `dropped_symbols_frozen` | Symbol arrived for a block not in FILLING. |
| Dropped erasure | `dropped_symbols_erasure` | Symbol with `is_erasure=1`. |
| Dropped CRC fail | `dropped_symbols_crc_fail` | CRC failed. |
| Evicted filling | `evicted_filling_blocks` | Slot evicted in FILLING. |
| Evicted done | `evicted_done_blocks` | Slot evicted in READY_TO_DECODE. |

### ARP Cache

Source: [src/arp_cache.c](../../src/arp_cache.c), struct in [include/arp_cache.h](../../include/arp_cache.h).

| GUI field | C field |
|---|---|
| ip / mac | `struct arp_entry { uint32_t ip; uint8_t mac[6]; uint64_t last_seen_ms; }` |

### Block Events (FEC Lifecycle)

Source: callbacks from [src/deinterleaver.c](../../src/deinterleaver.c) → ring buffer in [src/control_server.c](../../src/control_server.c).

| GUI reason | C enum |
|---|---|
| SUCCESS | `DIL_BLOCK_FINAL_REASON_SUCCESS` |
| DECODE_FAILED | `DIL_BLOCK_FINAL_REASON_DECODE_FAILED` |
| TIMEOUT | `DIL_BLOCK_FINAL_REASON_TIMEOUT` |
| TOO_MANY_HOLES | `DIL_BLOCK_FINAL_REASON_TOO_MANY_HOLES` |
| EVICTED_FILLING | `DIL_BLOCK_FINAL_REASON_EVICTED_FILLING` |
| EVICTED_READY | `DIL_BLOCK_FINAL_REASON_EVICTED_READY` |

### Config Echo

| GUI field | C source | CLI flag |
|---|---|---|
| `configEcho.k` | `cfg.k` | `--k` |
| `configEcho.m` | `cfg.m` | `--m` |
| `configEcho.depth` | `cfg.depth` | `--depth` |
| `configEcho.symbolSize` | `cfg.symbol_size` | `--symbol-size` |
| `configEcho.lanIface` | `cfg.lan_iface` | `--lan-iface` |
| `configEcho.fsoIface` | `cfg.fso_iface` | `--fso-iface` |
| `configEcho.internalSymbolCrc` | `cfg.internal_symbol_crc_enabled` | `--internal-symbol-crc 1` |

The config is passed to `control_server` in `gateway_create()` ([src/gateway.c](../../src/gateway.c)) and is included in every snapshot built by [src/control_server.c](../../src/control_server.c) `build_snapshot_json()`.

### Computed fields (not in C)

| GUI field | Computation | Where |
|---|---|---|
| Quality % | `100 × blocks_recovered / blocks_attempted` | [gateway_source.py snapshot()](../server/gateway_source.py) |
| txBps / rxBps | `(bytes_delta × 8) / dt_seconds` | gateway_source.py — `_ingest()` |
| txPps / rxPps | `packets_delta / dt_seconds` | same |
| Symbol Loss Ratio | `lost_symbols / total_symbols` | gateway_source.py snapshot() |
| Block Fail Rate | `blocks_failed / blocks_attempted` | same |
| FEC Success Rate | `(blocks_recovered / blocks_attempted) × 100` | client-side in [ErrorMetrics.tsx](../src/components/dashboard/ErrorMetrics.tsx) |
| Link Utilization | `(txBps + rxBps) / 20Gbps × 100` | client-side, fixed 10Gbps per direction |
| Burst Coverage | `burstWithinSpan / totalBursts × 100` | client-side on the Interleaver page |
| Avg Packet Size | `txBps / 8 / txPps` | client-side |
| Symbols each (approx) | `avgPacketSize / symbolSize` | client-side |

### Mock / Fabricated fields

⚠️ The following are *not* backed by C counters — either computed in the browser or simulated:

| Field | Where | Origin |
|---|---|---|
| Daemon state | TopBar, Configuration | `DaemonSupervisor` in the Bridge — reflects supervisor intent, not the C process. |
| Fade events (Link Status) | Link Status page | Client-side computation on quality history. |
| Sidebar Health 98.7% | Sidebar | hardcoded. |
| Notification badge | TopBar | hardcoded. |
| Mock alerts | Alerts page (when daemon is down) | random in `main.py:_maybe_emit_alert()`. |
| Mock logs | Logs page (when daemon is down) | generated in [log_source.py](../server/log_source.py). |
| Mock ARP entries | Topology (when daemon is down) | fixed list in `main.py`. |

---

## Common troubleshooting

### "Connection: DEMO" while the daemon is running

- Check that the socket exists: `ls -la /tmp/fso_gw.sock`.
- Check the Bridge is connecting: `tail -f /tmp/bridge.log` and look for "gateway connected".
- Make sure no other process is binding the same socket path.

### "Daemon: FAILED" with "binary not found"

- Check `FSO_DAEMON_BINARY` in the Bridge's environment.
- Verify the file exists and has `+x`.
- On Linux you may need `setcap cap_net_raw,cap_net_admin=eip` on the binary — or run with `sudo`.

### "Daemon: FAILED" with "exited immediately rc=N"

- Manually run the same argv shown in `status.command` to see the stderr error.
- For `fso_gw_runner` a common cause: the iface does not exist, or the process lacks permissions for `pcap_open`.

### Channel page errors ("tc: command not found" / "permission denied")

- Verify `tc` is on the Bridge's PATH.
- For the FSO iface: either run the Bridge as root, or add a sudoers entry like `NOPASSWD: /sbin/tc`.

### Throughput charts show zero

- Confirm there is real traffic on LAN. Run `iperf3` from Win-1 to Win-2 to verify.
- In demo mode (`control_server_demo`) the throughput rate is synthetic and independent of real traffic.

---

**Document version:** 1.0  
**Compatible with GUI:** `webgui-v1.3.0`  
**Schema:** `fso-gw-stats/2`  
**Last updated:** 2026-04-25
