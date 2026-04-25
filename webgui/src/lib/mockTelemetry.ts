/**
 * Last-resort local mock — used only when the bridge WebSocket is
 * unreachable. Matches the same TelemetrySnapshot schema the bridge
 * emits (which in turn mirrors the real C counters).
 *
 * Kept deliberately tiny: seven counters walking up at a steady rate,
 * no synthesized optical telemetry, no per-stage queue depths. If the
 * bridge comes back up the UI switches away from this within 3 seconds.
 */

import type { TelemetrySnapshot } from "@/types/telemetry";

const HISTORY_SAMPLES = 300;

interface Sim {
  lastT: number;
  txBps: number;
  rxBps: number;
  ingressPkts: number;
  txPkts: number;
  rxPkts: number;
  blocksAttempted: number;
  blocksRecovered: number;
  blocksFailed: number;
  lostSymbols: number;
  totalSymbols: number;
  crcDrops: number;
  history: TelemetrySnapshot["throughput"];
  uptimeStartMs: number;
}

let state: Sim | null = null;

function fresh(): Sim {
  const now = Date.now();
  return {
    lastT: now,
    txBps: 40e6,
    rxBps: 38e6,
    ingressPkts: 0,
    txPkts: 0,
    rxPkts: 0,
    blocksAttempted: 0,
    blocksRecovered: 0,
    blocksFailed: 0,
    lostSymbols: 0,
    totalSymbols: 0,
    crcDrops: 0,
    history: [],
    uptimeStartMs: now,
  };
}

function clamp(v: number, lo: number, hi: number) {
  return Math.max(lo, Math.min(hi, v));
}
function walk(v: number, step: number, lo: number, hi: number) {
  return clamp(v + (Math.random() - 0.5) * step * 2, lo, hi);
}

const K = 8, M = 4, DEPTH = 2, SYMBOL_SIZE = 1500;

function step(s: Sim) {
  const now = Date.now();
  const dtS = Math.max(0.001, (now - s.lastT) / 1000);

  s.txBps = walk(s.txBps, 1.5e6, 20e6, 55e6);
  s.rxBps = walk(s.rxBps, 1.5e6, 18e6, 52e6);

  const avgPkt = 1200;
  const txPps = s.txBps / 8 / avgPkt;
  const rxPps = s.rxBps / 8 / avgPkt;
  const dTxPkts = Math.floor(txPps * dtS);
  const dRxPkts = Math.floor(rxPps * dtS);
  s.ingressPkts += dTxPkts;
  // ~K+M wire symbols per ingress packet (for avg_pkt < symbol_size)
  s.txPkts += Math.floor(dTxPkts * (K + M) / K);
  s.rxPkts += dRxPkts;

  const blocksThisTick = Math.max(1, Math.floor(dRxPkts));
  s.blocksAttempted += blocksThisTick;
  const fails = Math.floor(blocksThisTick * 0.001 + Math.random());
  s.blocksFailed += fails;
  s.blocksRecovered += blocksThisTick - fails;
  s.totalSymbols += blocksThisTick * (K + M);
  s.lostSymbols += fails * (M + 1) + (Math.random() < 0.1 ? 1 : 0);
  if (Math.random() < 0.05) s.crcDrops++;

  s.history.push({
    t: now,
    txBps: s.txBps,
    rxBps: s.rxBps,
    txPps,
    rxPps,
  });
  if (s.history.length > HISTORY_SAMPLES) s.history.shift();

  s.lastT = now;
}

export function snapshot(): TelemetrySnapshot {
  if (!state) state = fresh();
  step(state);
  const s = state;

  const qualityPct = s.blocksAttempted > 0
    ? (s.blocksRecovered / s.blocksAttempted) * 100
    : 100;
  const linkState: TelemetrySnapshot["link"]["state"] =
    qualityPct > 99.5 ? "online" : qualityPct > 95 ? "degraded" : "offline";

  return {
    source: "mock-local",
    generatedAt: Date.now(),
    link: {
      state: linkState,
      qualityPct,
      uptimeSec: (Date.now() - s.uptimeStartMs) / 1000,
    },
    throughput: [...s.history],
    errors: {
      symbolLossRatio: s.totalSymbols > 0 ? s.lostSymbols / s.totalSymbols : null,
      blockFailRatio: s.blocksAttempted > 0 ? s.blocksFailed / s.blocksAttempted : 0,
      crcDrops: s.crcDrops,
      recoveredPackets: s.rxPkts,
      failedPackets: s.blocksFailed,
      blocksAttempted: s.blocksAttempted,
      blocksRecovered: s.blocksRecovered,
      blocksFailed: s.blocksFailed,
    },
    burstHistogram: [
      { label: "1",       count: Math.floor(s.blocksAttempted * 0.002) },
      { label: "2-5",     count: s.blocksFailed },
      { label: "6-10",    count: 0 },
      { label: "11-50",   count: 0 },
      { label: "51-100",  count: 0 },
      { label: "101-500", count: 0 },
      { label: "501+",    count: 0 },
    ],
    decoderStress: {
      blocksWithLoss: s.blocksFailed + Math.floor(s.blocksRecovered * 0.001),
      worstHolesInBlock: s.blocksFailed > 0 ? M + 1 : 0,
      totalHolesInBlocks: s.lostSymbols,
      recoverableBursts: Math.floor(s.blocksRecovered * 0.001),
      criticalBursts: 0,
      burstsExceedingFecSpan: 0,
      configuredFecBurstSpan: M * DEPTH,
    },
    configEcho: {
      k: K, m: M, depth: DEPTH, symbolSize: SYMBOL_SIZE,
      lanIface: "enp1s0f0np0", fsoIface: "enp1s0f1np1",
      internalSymbolCrc: true,
    },
    alerts: [],
    dilStats: {
      droppedDuplicate: 0,
      droppedFrozen: s.blocksAttempted * 3,
      droppedErasure: s.lostSymbols,
      droppedCrcFail: s.crcDrops,
      evictedFilling: 0,
      evictedDone: 0,
      blocksReady: s.blocksRecovered,
      blocksFailedTimeout: 0,
      blocksFailedHoles: s.blocksFailed,
      activeBlocks: 0,
      readyCount: 0,
    },
    arpEntries: [],
    blockEvents: [],
  };
}

export const SAMPLE_RATE_MS = 1000;
