"use client";

import dynamic from "next/dynamic";
import type { EChartsOption } from "echarts";
import { useMemo, useState } from "react";
import { Activity, ArrowDownLeft, ArrowUpRight, Gauge, Package, Zap } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { ChartZoomModal } from "@/components/primitives/ChartZoomModal";
import { FieldHint } from "@/components/primitives/FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
import { useTelemetry } from "@/lib/useTelemetry";
import { formatBitrate, formatNumber } from "@/lib/utils";
import type { ThroughputSample } from "@/types/telemetry";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

const CYAN = "#00d4ff";
const BLUE = "#5aa0ff";
const AMBER = "#ffb020";
const AXIS_COLOR = "rgba(255,255,255,0.06)";
const TEXT_MUTED = "#566377";

const baseAxis = {
  axisLine: { lineStyle: { color: AXIS_COLOR } },
  axisTick: { lineStyle: { color: AXIS_COLOR } },
  splitLine: { lineStyle: { color: AXIS_COLOR } },
  axisLabel: { color: TEXT_MUTED, fontSize: 10, fontFamily: "var(--font-mono)" },
};

const baseOpts: Partial<EChartsOption> = {
  grid: { left: 58, right: 16, top: 28, bottom: 30 },
  textStyle: { fontFamily: "var(--font-sans)" },
  animation: true,
  animationDuration: 100,
  animationDurationUpdate: 100,
  animationEasing: "linear",
  animationEasingUpdate: "linear",
  tooltip: {
    trigger: "axis",
    backgroundColor: "rgba(13, 19, 32, 0.95)",
    borderColor: "rgba(0, 212, 255, 0.3)",
    textStyle: { color: "#e8f1ff", fontSize: 11 },
    axisPointer: { lineStyle: { color: "rgba(0, 212, 255, 0.4)" } },
  },
};

const LINK_CAPACITY_BPS = 10e9; // 10 Gbps per direction — adjust when we know the spec

type TrafficChartId = "throughput" | "pps";

export default function TrafficPage() {
  const { snapshot: snap } = useTelemetry();
  const [zoomed, setZoomed] = useState<TrafficChartId | null>(null);

  const throughputChart = useMemo(
    () => (snap ? buildThroughputOption(snap.throughput) : null),
    [snap?.throughput],
  );
  const ppsChart = useMemo(
    () => (snap ? buildPpsOption(snap.throughput) : null),
    [snap?.throughput],
  );

  if (!snap || !throughputChart || !ppsChart) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading traffic telemetry…
        </div>
      </div>
    );
  }

  const hist = snap.throughput;
  const latest = hist[hist.length - 1];

  // Peaks and averages over the whole history window
  const txPeakBps = Math.max(0, ...hist.map((s) => s.txBps));
  const rxPeakBps = Math.max(0, ...hist.map((s) => s.rxBps));
  const txAvgBps  = avg(hist.map((s) => s.txBps));
  const rxAvgBps  = avg(hist.map((s) => s.rxBps));
  const txPpsPeak = Math.max(0, ...hist.map((s) => s.txPps));
  const rxPpsPeak = Math.max(0, ...hist.map((s) => s.rxPps));
  const txSparkMbps = hist.slice(-60).map((s) => s.txBps / 1e6);
  const rxSparkMbps = hist.slice(-60).map((s) => s.rxBps / 1e6);

  const txUtil = latest.txBps / LINK_CAPACITY_BPS;
  const rxUtil = latest.rxBps / LINK_CAPACITY_BPS;

  const txRate = formatBitrate(latest.txBps);
  const rxRate = formatBitrate(latest.rxBps);
  const txPeak = formatBitrate(txPeakBps);
  const rxPeak = formatBitrate(rxPeakBps);
  const txAvg  = formatBitrate(txAvgBps);
  const rxAvg  = formatBitrate(rxAvgBps);

  // Packet size distribution from observed bytes/packets (no real per-packet sizes
  // in the snapshot; derive avg sizes client-side and bucket against standard MTU
  // thresholds so the visual is meaningful even on mock data).
  const avgTxPkt = latest.txPps > 0 ? latest.txBps / 8 / latest.txPps : 0;
  const avgRxPkt = latest.rxPps > 0 ? latest.rxBps / 8 / latest.rxPps : 0;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Traffic Monitor
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Per-direction throughput, packet rates, peaks and averages over the last
            few minutes.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4F · Traffic Insights
        </div>
      </div>

      {/* Direction cards */}
      <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
        <DirectionCard
          direction="TX"
          nowBps={latest.txBps}
          avgBps={txAvgBps}
          peakBps={txPeakBps}
          nowPps={latest.txPps}
          peakPps={txPpsPeak}
          avgPktBytes={avgTxPkt}
          utilization={txUtil}
          spark={txSparkMbps}
          rate={txRate}
          peak={txPeak}
          avg={txAvg}
        />
        <DirectionCard
          direction="RX"
          nowBps={latest.rxBps}
          avgBps={rxAvgBps}
          peakBps={rxPeakBps}
          nowPps={latest.rxPps}
          peakPps={rxPpsPeak}
          avgPktBytes={avgRxPkt}
          utilization={rxUtil}
          spark={rxSparkMbps}
          rate={rxRate}
          peak={rxPeak}
          avg={rxAvg}
        />
      </div>

      {/* Charts */}
      <GlassPanel
        label="Throughput — TX & RX"
        hintId="traffic.txBps"
        onBodyClick={() => setZoomed("throughput")}
        trailing={
          <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            <Legend color={CYAN} label="TX Mbps" />
            <Legend color={BLUE} label="RX Mbps" />
            <span>· {hist.length} samples</span>
          </div>
        }
      >
        <ReactECharts option={throughputChart} style={{ height: 240 }} notMerge={false} lazyUpdate />
      </GlassPanel>

      <GlassPanel
        label="Packet Rate — pps"
        hintId="traffic.txPps"
        onBodyClick={() => setZoomed("pps")}
        trailing={
          <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            <Legend color={CYAN} label="TX pps" />
            <Legend color={AMBER} label="RX pps" />
          </div>
        }
      >
        <ReactECharts option={ppsChart} style={{ height: 240 }} notMerge={false} lazyUpdate />
      </GlassPanel>

      {/* Hero strip: totals */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <MetricCard
          label="Combined Throughput"
          value={formatBitrate(latest.txBps + latest.rxBps).value}
          unit={formatBitrate(latest.txBps + latest.rxBps).unit}
          tone="cyan"
          icon={<Gauge size={14} />}
          hintId="traffic.combined"
          sub={<span className="text-[color:var(--color-text-secondary)]">TX + RX, current second</span>}
        />
        <MetricCard
          label="Combined PPS"
          value={formatNumber(Math.round(latest.txPps + latest.rxPps))}
          unit="pps"
          icon={<Zap size={14} />}
          hintId="traffic.combinedPps"
          sub={
            <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
              Peak: {formatNumber(Math.round(Math.max(txPpsPeak, rxPpsPeak)))}
            </span>
          }
        />
        <MetricCard
          label="Avg TX Packet"
          value={avgTxPkt > 0 ? avgTxPkt.toFixed(0) : "—"}
          unit="bytes"
          icon={<Package size={14} />}
          hintId="traffic.avgPacket"
          sub={<span className="text-[color:var(--color-text-secondary)]">Derived from bps/pps</span>}
          tone={avgTxPkt > 1500 ? "cyan" : "neutral"}
        />
        <MetricCard
          label="Peak Utilization"
          value={(Math.max(txPeakBps, rxPeakBps) / LINK_CAPACITY_BPS * 100).toFixed(1)}
          unit="%"
          icon={<Activity size={14} />}
          hintId="traffic.peakUtil"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              vs {formatBitrate(LINK_CAPACITY_BPS).value} {formatBitrate(LINK_CAPACITY_BPS).unit} link
            </span>
          }
          tone={Math.max(txPeakBps, rxPeakBps) / LINK_CAPACITY_BPS > 0.8 ? "warning" : "cyan"}
        />
      </div>

      {zoomed && (
        <ChartZoomModal
          title={zoomed === "throughput" ? "Throughput — TX & RX" : "Packet Rate — pps"}
          onClose={() => setZoomed(null)}
        >
          <ReactECharts
            option={zoomed === "throughput" ? throughputChart : ppsChart}
            style={{ height: "100%", width: "100%" }}
            notMerge={false}
            lazyUpdate
          />
        </ChartZoomModal>
      )}
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Sub-components                                                              */
/* -------------------------------------------------------------------------- */

function DirectionCard({
  direction,
  nowBps,
  avgBps,
  peakBps,
  nowPps,
  peakPps,
  avgPktBytes,
  utilization,
  spark,
  rate,
  peak,
  avg,
}: {
  direction: "TX" | "RX";
  nowBps: number;
  avgBps: number;
  peakBps: number;
  nowPps: number;
  peakPps: number;
  avgPktBytes: number;
  utilization: number;
  spark: number[];
  rate: { value: string; unit: string };
  peak: { value: string; unit: string };
  avg: { value: string; unit: string };
}) {
  const utilPct = Math.min(100, utilization * 100);
  const utilTone = utilPct > 85 ? "warning" : utilPct > 50 ? "cyan" : "success";
  const utilColor =
    utilTone === "warning" ? "var(--color-warning)"
    : utilTone === "success" ? "var(--color-success)" : "var(--color-cyan-300)";
  const Arrow = direction === "TX" ? ArrowUpRight : ArrowDownLeft;

  return (
    <GlassPanel variant="raised" padded={false} className="overflow-hidden">
      <div className="px-5 py-4 flex items-start justify-between">
        <div>
          <div className="flex items-center gap-2">
            <Arrow size={14} className="text-[color:var(--color-cyan-300)]" />
            <span className="text-[10px] font-semibold tracking-[0.25em] uppercase text-[color:var(--color-cyan-300)] inline-flex items-center gap-1">
              <span>{direction} Direction</span>
              <FieldHint id={direction === "TX" ? "traffic.txBps" : "traffic.rxBps"} size={10} />
            </span>
          </div>
          <div className="mt-2 flex items-baseline gap-1.5">
            <span className="font-display text-3xl font-bold tabular text-[color:var(--color-cyan-300)]"
              style={{ textShadow: "0 0 24px rgba(0,212,255,0.35)" }}>
              {rate.value}
            </span>
            <span className="text-xs text-[color:var(--color-text-muted)]">{rate.unit}</span>
          </div>
          <div className="mt-1 font-mono text-[11px] tabular text-[color:var(--color-text-secondary)]">
            {formatNumber(Math.round(nowPps))} pps
          </div>
        </div>
        <div className="text-right">
          <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
            <span>Utilization</span>
            <FieldHint id="traffic.utilization" size={10} />
          </div>
          <div className="font-display text-xl font-semibold tabular mt-0.5"
            style={{ color: utilColor }}>
            {utilPct.toFixed(1)}%
          </div>
          <div className="mt-2 h-1 w-24 rounded-full overflow-hidden bg-white/5">
            <div
              className="h-full rounded-full transition-all"
              style={{
                width: `${utilPct}%`,
                background: `linear-gradient(90deg, ${utilColor}, ${utilColor}aa)`,
                boxShadow: `0 0 8px ${utilColor}88`,
              }}
            />
          </div>
        </div>
      </div>

      {/* Inline sparkline */}
      <div className="px-5 pb-2">
        <MiniSpark values={spark} />
      </div>

      {/* Sub-stats */}
      <div className="grid grid-cols-3 gap-2 px-5 pb-4">
        <SubTile label="Peak" value={`${peak.value} ${peak.unit}`} hintId="traffic.peakRate" />
        <SubTile label="Avg" value={`${avg.value} ${avg.unit}`} hintId="traffic.avgRate" />
        <SubTile label="Peak PPS" value={formatNumber(Math.round(peakPps))} hintId="traffic.peakPps" />
      </div>
      <div className="px-5 pb-4 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
        Avg packet size:{" "}
        <span className="font-mono text-[color:var(--color-text-primary)]">
          {avgPktBytes > 0 ? `${avgPktBytes.toFixed(0)}B` : "—"}
        </span>
      </div>
    </GlassPanel>
  );
}

function SubTile({ label, value, hintId }: { label: string; value: string; hintId?: FieldHintId }) {
  return (
    <div className="glass rounded px-2 py-1.5">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </div>
      <div className="font-mono text-xs tabular text-[color:var(--color-text-primary)] mt-0.5 truncate">
        {value}
      </div>
    </div>
  );
}

function MiniSpark({ values }: { values: number[] }) {
  if (values.length < 2) return <div className="h-10" />;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min || 1;
  const w = 100;
  const h = 40;
  const step = w / (values.length - 1);
  const points = values.map((v, i) => {
    const x = i * step;
    const y = h - ((v - min) / range) * (h - 2) - 1;
    return `${x.toFixed(2)},${y.toFixed(2)}`;
  });
  const path = `M${points.join(" L")}`;
  const area = `${path} L${w},${h} L0,${h} Z`;
  const gid = `traffic-spark-${Math.random().toString(36).slice(2, 9)}`;
  return (
    <svg viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" className="w-full h-10 block">
      <defs>
        <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%"   stopColor={CYAN} stopOpacity="0.4" />
          <stop offset="100%" stopColor={CYAN} stopOpacity="0" />
        </linearGradient>
      </defs>
      <path d={area} fill={`url(#${gid})`} />
      <path d={path} fill="none" stroke={CYAN} strokeWidth="1" vectorEffect="non-scaling-stroke" />
    </svg>
  );
}

function Legend({ color, label }: { color: string; label: string }) {
  return (
    <span className="inline-flex items-center gap-1.5">
      <span className="w-2 h-2 rounded-sm" style={{ background: color, boxShadow: `0 0 6px ${color}` }} />
      {label}
    </span>
  );
}

/* -------------------------------------------------------------------------- */
/* Helpers + chart builders                                                    */
/* -------------------------------------------------------------------------- */

function avg(a: number[]): number {
  if (a.length === 0) return 0;
  let s = 0;
  for (const v of a) s += v;
  return s / a.length;
}

function buildThroughputOption(samples: ThroughputSample[]): EChartsOption {
  const labels = samples.map((s) =>
    new Date(s.t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }),
  );
  return {
    ...baseOpts,
    legend: { show: false },
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.floor(samples.length / 6) },
    },
    yAxis: {
      type: "value",
      name: "Mbps",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        name: "TX Mbps",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +(s.txBps / 1e6).toFixed(1)),
        lineStyle: { color: CYAN, width: 2 },
        areaStyle: {
          color: {
            type: "linear", x: 0, y: 0, x2: 0, y2: 1,
            colorStops: [
              { offset: 0, color: "rgba(0, 212, 255, 0.35)" },
              { offset: 1, color: "rgba(0, 212, 255, 0)" },
            ],
          },
        },
      },
      {
        name: "RX Mbps",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +(s.rxBps / 1e6).toFixed(1)),
        lineStyle: { color: BLUE, width: 1.5 },
      },
    ],
  };
}

function buildPpsOption(samples: ThroughputSample[]): EChartsOption {
  const labels = samples.map((s) =>
    new Date(s.t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }),
  );
  return {
    ...baseOpts,
    legend: { show: false },
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.floor(samples.length / 6) },
    },
    yAxis: {
      type: "value",
      name: "pps",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        name: "TX pps",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => Math.round(s.txPps)),
        lineStyle: { color: CYAN, width: 1.6 },
      },
      {
        name: "RX pps",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => Math.round(s.rxPps)),
        lineStyle: { color: AMBER, width: 1.4, type: "dashed" },
      },
    ],
  };
}
