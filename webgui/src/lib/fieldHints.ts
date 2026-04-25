/**
 * Central registry for field hints (tooltips) across the GUI.
 *
 * Each entry has:
 *   - title:  short label that appears at the top of the tooltip card.
 *   - body:   1–3 sentences explaining what the value means and how to read it.
 *             Use `\n` for paragraph breaks; the tooltip preserves them.
 *   - source: (optional) where the value originates — e.g. C function or formula.
 *             Shown in monospace at the bottom; helpful for engineers.
 *
 * Naming convention: <area>.<field>. Areas group related screens:
 *   topbar.*, sidebar.*, link.*, traffic.*, fec.*, dil.*, burst.*, stress.*,
 *   block.*, interleaver.*, packet.*, channel.*, config.*, daemon.*, alert.*,
 *   topology.*, log.*, run.*, about.*.
 *
 * Add new entries here rather than inline in components — that keeps the GUI
 * documentation centralized and easy to audit.
 */

export interface FieldHint {
  title: string;
  body: string;
  source?: string;
}

// First declare the table to extract its key set, then re-export with a
// uniform value type so optional `source` is a structural option.
const _HINTS = {
  // ---------- Top Bar ----------
  "topbar.connection": {
    title: "Bridge Connection",
    body:
      "LIVE = WebSocket open and the C daemon is streaming on /tmp/fso_gw.sock.\n" +
      "DEMO = bridge is up but the daemon socket is unavailable; values are simulated.\n" +
      "CONNECTING = WebSocket is reconnecting (with backoff).",
    source: "useTelemetry.ts → /ws/live",
  },
  "topbar.system": {
    title: "System State",
    body:
      "Overall link health based on FEC block recovery rate.\n" +
      "OPERATIONAL >99.5% · DEGRADED 95–99.5% · OFFLINE ≤95%.",
    source: "qualityPct = 100 × blocks_recovered / blocks_attempted",
  },
  "topbar.daemon": {
    title: "Daemon State",
    body:
      "State of the gateway process supervised by the bridge.\n" +
      "STOPPED, STARTING, RUNNING, STOPPING, FAILED. Reflects supervisor intent — actual C process state surfaces via the watcher task on unexpected exit.",
    source: "webgui/server/daemon.py — DaemonSupervisor",
  },
  "topbar.uptime": {
    title: "Daemon Uptime",
    body:
      "Time since the daemon opened its telemetry server. NOT host or bridge uptime.",
    source: "control_server.c — elapsed_seconds() (CLOCK_MONOTONIC)",
  },
  "topbar.time": {
    title: "Clock (Israel)",
    body: "Wall clock in the Asia/Jerusalem timezone, sourced from the browser. Refreshes every second; unrelated to the daemon.",
  },

  // ---------- Link / quality ----------
  "link.state": {
    title: "Link State",
    body:
      "Derived from FEC block recovery rate.\n" +
      "ONLINE = qualityPct > 99.5%\nDEGRADED = 95% < qualityPct ≤ 99.5%\nOFFLINE = qualityPct ≤ 95%.",
    source: "gateway_source.py snapshot()",
  },
  "link.qualityPct": {
    title: "Link Quality",
    body:
      "Percentage of FEC blocks that decoded successfully out of those attempted. " +
      "This is the primary measure of link health on this gateway.",
    source: "100 × blocks_recovered / blocks_attempted",
  },
  "link.uptimeSec": {
    title: "Session Uptime",
    body: "Seconds elapsed since the daemon's control_server started.",
    source: "control_server.c — uptime_sec field",
  },

  // ---------- Throughput ----------
  "traffic.txBps": {
    title: "TX Throughput",
    body:
      "Per-second bit rate of symbols transmitted on the FSO interface. " +
      "Counts every symbol sent (K source + M parity), so it includes FEC overhead.",
    source: "(transmitted_bytes_delta × 8) / dt — stats_inc_transmitted() at tx_pipeline.c:554",
  },
  "traffic.txPps": {
    title: "TX Packets / sec",
    body:
      "Per-second rate of symbols pushed onto the FSO wire. " +
      "Each LAN packet becomes K source + M parity symbols, so this is much higher than the LAN packet rate.",
    source: "transmitted_packets delta",
  },
  "traffic.rxBps": {
    title: "RX Throughput",
    body:
      "Per-second bit rate of LAN packets that were successfully reassembled and forwarded out the LAN interface. " +
      "Failed blocks contribute zero to this number — it measures delivered traffic.",
    source: "(recovered_bytes_delta × 8) / dt — stats_inc_recovered() at rx_pipeline.c:463,502,529",
  },
  "traffic.rxPps": {
    title: "RX Packets / sec",
    body: "Per-second rate of fully reassembled LAN packets delivered to the customer side.",
    source: "recovered_packets delta",
  },
  "traffic.utilization": {
    title: "Link Utilization",
    body:
      "(TX + RX) ÷ (10 Gbps × 2). The 10 Gbps cap is an assumption, not a measurement of physical link capacity. " +
      "Useful as a rough comparison, not as a hard ceiling.",
  },
  "traffic.peakRate": {
    title: "Peak Rate",
    body: "Highest throughput observed across the in-memory history window (~300 samples).",
  },
  "traffic.avgRate": {
    title: "Average Rate",
    body: "Mean throughput across the in-memory history window.",
  },
  "traffic.peakPps": {
    title: "Peak Packets / sec",
    body: "Highest packet rate seen across the history window.",
  },
  "traffic.avgPacket": {
    title: "Average Packet Size",
    body: "Bytes per packet, computed from the current bit rate divided by the current packet rate.",
    source: "txBps / 8 / txPps",
  },
  "traffic.combined": {
    title: "Combined Throughput",
    body: "TX + RX bit rate at the latest sample.",
  },
  "traffic.combinedPps": {
    title: "Combined Packets / sec",
    body: "TX + RX packet rate at the latest sample.",
  },
  "traffic.peakUtil": {
    title: "Peak Utilization",
    body: "Highest utilization observed across the history window.",
  },

  // ---------- Errors / FEC top-level ----------
  "errors.symbolLossRatio": {
    title: "Symbol Loss Ratio",
    body:
      "Fraction of symbols that did not arrive at the deinterleaver (lost on the channel).\n" +
      "<0.01% = clean · 0.01–1% = within FEC budget · >5% = expect failures.",
    source: "lost_symbols / total_symbols — stats_record_symbol(bool lost) in stats.c",
  },
  "errors.blockFailRatio": {
    title: "Block Fail Rate",
    body:
      "Fraction of FEC blocks that could not be decoded — either due to timeout or too many holes.",
    source: "blocks_failed / blocks_attempted — stats_inc_block_failure() in deinterleaver.c",
  },
  "errors.crcDrops": {
    title: "CRC Drops",
    body:
      "Number of symbols rejected by per-symbol CRC-32C validation (when enabled). " +
      "A CRC-failed symbol is treated as an erasure — FEC may still recover the block.\n" +
      "If this climbs fast, the link is suffering corruption rather than just loss.",
    source: "symbols_dropped_crc — stats_inc_crc_drop_symbol() in stats.c:485",
  },
  "errors.recoveredPackets": {
    title: "Recovered Packets",
    body: "Cumulative count of LAN packets successfully reassembled and forwarded since daemon start.",
    source: "recovered_packets — stats_inc_recovered() in stats.c",
  },
  "errors.failedPackets": {
    title: "Failed Packets",
    body: "Cumulative count of LAN packets that could not be recovered after FEC decoding.",
    source: "failed_packets — stats_inc_failed_packet()",
  },
  "errors.fecSuccessRate": {
    title: "FEC Success Rate",
    body:
      "Same numerator/denominator as Link Quality — shown here in FEC framing.\n" +
      "Tone: green >99.9%, cyan >99%, amber otherwise.",
    source: "(blocks_recovered / blocks_attempted) × 100",
  },
  "errors.blocksAttempted": {
    title: "Blocks Attempted",
    body:
      "Cumulative count of blocks that entered the FILLING state (a new packet group started arriving).",
    source: "blocks_attempted — stats_inc_block_attempt() in deinterleaver.c",
  },
  "errors.blocksRecovered": {
    title: "Blocks Recovered",
    body: "Cumulative count of blocks that successfully decoded.",
    source: "blocks_recovered — stats_inc_block_success()",
  },
  "errors.blocksFailed": {
    title: "Blocks Failed",
    body:
      "Cumulative count of blocks that could not be decoded — timed out or had more than M holes.",
    source: "blocks_failed — stats_inc_block_failure()",
  },
  "fec.blocksPerSec": {
    title: "Blocks / sec",
    body: "Average block-decode rate over the last 5 samples. Sub-text breaks it into recovered vs. failed.",
  },
  "fec.criticalBursts": {
    title: "Critical Bursts (≥101 symbols)",
    body:
      "Sum of the two longest burst-histogram bins (101–500 and 501+). " +
      "These bursts are typically beyond what FEC can repair.",
    source: "burst_len_101_500 + burst_len_501_plus from stats.c",
  },

  // ---------- Bursts ----------
  "burst.histogram": {
    title: "Burst-Length Distribution",
    body:
      "Histogram of consecutive-symbol-loss runs, bucketed into 7 bins (1, 2-5, 6-10, 11-50, 51-100, 101-500, 501+). " +
      "Bursts up to M × depth are recoverable; longer ones are critical.",
    source: "burst_len_* counters in stats.c:50–56",
  },
  "burst.maxLength": {
    title: "Max Burst Length",
    body: "Longest run of consecutive symbol losses observed since daemon start.",
    source: "max_burst_length — updated in stats_close_current_burst()",
  },

  // ---------- Decoder Stress ----------
  "stress.blocksWithLoss": {
    title: "Blocks with Loss",
    body:
      "Cumulative number of blocks that had at least one missing symbol after deinterleave " +
      "(but may still have decoded thanks to FEC).",
    source: "blocks_with_loss — stats_record_block(holes) in stats.c:66",
  },
  "stress.totalHolesInBlocks": {
    title: "Total Holes",
    body: "Sum of all missing symbols across all blocks since daemon start.",
    source: "total_holes_in_blocks in stats.c:68",
  },
  "stress.worstHolesInBlock": {
    title: "Worst Holes / Block",
    body: "Maximum number of missing symbols ever observed in a single block.",
    source: "worst_holes_in_block (atomic max) in stats.c",
  },
  "stress.recoverableBursts": {
    title: "Recoverable Bursts",
    body: "Bursts whose length is ≤ M × depth — within the FEC repair budget.",
    source: "recoverable_bursts in stats.c:60",
  },
  "stress.criticalBursts": {
    title: "Critical Bursts",
    body: "Bursts that exceeded M × depth and could not have been repaired by FEC.",
    source: "critical_bursts in stats.c:61",
  },
  "stress.exceedingFecSpan": {
    title: "Exceeding FEC Span",
    body: "Identical to Critical Bursts in most cases; differs only in edge conditions.",
    source: "bursts_exceeding_fec_span in stats.c:58",
  },

  // ---------- Deinterleaver internal stats ----------
  "dil.active": {
    title: "Active Slots",
    body:
      "Blocks currently in FILLING + READY_TO_DECODE states — i.e., the deinterleaver's current occupancy.",
    source: "dil_stats.active_blocks in deinterleaver.c",
  },
  "dil.ready": {
    title: "Ready Slots",
    body:
      "Blocks already decoded and waiting for the consumer to drain via deinterleaver_get_ready_block().",
    source: "dil_stats.ready_count",
  },
  "dil.blocksReady": {
    title: "Blocks Ready (cumulative)",
    body: "Cumulative count of blocks that successfully transitioned to READY_TO_DECODE.",
    source: "dil_stats.blocks_ready",
  },
  "dil.failedTimeout": {
    title: "Failed · Timeout",
    body:
      "Blocks discarded because not enough symbols arrived within the timeout window (50 ms by default).",
    source: "dil_stats.blocks_failed_timeout",
  },
  "dil.failedHoles": {
    title: "Failed · Holes",
    body: "Blocks that could not be decoded because more than M symbols were missing.",
    source: "dil_stats.blocks_failed_holes",
  },
  "dil.droppedDuplicate": {
    title: "Dropped · Duplicate",
    body: "Symbols rejected because the same fec_id arrived twice for the same block.",
    source: "dil_stats.dropped_symbols_duplicate",
  },
  "dil.droppedFrozen": {
    title: "Dropped · Frozen",
    body:
      "Symbols that arrived for a block that had already left FILLING (too late to be useful).",
    source: "dil_stats.dropped_symbols_frozen",
  },
  "dil.droppedErasure": {
    title: "Dropped · Erasure",
    body: "Symbols arrived flagged as erasures (is_erasure=1) by the upstream pipeline.",
    source: "dil_stats.dropped_symbols_erasure",
  },
  "dil.droppedCrcFail": {
    title: "Dropped · CRC Fail",
    body: "Symbols rejected by per-symbol CRC-32C check.",
    source: "dil_stats.dropped_symbols_crc_fail",
  },
  "dil.evictedFilling": {
    title: "Evicted · Filling",
    body:
      "Slots evicted from the deinterleaver while still in FILLING state (buffer overflow). " +
      "Should not happen in a healthy pipeline.",
    source: "dil_stats.evicted_filling_blocks",
  },
  "dil.evictedDone": {
    title: "Evicted · Done",
    body:
      "Slots evicted from READY_TO_DECODE without being drained — RX backpressure.",
    source: "dil_stats.evicted_done_blocks",
  },

  // ---------- Block lifecycle event feed ----------
  "block.eventFeed": {
    title: "Block Lifecycle Events",
    body:
      "Stream of final-state transitions in the deinterleaver FSM (last ~128 events). " +
      "Reasons: SUCCESS, DECODE_FAILED, TIMEOUT, TOO_MANY_HOLES, EVICTED_FILLING, EVICTED_READY.",
    source: "block_events ring buffer in control_server.c",
  },

  // ---------- Interleaver page ----------
  "interleaver.matrix": {
    title: "Matrix Dimensions",
    body: "Interleaver matrix shape: depth rows × (K+M) columns.",
  },
  "interleaver.cells": {
    title: "Cell Count",
    body: "Total cells in the interleaver matrix = depth × (K+M).",
  },
  "interleaver.matrixBytes": {
    title: "Matrix Size",
    body: "Total bytes the interleaver buffer occupies = cells × symbol size.",
  },
  "interleaver.recoverySpan": {
    title: "Recovery Span",
    body:
      "Maximum consecutive symbol-loss length the FEC can repair = M × depth. " +
      "Any longer burst is unrecoverable.",
  },
  "interleaver.burstCoverage": {
    title: "Burst Coverage",
    body:
      "Stacked horizontal bar of every burst-length bin observed.\n" +
      "Each segment's width = that bin's share of all bursts.\n" +
      "Cyan segments are within the recovery span (M × depth) — they were FEC-recoverable. Red segments exceed the span and could not have been repaired.",
    source: "snap.burstHistogram + recoverySpanSymbols = m × depth",
  },
  "interleaver.depth": {
    title: "Depth (rows)",
    body:
      "Number of interleaver matrix rows. Higher depth = better burst spread, but more memory and latency.",
    source: "config.depth",
  },
  "interleaver.blockWidth": {
    title: "Block Width",
    body: "Symbols per block = K + M. Number of columns in the matrix.",
  },
  "interleaver.symbolSize": {
    title: "Symbol Size",
    body: "Bytes of payload per symbol on the wire.",
    source: "config.symbol_size",
  },
  "interleaver.blockBytes": {
    title: "Block Size",
    body: "Total bytes a block occupies = (K+M) × symbol size.",
  },

  // ---------- Packet Inspector ----------
  "packet.symbolsProcessed": {
    title: "Symbols Processed",
    body:
      "Estimate of the total source symbols processed since start. " +
      "Computed as blocks_attempted × K (parity symbols not counted).",
  },
  "packet.symbolsPerPacket": {
    title: "Symbols per Packet",
    body: "Average count of symbols a single LAN packet is fragmented into.",
    source: "avg_packet_size / symbol_size",
  },
  "packet.headerSize": {
    title: "Symbol Header",
    body:
      "Fixed 18-byte header on every symbol: packet_id (4) + fec_id (4) + symbol_index (2) + total_symbols (2) + payload_len (2) + crc32 (4).",
    source: "include/symbol.h",
  },
  "packet.totalOnWire": {
    title: "Total per Symbol on Wire",
    body: "18-byte header + symbol size payload = full symbol size on the FSO wire.",
  },

  // ---------- Channel ----------
  "channel.iface": {
    title: "Channel Interface",
    body: "The interface where netem will inject loss. Typically the FSO-side iface.",
  },
  "channel.available": {
    title: "tc Availability",
    body:
      "Whether the bridge can run `tc qdisc show` against this iface. " +
      "Requires `tc` on PATH and CAP_NET_ADMIN (root or sudoers entry).",
  },
  "channel.active": {
    title: "Netem Active",
    body: "True if a netem qdisc is currently attached to the iface.",
  },
  "channel.model": {
    title: "Loss Model",
    body:
      "gemodel = Gilbert-Elliott (good/bad state machine, realistic bursts). " +
      "uniform = each packet independently dropped at lossPct.",
  },
  "channel.enterPct": {
    title: "Enter % (Burst Entry)",
    body:
      "Probability of transitioning from the 'good' to the 'bad' Gilbert-Elliott state per packet. Higher = more frequent burst onset.",
  },
  "channel.exitPct": {
    title: "Exit % (Burst Exit)",
    body:
      "Probability of leaving the 'bad' state per packet. Higher = shorter burst durations.",
  },
  "channel.lossPct": {
    title: "Loss % (in bad state)",
    body: "Packet loss rate while in the 'bad' state. Combined with enter/exit, defines burst severity.",
  },
  "channel.applyBtn": {
    title: "Apply",
    body:
      "Runs `tc qdisc replace dev <iface> root netem loss gemodel <enter>% <exit>% <loss>%` via the bridge.",
    source: "webgui/server/channel.py — apply_gemodel()",
  },
  "channel.clearBtn": {
    title: "Clear",
    body: "Runs `tc qdisc del dev <iface> root` to remove any active loss model.",
  },

  // ---------- Configuration ----------
  "config.k": {
    title: "K — Source Symbols",
    body:
      "Number of original (data) symbols per FEC block. " +
      "Higher K = lower overhead, more latency. Range 4–256.",
    source: "config.k → fso_gw_runner --k",
  },
  "config.m": {
    title: "M — Repair Symbols",
    body:
      "Number of parity (repair) symbols per block. " +
      "Higher M = better recovery; can repair up to M lost symbols per block. Range 1–256.",
    source: "config.m → fso_gw_runner --m",
  },
  "config.overhead": {
    title: "FEC Overhead",
    body: "Redundancy as a fraction of total symbols.",
    source: "M / (K + M)",
  },
  "config.codeRate": {
    title: "Code Rate",
    body: "Information efficiency: data symbols out of total symbols.",
    source: "K / (K + M)",
  },
  "config.blockSize": {
    title: "Block Size",
    body: "Total bytes a block occupies on the wire.",
    source: "(K + M) × symbol_size",
  },
  "config.burstRecovery": {
    title: "Burst Recovery (approx)",
    body: "Approximate maximum consecutive symbol-loss length the FEC can repair = M × depth.",
  },
  "config.depth": {
    title: "Depth (rows)",
    body:
      "Number of interleaver matrix rows. Higher depth = better burst spread, but more memory and end-to-end latency.",
    source: "config.depth → fso_gw_runner --depth",
  },
  "config.symbolSize": {
    title: "Symbol Size",
    body:
      "Payload bytes per symbol. Larger = lower overhead per packet, but each lost symbol costs more bytes.",
    source: "config.symbol_size → fso_gw_runner --symbol-size",
  },
  "config.lanIface": {
    title: "LAN Interface",
    body:
      "Name of the network interface for client traffic. The daemon opens it via libpcap to capture frames.",
    source: "config.lan_iface → fso_gw_runner --lan-iface",
  },
  "config.fsoIface": {
    title: "FSO Interface",
    body: "Name of the network interface used as the FSO link. Encoded symbols go out here.",
    source: "config.fso_iface → fso_gw_runner --fso-iface",
  },
  "config.internalSymbolCrc": {
    title: "Internal Symbol CRC-32C",
    body:
      "Per-symbol Castagnoli CRC. When enabled, mismatched symbols are dropped before reaching FEC. " +
      "Adds 4 bytes per symbol; provides robustness against link corruption.",
    source: "config.internal_symbol_crc → fso_gw_runner --internal-symbol-crc 1",
  },
  "config.applyBtn": {
    title: "Apply Changes",
    body:
      "Validates and persists the draft to webgui/server/config.yaml. " +
      "The running daemon picks up changes only on restart.",
  },
  "config.revertBtn": {
    title: "Revert",
    body: "Discards client-side draft changes back to the last saved configuration.",
  },
  "config.statusDot": {
    title: "Configuration Status",
    body:
      "Green = in sync with bridge. Cyan = unsaved changes. Amber = saved but daemon must restart. Red = error.",
  },

  // ---------- Daemon supervision ----------
  "daemon.startBtn": {
    title: "Start Gateway",
    body:
      "Launches the configured daemon binary as a subprocess with current YAML config. " +
      "Fails if the binary is missing or exits within 200 ms.",
    source: "POST /api/daemon/start — DaemonSupervisor.start()",
  },
  "daemon.restartBtn": {
    title: "Restart",
    body: "Stops then starts. Used after applying config changes.",
    source: "POST /api/daemon/restart",
  },
  "daemon.stopBtn": {
    title: "Stop",
    body:
      "Sends SIGTERM to the process group, waits up to 4 seconds, then SIGKILL.",
    source: "POST /api/daemon/stop",
  },
  "daemon.pid": {
    title: "PID",
    body: "Process ID of the running daemon subprocess. Empty when stopped.",
  },
  "daemon.uptime": {
    title: "Daemon Uptime",
    body: "Time since the supervisor launched the current process.",
  },
  "daemon.sudo": {
    title: "Sudo Mode",
    body:
      "If yes, the supervisor prepends `sudo -n` when launching. Required in production for raw pcap capture.",
    source: "FSO_DAEMON_SUDO=1 env var",
  },
  "daemon.binary": {
    title: "Binary Status",
    body:
      "found = the configured executable exists and has +x. missing = path is wrong or not executable.",
    source: "FSO_DAEMON_BINARY env var (default: fso_gw_runner)",
  },

  // ---------- Alerts ----------
  "alert.severity": {
    title: "Severity",
    body:
      "critical = something failed (e.g., FEC blocks failed). warning = degraded but recoverable. info = informational only.",
  },
  "alert.module": {
    title: "Module",
    body:
      "Subsystem that emitted the alert: LINK, FEC, CRC, BURST, ARP, CONFIG. Used for filtering.",
  },
  "alert.timestamp": {
    title: "Alert Time",
    body: "When the alert fired (epoch milliseconds).",
  },
  "alert.acked": {
    title: "Acknowledged",
    body:
      "Whether the user has dismissed this alert. Stored in browser localStorage — does not affect server state.",
  },

  // ---------- Topology ----------
  "topology.beam": {
    title: "FSO Beam",
    body:
      "Animated visualization of the link. Color = link state (green/amber/red). Particle density scales with packet rate.",
  },
  "topology.arpEntries": {
    title: "ARP Cache Entries",
    body:
      "IP↔MAC mappings learned by the daemon's arp_cache. Used for proxy-ARP toward LAN clients.",
    source: "src/arp_cache.c — arp_cache_learn()",
  },
  "topology.arpIp": {
    title: "Peer IP",
    body: "IPv4 address of a learned LAN-side peer.",
  },
  "topology.arpMac": {
    title: "Peer MAC",
    body: "Layer-2 MAC of the learned peer.",
  },
  "topology.arpLastSeen": {
    title: "Last Seen",
    body: "Time since the entry was last refreshed. TTL is 5 minutes — expired entries drop from the cache.",
  },
  "topology.recentEvents": {
    title: "Recent Link Events",
    body: "Filtered view of the alert stream — only the latest entries are shown here.",
  },

  // ---------- Logs ----------
  "log.level": {
    title: "Log Level",
    body: "Severity from the C daemon: DEBUG / INFO / WARN / ERROR.",
    source: "src/logging.c — LOG_* macros",
  },
  "log.module": {
    title: "Log Module",
    body: "Subsystem that produced the log line: tx_pipeline, deinterleaver, control_server, etc.",
  },
  "log.export": {
    title: "Export Logs",
    body: "Not implemented yet.",
  },

  // ---------- Analytics / Runs ----------
  "run.activeRun": {
    title: "Active Recording",
    body:
      "If set, the bridge is sampling telemetry to SQLite at 1 Hz under this run ID. New Run starts a fresh recording.",
    source: "webgui/server/run_store.py",
  },
  "run.duration": {
    title: "Run Duration",
    body: "Wall-clock seconds between run start and end. NULL while still recording.",
  },
  "run.sampleCount": {
    title: "Samples Recorded",
    body: "Number of 1 Hz snapshots persisted to the SQLite samples table for this run.",
  },
  "run.exportCsv": {
    title: "Export CSV",
    body:
      "Downloads the run's samples as CSV (t, txBps, rxBps, txPps, rxPps, qualityPct, …). " +
      "Useful for offline analysis.",
    source: "GET /api/runs/{id}/export.csv",
  },
  "run.peakThroughput": {
    title: "Peak Throughput",
    body: "Maximum txBps / rxBps observed during the run.",
  },
  "run.avgThroughput": {
    title: "Average Throughput",
    body: "Mean txBps / rxBps over the run's samples.",
  },
  "run.minQuality": {
    title: "Minimum Quality",
    body: "Lowest qualityPct observed during the run.",
  },
  "run.avgQuality": {
    title: "Average Quality",
    body: "Mean qualityPct over the run's samples.",
  },

  // ---------- About ----------
  "about.bridge": {
    title: "Bridge",
    body: "Health of the FastAPI bridge process. 'Connected' means the GUI can reach /health.",
    source: "GET /health — useHealth()",
  },
  "about.gateway": {
    title: "Gateway Source",
    body:
      "live = streaming from the C daemon's UNIX socket. mock = bridge's SimState fallback (synthetic values).",
  },
  "about.recording": {
    title: "Recording State",
    body:
      "Run #N = a run is being recorded to SQLite. Otherwise 'Not recording'. New Run on Analytics page starts one.",
  },
  "about.tickHz": {
    title: "Bridge Tick Rate",
    body: "How often the bridge advances and broadcasts a snapshot (1 Hz default).",
  },
} satisfies Record<string, FieldHint>;

export type FieldHintId = keyof typeof _HINTS;

export const FIELD_HINTS: Record<FieldHintId, FieldHint> = _HINTS;

/** Look up a hint by id; return undefined if missing (no throw). */
export function getFieldHint(id: FieldHintId): FieldHint {
  return FIELD_HINTS[id];
}
