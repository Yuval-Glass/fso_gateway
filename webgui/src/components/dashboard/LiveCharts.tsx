"use client";

import dynamic from "next/dynamic";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { ChartZoomModal } from "@/components/primitives/ChartZoomModal";
import type { TelemetrySnapshot } from "@/types/telemetry";
import { useMemo, useState } from "react";
import type { EChartsOption } from "echarts";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

type ChartId = "throughput" | "pps" | "burst" | "stress";

interface ChartMeta {
  title: string;
  href: string;
  hintId: "traffic.txBps" | "traffic.txPps" | "burst.histogram" | "stress.blocksWithLoss";
}

const CHART_META: Record<ChartId, ChartMeta> = {
  throughput: { title: "Throughput Over Time", href: "/traffic",       hintId: "traffic.txBps" },
  pps:        { title: "Packet Rate (pps)",    href: "/traffic",       hintId: "traffic.txPps" },
  burst:      { title: "Burst-Length Distribution", href: "/fec-analytics", hintId: "burst.histogram" },
  stress:     { title: "Cumulative Counters",  href: "/fec-analytics", hintId: "stress.blocksWithLoss" },
};

const AXIS_COLOR = "rgba(255,255,255,0.06)";
const TEXT_MUTED = "#566377";
const CYAN = "#00d4ff";
const BLUE = "#5aa0ff";
const RED = "#ff2d5c";
const AMBER = "#ffb020";

const baseAxis = {
  axisLine: { lineStyle: { color: AXIS_COLOR } },
  axisTick: { lineStyle: { color: AXIS_COLOR } },
  splitLine: { lineStyle: { color: AXIS_COLOR } },
  axisLabel: { color: TEXT_MUTED, fontSize: 10, fontFamily: "var(--font-mono)" },
};

const baseOpts: Partial<EChartsOption> = {
  grid: { left: 52, right: 16, top: 20, bottom: 28 },
  textStyle: { fontFamily: "var(--font-sans)" },
  // Smooth tweening between snapshots — match the bridge tick (~200 ms by
  // default). Linear easing reads as continuous motion rather than bouncy.
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

export function LiveCharts({ snap }: { snap: TelemetrySnapshot }) {
  const throughputOpt = useMemo(() => buildThroughputOption(snap), [snap]);
  const ppsOpt = useMemo(() => buildPpsOption(snap), [snap]);
  const burstOpt = useMemo(() => buildBurstOption(snap), [snap]);
  const symbolLossOpt = useMemo(() => buildSymbolLossOption(snap), [snap]);
  const [zoomed, setZoomed] = useState<ChartId | null>(null);

  const optFor = (id: ChartId): EChartsOption => {
    if (id === "throughput") return throughputOpt;
    if (id === "pps") return ppsOpt;
    if (id === "burst") return burstOpt;
    return symbolLossOpt;
  };

  return (
    <>
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <GlassPanel
          label={CHART_META.throughput.title}
          hintId={CHART_META.throughput.hintId}
          labelHref={CHART_META.throughput.href}
          onBodyClick={() => setZoomed("throughput")}
          trailing={<RangePill />}
        >
          <ReactECharts option={throughputOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
        </GlassPanel>
        <GlassPanel
          label={CHART_META.pps.title}
          hintId={CHART_META.pps.hintId}
          labelHref={CHART_META.pps.href}
          onBodyClick={() => setZoomed("pps")}
          trailing={<RangePill />}
        >
          <ReactECharts option={ppsOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
        </GlassPanel>
        <GlassPanel
          label={CHART_META.burst.title}
          hintId={CHART_META.burst.hintId}
          labelHref={CHART_META.burst.href}
          onBodyClick={() => setZoomed("burst")}
        >
          <ReactECharts option={burstOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
        </GlassPanel>
        <GlassPanel
          label={CHART_META.stress.title}
          hintId={CHART_META.stress.hintId}
          labelHref={CHART_META.stress.href}
          onBodyClick={() => setZoomed("stress")}
        >
          <ReactECharts option={symbolLossOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
        </GlassPanel>
      </div>

      {zoomed && (
        <ChartZoomModal
          title={CHART_META[zoomed].title}
          href={CHART_META[zoomed].href}
          onClose={() => setZoomed(null)}
        >
          <ReactECharts
            option={optFor(zoomed)}
            style={{ height: "100%", width: "100%" }}
            notMerge={false}
            lazyUpdate
          />
        </ChartZoomModal>
      )}
    </>
  );
}

function RangePill() {
  return (
    <span className="text-[10px] font-mono tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] px-2 py-1 rounded border border-[color:var(--color-border-hair)]">
      Live
    </span>
  );
}

function buildThroughputOption(snap: TelemetrySnapshot): EChartsOption {
  const samples = snap.throughput.slice(-180);
  return {
    ...baseOpts,
    legend: {
      show: true, top: 0, right: 8, itemWidth: 8, itemHeight: 8,
      textStyle: { color: TEXT_MUTED, fontSize: 10 },
      data: ["TX Mbps", "RX Mbps"],
    },
    xAxis: {
      type: "category",
      data: samples.map((s) => new Date(s.t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })),
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, showMaxLabel: true, interval: Math.floor(samples.length / 6) },
    },
    yAxis: { type: "value", name: "Mbps", nameTextStyle: { color: TEXT_MUTED, fontSize: 10 }, ...baseAxis },
    series: [
      {
        name: "TX Mbps", type: "line", smooth: true, showSymbol: false,
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
        name: "RX Mbps", type: "line", smooth: true, showSymbol: false,
        data: samples.map((s) => +(s.rxBps / 1e6).toFixed(1)),
        lineStyle: { color: BLUE, width: 1.5 },
      },
    ],
  };
}

function buildPpsOption(snap: TelemetrySnapshot): EChartsOption {
  const samples = snap.throughput.slice(-180);
  return {
    ...baseOpts,
    legend: {
      show: true, top: 0, right: 8, itemWidth: 8, itemHeight: 8,
      textStyle: { color: TEXT_MUTED, fontSize: 10 },
      data: ["TX pps", "RX pps"],
    },
    xAxis: {
      type: "category",
      data: samples.map((s) => new Date(s.t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })),
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.floor(samples.length / 6) },
    },
    yAxis: { type: "value", name: "pps", nameTextStyle: { color: TEXT_MUTED, fontSize: 10 }, ...baseAxis },
    series: [
      {
        name: "TX pps", type: "line", smooth: true, showSymbol: false,
        data: samples.map((s) => Math.round(s.txPps)),
        lineStyle: { color: CYAN, width: 1.6 },
      },
      {
        name: "RX pps", type: "line", smooth: true, showSymbol: false,
        data: samples.map((s) => Math.round(s.rxPps)),
        lineStyle: { color: AMBER, width: 1.4, type: "dashed" },
      },
    ],
  };
}

// Bin extents (inclusive on both sides). 501+ is open-ended — we cap the
// visualization at 5000 so the bar has finite width on a log axis.
const BURST_BIN_RANGES: Array<[number, number]> = [
  [1, 1],
  [2, 5],
  [6, 10],
  [11, 50],
  [51, 100],
  [101, 500],
  [501, 5000],
];
// Geometric mean of each bin — the natural center on a log axis.
const BURST_BIN_CENTERS = BURST_BIN_RANGES.map(([lo, hi]) => Math.sqrt(lo * hi));
const BURST_X_MIN = 0.7;
const BURST_X_MAX = 6000;

function buildBurstOption(snap: TelemetrySnapshot): EChartsOption {
  const counts = snap.burstHistogram.map((b) => b.count);
  const total = counts.reduce((a, b) => a + b, 0) || 1;
  return {
    ...baseOpts,
    tooltip: {
      trigger: "item",
      backgroundColor: "rgba(13, 19, 32, 0.95)",
      borderColor: "rgba(0, 212, 255, 0.3)",
      textStyle: { color: "#e8f1ff", fontSize: 11 },
      formatter: (params: unknown) => {
        const p = (Array.isArray(params) ? params[0] : params) as { dataIndex: number };
        const i = p.dataIndex;
        const real = counts[i] ?? 0;
        const pct = (real / total) * 100;
        const label = snap.burstHistogram[i].label;
        return `burst ${label}<br/><b>${real.toLocaleString()}</b> events<br/>${pct.toFixed(2)}% of all bursts`;
      },
    },
    xAxis: {
      type: "log",
      logBase: 10,
      min: BURST_X_MIN,
      max: BURST_X_MAX,
      name: "burst length (symbols, log)",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      nameLocation: "middle",
      nameGap: 24,
      ...baseAxis,
    },
    yAxis: {
      // Log scale so a 10000:1 ratio between common short bursts and rare
      // long ones still shows up as visible bars instead of a flat line.
      type: "log",
      name: "count (log)",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
      min: 1,
    },
    series: [
      {
        type: "custom",
        renderItem: (_params, api) => {
          const v = api.value(1) as number;
          const i = api.value(2) as number;
          const [lo, hi] = BURST_BIN_RANGES[i];
          // Each bar spans the bin's actual extent on the log X axis (with
          // a tiny inset so adjacent bars don't touch), and rises from y=1
          // to the count on the log Y axis.
          const xLo = Math.max(BURST_X_MIN, lo === hi ? lo * 0.85 : lo);
          const xHi = Math.min(BURST_X_MAX, lo === hi ? lo * 1.15 : hi);
          const left   = api.coord([xLo, 1])[0];
          const right  = api.coord([xHi, 1])[0];
          const top    = api.coord([1, Math.max(1, v)])[1];
          const bottom = api.coord([1, 1])[1];
          const w = Math.max(2, right - left - 2);
          const h = bottom - top;
          const colors = [CYAN, CYAN, BLUE, BLUE, AMBER, AMBER, RED];
          return {
            type: "rect",
            shape: { x: left + 1, y: top, width: w, height: h, r: [3, 3, 0, 0] },
            style: { fill: colors[i] },
          };
        },
        encode: { x: 0, y: 1, tooltip: [0, 1] },
        data: snap.burstHistogram.map((b, i) => [BURST_BIN_CENTERS[i], Math.max(1, b.count), i]),
      },
    ],
  };
}

function buildSymbolLossOption(snap: TelemetrySnapshot): EChartsOption {
  // Show cumulative: total_symbols on LHS axis, lost_symbols on RHS (log-friendly
  // via secondary axis). Values come straight from real counters.
  const total = snap.decoderStress.totalHolesInBlocks;
  const withLoss = snap.decoderStress.blocksWithLoss;
  const recoverable = snap.decoderStress.recoverableBursts;
  const critical = snap.decoderStress.criticalBursts;
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: ["Blocks w/ loss", "Total holes", "Recoverable bursts", "Critical bursts"],
      ...baseAxis,
    },
    yAxis: { type: "value", ...baseAxis },
    series: [
      {
        type: "bar",
        data: [
          { value: withLoss,    itemStyle: { color: BLUE, borderRadius: [3, 3, 0, 0] } },
          { value: total,       itemStyle: { color: AMBER, borderRadius: [3, 3, 0, 0] } },
          { value: recoverable, itemStyle: { color: CYAN, borderRadius: [3, 3, 0, 0] } },
          { value: critical,    itemStyle: { color: RED, borderRadius: [3, 3, 0, 0] } },
        ],
        barWidth: "50%",
      },
    ],
  };
}
