"use client";

import dynamic from "next/dynamic";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import type { TelemetrySnapshot } from "@/types/telemetry";
import { useMemo } from "react";
import type { EChartsOption } from "echarts";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

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

  return (
    <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
      <GlassPanel label="Throughput Over Time" trailing={<RangePill />}>
        <ReactECharts option={throughputOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
      </GlassPanel>
      <GlassPanel label="Packet Rate (pps)" trailing={<RangePill />}>
        <ReactECharts option={ppsOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
      </GlassPanel>
      <GlassPanel label="Burst-Length Distribution">
        <ReactECharts option={burstOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
      </GlassPanel>
      <GlassPanel label="Cumulative Counters">
        <ReactECharts option={symbolLossOpt} style={{ height: 220 }} notMerge={false} lazyUpdate />
      </GlassPanel>
    </div>
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

function buildBurstOption(snap: TelemetrySnapshot): EChartsOption {
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: snap.burstHistogram.map((b) => b.label),
      name: "burst length (symbols)",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      nameLocation: "middle",
      nameGap: 24,
      ...baseAxis,
    },
    yAxis: {
      type: "value",
      name: "count",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        type: "bar",
        data: snap.burstHistogram.map((b, i) => ({
          value: b.count,
          itemStyle: {
            color: i < 2 ? CYAN : i < 4 ? BLUE : i < 6 ? AMBER : RED,
            borderRadius: [3, 3, 0, 0],
          },
        })),
        barWidth: "60%",
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
