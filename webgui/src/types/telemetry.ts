/**
 * Telemetry contract — mirrors the atomic counters in include/stats.h plus
 * the live config echo emitted by src/control_server.c.
 *
 * Every field below is backed by a real counter or a real configuration value
 * in the C daemon. There are NO placeholder fabrications — if a metric does
 * not map to something the daemon actually tracks (e.g. optical RSSI/SNR,
 * per-packet latency), it is not part of this type.
 */

export type LinkState = "online" | "degraded" | "offline";

export interface LinkStatus {
  /** Derived client-side from the FEC block failure rate. */
  state: LinkState;
  /** 100 * blocks_recovered / blocks_attempted. 100 when no blocks yet. */
  qualityPct: number;
  /** Seconds since the bridge started observing this gateway session. */
  uptimeSec: number;
}

export interface ThroughputSample {
  t: number;       // epoch ms
  txBps: number;   // derived from transmitted_bytes deltas (wire symbols, incl. FEC overhead)
  rxBps: number;   // derived from recovered_bytes deltas (reassembled LAN packets)
  txPps: number;   // transmitted_packets deltas (per-symbol on FSO)
  rxPps: number;   // recovered_packets deltas (per reassembled Ethernet frame)
  // Per-tick FEC block deltas (rate per second). Optional because older
  // bridge versions / mock fallbacks may not include them.
  blocksAttempted?: number;
  blocksRecovered?: number;
  blocksFailed?: number;
}

export interface ErrorMetrics {
  /** lost_symbols / total_symbols — packet-erasure ratio on FSO. */
  symbolLossRatio: number | null;
  /** blocks_failed / blocks_attempted. */
  blockFailRatio: number;
  /** symbols_dropped_crc — symbols rejected by per-symbol CRC-32C. */
  crcDrops: number;
  /** recovered_packets — Ethernet frames re-emitted on LAN by RX pipeline. */
  recoveredPackets: number;
  /** failed_packets — reassembly failures (FEC-decoded but malformed). */
  failedPackets: number;
  /** blocks_attempted — FEC decode attempts. */
  blocksAttempted: number;
  blocksRecovered: number;
  blocksFailed: number;
}

export interface BurstHistogramBucket {
  label: string;
  count: number;
}

/** Matches struct config in the C daemon, echoed by control_server. */
export interface ConfigEcho {
  k: number;
  m: number;
  depth: number;
  symbolSize: number;
  lanIface: string;
  fsoIface: string;
  internalSymbolCrc: boolean;
}

export interface DecoderStress {
  blocksWithLoss: number;
  worstHolesInBlock: number;
  totalHolesInBlocks: number;
  recoverableBursts: number;
  criticalBursts: number;
  burstsExceedingFecSpan: number;
  /** configured_fec_burst_span = m × depth, in symbols. */
  configuredFecBurstSpan: number;
}

export interface AlertEvent {
  id: string;
  t: number;
  severity: "info" | "warning" | "critical";
  module: string;
  message: string;
}

/** Live counters from include/deinterleaver.h dil_stats_t plus the live
 *  active/ready slot counts. Source: deinterleaver_get_stats(). */
export interface DilStats {
  droppedDuplicate: number;
  droppedFrozen: number;
  droppedErasure: number;
  droppedCrcFail: number;
  evictedFilling: number;
  evictedDone: number;
  blocksReady: number;
  blocksFailedTimeout: number;
  blocksFailedHoles: number;
  activeBlocks: number;
  readyCount: number;
}

/** Snapshot row from arp_cache_dump(). */
export interface ArpEntry {
  ip: string;
  mac: string;
  lastSeenMs: number;
}

/** A block lifecycle event emitted by the deinterleaver's
 *  block_final or eviction callback, drained on each control_server tick. */
export type BlockEventReason =
  | "SUCCESS"
  | "DECODE_FAILED"
  | "TIMEOUT"
  | "TOO_MANY_HOLES"
  | "EVICTED_FILLING"
  | "EVICTED_READY"
  | "NONE"
  | "UNKNOWN";

export interface BlockEvent {
  blockId: number;
  t: number;
  reason: BlockEventReason;
  evicted: boolean;
}

export interface TelemetrySnapshot {
  /** Origin marker. */
  source?: "gateway" | "mock" | "mock-local";
  generatedAt?: number;
  link: LinkStatus;
  throughput: ThroughputSample[]; // ring buffer
  errors: ErrorMetrics;
  burstHistogram: BurstHistogramBucket[];
  decoderStress: DecoderStress;
  configEcho: ConfigEcho;
  alerts: AlertEvent[];
  /** Live deinterleaver state — present when fso-gw-stats/2+ schema. */
  dilStats?: DilStats | null;
  /** Learned proxy-ARP peers (IP↔MAC). */
  arpEntries?: ArpEntry[];
  /** Per-block lifecycle events drained from the deinterleaver callbacks. */
  blockEvents?: BlockEvent[];
}

export type ConnectionStatus = "connecting" | "live" | "demo";

export interface TelemetryFeed {
  snapshot: TelemetrySnapshot | null;
  connection: ConnectionStatus;
}
