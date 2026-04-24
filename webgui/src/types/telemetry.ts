/**
 * Telemetry contract — mirrors (will mirror) the struct stats_container in src/stats.c.
 * During Phase 1 these values come from mockTelemetry. In Phase 2 they will come from
 * the FastAPI bridge over WebSocket.
 */

export type LinkState = "online" | "degraded" | "offline";

export interface LinkStatus {
  state: LinkState;
  qualityPct: number; // 0-100
  rssiDbm: number | null; // null = sensor not available
  snrDb: number | null;
  berEstimate: number | null;
  latencyMsAvg: number;
  latencyMsMax: number;
  uptimeSec: number;
}

export interface ThroughputSample {
  t: number; // epoch ms
  txBps: number;
  rxBps: number;
  txPps: number;
  rxPps: number;
}

export interface ErrorMetrics {
  ber: number | null; // estimated from lost_symbols / total_symbols
  flrPct: number; // failed-packet loss rate (0-1)
  crcDrops: number;
  recoveredPackets: number;
  lostBlocks: number;
  blocksAttempted: number;
  blocksRecovered: number;
  blocksFailed: number;
}

export interface PipelineStageStats {
  name: string;
  queueDepth: number;
  processingUs: number;
  throughputPps: number;
  healthy: boolean;
}

export interface BurstHistogramBucket {
  label: string;
  count: number;
}

export interface SystemInfo {
  version: string;
  build: string;
  configProfile: string;
  gatewayId: string;
  firmware: string;
  cpuPct: number;
  memoryPct: number;
  temperatureC: number;
  fpgaAccel: boolean;
}

/** Raw FEC/interleaver diagnostics from the C stats_container. */
export interface DecoderStress {
  blocksWithLoss: number;
  worstHolesInBlock: number;
  totalHolesInBlocks: number;
  recoverableBursts: number;
  criticalBursts: number;
  burstsExceedingFecSpan: number;
}

/** Echo of the live gateway config (from control_server or mock). */
export interface ConfigEcho {
  k: number;
  m: number;
  depth: number;
  symbolSize: number;
  lanIface: string;
  fsoIface: string;
  internalSymbolCrc: boolean;
}

export interface AlertEvent {
  id: string;
  t: number;
  severity: "info" | "warning" | "critical";
  module: string;
  message: string;
}

export interface TelemetrySnapshot {
  /** Origin marker set by whoever produced this snapshot. */
  source?: "mock" | "gateway" | "mock-local";
  /** Server-side timestamp (epoch ms) when snapshot was generated. */
  generatedAt?: number;
  link: LinkStatus;
  throughput: ThroughputSample[]; // ring buffer
  errors: ErrorMetrics;
  pipeline: PipelineStageStats[];
  burstHistogram: BurstHistogramBucket[];
  system: SystemInfo;
  alerts: AlertEvent[];
  /** Optional deep FEC diagnostics. Present when the gateway control_server
   *  is connected or the mock source is seeded with stress values. */
  decoderStress?: DecoderStress;
  /** Optional echo of the currently-running config (from control_server). */
  configEcho?: ConfigEcho;
}

export type ConnectionStatus = "connecting" | "live" | "demo";

export interface TelemetryFeed {
  snapshot: TelemetrySnapshot | null;
  connection: ConnectionStatus;
}

export interface FecConfig {
  k: number;
  m: number;
  depth: number;
  symbolSize: number;
  flushTimeoutMs: number;
  internalSymbolCrc: boolean;
  lanIface: string;
  fsoIface: string;
}
