/**
 * Phase 1 mock telemetry generator.
 * Produces a realistic-feeling stream of data using bounded random walks.
 * Will be replaced by WebSocket feed from FastAPI bridge in Phase 2.
 */

import type {
  AlertEvent,
  BurstHistogramBucket,
  ErrorMetrics,
  LinkStatus,
  PipelineStageStats,
  SystemInfo,
  TelemetrySnapshot,
  ThroughputSample,
} from "@/types/telemetry";

const HISTORY_SAMPLES = 300; // 5 min @ 1Hz

// ---- Random walk primitives --------------------------------------------------

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, v));
}

function walk(current: number, step: number, lo: number, hi: number): number {
  return clamp(current + (Math.random() - 0.5) * step * 2, lo, hi);
}

// ---- Internal simulation state (module-level, persists across calls) ---------

interface SimState {
  lastT: number;
  txBps: number;
  rxBps: number;
  txPps: number;
  rxPps: number;
  history: ThroughputSample[];
  qualityPct: number;
  latencyMs: number;
  blocksAttempted: number;
  blocksRecovered: number;
  blocksFailed: number;
  recoveredPackets: number;
  lostBlocks: number;
  crcDrops: number;
  uptimeStart: number;
  alerts: AlertEvent[];
}

let state: SimState | null = null;

function initState(): SimState {
  const now = Date.now();
  const history: ThroughputSample[] = [];
  for (let i = HISTORY_SAMPLES - 1; i >= 0; i--) {
    history.push({
      t: now - i * 1000,
      txBps: 560e6 + Math.random() * 40e6,
      rxBps: 555e6 + Math.random() * 40e6,
      txPps: 50000 + Math.random() * 3000,
      rxPps: 49500 + Math.random() * 3000,
    });
  }
  return {
    lastT: now,
    txBps: 572e6,
    rxBps: 568e6,
    txPps: 51236,
    rxPps: 51102,
    history,
    qualityPct: 98.7,
    latencyMs: 1.62,
    blocksAttempted: 192_530,
    blocksRecovered: 192_434,
    blocksFailed: 12,
    recoveredPackets: 3_847,
    lostBlocks: 8,
    crcDrops: 46,
    uptimeStart: now - 12 * 86400e3 - 4 * 3600e3 - 32 * 60e3,
    alerts: seedAlerts(now),
  };
}

function seedAlerts(now: number): AlertEvent[] {
  const mk = (
    offsetSec: number,
    severity: AlertEvent["severity"],
    module: string,
    message: string,
  ): AlertEvent => ({
    id: `${now - offsetSec * 1000}-${module}`,
    t: now - offsetSec * 1000,
    severity,
    module,
    message,
  });
  return [
    mk(127, "info", "LINK", "Link quality recovered"),
    mk(144, "warning", "LATENCY", "High latency detected (4.2 ms)"),
    mk(223, "info", "FEC", "FEC recovery rate improved"),
    mk(264, "critical", "BURST", "Burst loss detected — 8 symbols"),
    mk(368, "info", "CONFIG", "Configuration applied"),
    mk(512, "warning", "RX", "Symbol queue depth nearing threshold"),
  ];
}

function step(s: SimState): SimState {
  const now = Date.now();
  const dtMs = now - s.lastT;

  // Throughput random-walks around ~570 Mbps
  s.txBps = walk(s.txBps, 8e6, 420e6, 680e6);
  s.rxBps = walk(s.rxBps, 8e6, 420e6, 680e6);
  s.txPps = walk(s.txPps, 800, 42000, 58000);
  s.rxPps = walk(s.rxPps, 800, 42000, 58000);

  s.history.push({
    t: now,
    txBps: s.txBps,
    rxBps: s.rxBps,
    txPps: s.txPps,
    rxPps: s.rxPps,
  });
  while (s.history.length > HISTORY_SAMPLES) s.history.shift();

  // Link metrics
  s.qualityPct = walk(s.qualityPct, 0.15, 95, 99.9);
  s.latencyMs = walk(s.latencyMs, 0.08, 0.9, 2.6);

  // FEC block counters grow monotonically
  const blocksThisTick = Math.floor(dtMs * 0.08 + Math.random() * 4);
  s.blocksAttempted += blocksThisTick;
  s.blocksRecovered += Math.max(0, blocksThisTick - (Math.random() < 0.03 ? 1 : 0));
  if (Math.random() < 0.02) s.blocksFailed += 1;
  if (Math.random() < 0.15) s.recoveredPackets += Math.floor(Math.random() * 3);
  if (Math.random() < 0.01) s.lostBlocks += 1;
  if (Math.random() < 0.06) s.crcDrops += 1;

  s.lastT = now;
  return s;
}

// ---- Public getters ----------------------------------------------------------

function link(s: SimState): LinkStatus {
  return {
    state: s.qualityPct > 96 ? "online" : s.qualityPct > 88 ? "degraded" : "offline",
    qualityPct: s.qualityPct,
    rssiDbm: -21.3 + (Math.random() - 0.5) * 1.8,
    snrDb: 28.6 + (Math.random() - 0.5) * 1.2,
    berEstimate: 1.2e-9 * (1 + (Math.random() - 0.5) * 0.6),
    latencyMsAvg: s.latencyMs,
    latencyMsMax: s.latencyMs + 1.8 + Math.random() * 1.2,
    uptimeSec: (Date.now() - s.uptimeStart) / 1000,
  };
}

function errors(s: SimState): ErrorMetrics {
  const totalSymbols = s.blocksAttempted * 14;
  const lostSymbols = s.blocksFailed * 14 + s.crcDrops;
  return {
    ber: totalSymbols > 0 ? lostSymbols / (totalSymbols * 8 * 800) : null,
    flrPct: s.blocksAttempted > 0 ? s.blocksFailed / s.blocksAttempted : 0,
    crcDrops: s.crcDrops,
    recoveredPackets: s.recoveredPackets,
    lostBlocks: s.lostBlocks,
    blocksAttempted: s.blocksAttempted,
    blocksRecovered: s.blocksRecovered,
    blocksFailed: s.blocksFailed,
  };
}

function pipeline(s: SimState): PipelineStageStats[] {
  const base = s.txPps;
  return [
    { name: "LAN RX", queueDepth: Math.floor(Math.random() * 12), processingUs: 0.4, throughputPps: base, healthy: true },
    { name: "Fragment", queueDepth: Math.floor(Math.random() * 8), processingUs: 0.8, throughputPps: base * 1.8, healthy: true },
    { name: "FEC Encode", queueDepth: Math.floor(Math.random() * 24), processingUs: 3.2, throughputPps: base * 1.8, healthy: true },
    { name: "Interleave", queueDepth: Math.floor(Math.random() * 18), processingUs: 1.1, throughputPps: base * 1.8, healthy: true },
    { name: "FSO TX", queueDepth: Math.floor(Math.random() * 10), processingUs: 0.6, throughputPps: base * 1.8, healthy: true },
    { name: "FSO RX", queueDepth: Math.floor(Math.random() * 10), processingUs: 0.5, throughputPps: base * 1.8, healthy: true },
    { name: "Deinterleave", queueDepth: Math.floor(Math.random() * 22), processingUs: 1.3, throughputPps: base * 1.8, healthy: true },
    { name: "FEC Decode", queueDepth: Math.floor(Math.random() * 28), processingUs: 4.1, throughputPps: base * 1.8, healthy: true },
    { name: "Reassemble", queueDepth: Math.floor(Math.random() * 6), processingUs: 0.7, throughputPps: base, healthy: true },
    { name: "LAN TX", queueDepth: Math.floor(Math.random() * 12), processingUs: 0.4, throughputPps: base, healthy: true },
  ];
}

function burstHistogram(): BurstHistogramBucket[] {
  return [
    { label: "1", count: 1240 + Math.floor(Math.random() * 50) },
    { label: "2-5", count: 380 + Math.floor(Math.random() * 20) },
    { label: "6-10", count: 92 + Math.floor(Math.random() * 8) },
    { label: "11-20", count: 34 },
    { label: "21-50", count: 12 },
    { label: "51-100", count: 4 },
    { label: "101-500", count: 1 },
    { label: "501+", count: 0 },
  ];
}

function system(s: SimState): SystemInfo {
  return {
    version: "v3.1.0-phase8",
    build: "a202b70",
    configProfile: "LAB-TEST",
    gatewayId: "FSO-GW-001",
    firmware: "3.1.7",
    cpuPct: 18 + (Math.random() - 0.5) * 6,
    memoryPct: 42 + (Math.random() - 0.5) * 4,
    temperatureC: 48 + (Math.random() - 0.5) * 2,
    fpgaAccel: true,
  };
}

// ---- Public API --------------------------------------------------------------

export function snapshot(): TelemetrySnapshot {
  if (!state) state = initState();
  state = step(state);
  return {
    source: "mock-local",
    generatedAt: Date.now(),
    link: link(state),
    throughput: [...state.history],
    errors: errors(state),
    pipeline: pipeline(state),
    burstHistogram: burstHistogram(),
    system: system(state),
    alerts: state.alerts,
  };
}

export const SAMPLE_RATE_MS = 1000;
