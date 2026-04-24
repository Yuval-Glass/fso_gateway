"use client";

import dynamic from "next/dynamic";
import { useMemo } from "react";
import type { EChartsOption } from "echarts";
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  ShieldX,
  Target,
  TriangleAlert,
  Zap,
} from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { useTelemetry } from "@/lib/useTelemetry";
import { useFecHistory } from "@/lib/useFecHistory";
import { cn, formatBytes, formatNumber, formatPercent } from "@/lib/utils";
import type { TelemetrySnapshot } from "@/types/telemetry";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

const CYAN = "#00d4ff";
const BLUE = "#5aa0ff";
const RED = "#ff2d5c";
const AMBER = "#ffb020";
const GREEN = "#34d399";
const AXIS_COLOR = "rgba(255,255,255,0.06)";
const TEXT_MUTED = "#566377";

const baseAxis = {
  axisLine: { lineStyle: { color: AXIS_COLOR } },
  axisTick: { lineStyle: { color: AXIS_COLOR } },
  splitLine: { lineStyle: { color: AXIS_COLOR } },
  axisLabel: { color: TEXT_MUTED, fontSize: 10, fontFamily: "var(--font-mono)" },
};

const baseOpts: Partial<EChartsOption> = {
  grid: { left: 54, right: 16, top: 28, bottom: 30 },
  textStyle: { fontFamily: "var(--font-sans)" },
  tooltip: {
    trigger: "axis",
    backgroundColor: "rgba(13, 19, 32, 0.95)",
    borderColor: "rgba(0, 212, 255, 0.3)",
    textStyle: { color: "#e8f1ff", fontSize: 11 },
    axisPointer: { lineStyle: { color: "rgba(0, 212, 255, 0.4)" } },
  },
};

export default function FecAnalyticsPage() {
  const { snapshot: snap } = useTelemetry();
  const history = useFecHistory(snap);

  // Hooks must be called unconditionally — keep these before any early return.
  const outcomeChart = useMemo(() => buildOutcomesOption(history), [history]);
  const burstChart = useMemo(
    () => (snap ? buildBurstOption(snap) : null),
    [snap?.burstHistogram],
  );

  if (!snap || !burstChart) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading FEC telemetry…
        </div>
      </div>
    );
  }

  const e = snap.errors;
  const recoveryRatePct = e.blocksAttempted > 0
    ? (e.blocksRecovered / e.blocksAttempted) * 100
    : 100;
  const failureRatePct = e.blocksAttempted > 0
    ? (e.blocksFailed / e.blocksAttempted) * 100
    : 0;

  // Current rate (last sample, or last 5 seconds average)
  const recent = history.slice(-5);
  const attemptedRate = avg(recent.map((s) => s.attempted));
  const recoveredRate = avg(recent.map((s) => s.recovered));
  const failedRate    = avg(recent.map((s) => s.failed));
  const spark = history.slice(-60).map((s) => s.attempted);

  const totalBursts = snap.burstHistogram.reduce((a, b) => a + b.count, 0);
  const criticalBuckets = snap.burstHistogram.slice(-3);
  const criticalCount = criticalBuckets.reduce((a, b) => a + b.count, 0);
  const criticalPct = totalBursts > 0 ? (criticalCount / totalBursts) * 100 : 0;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            FEC Analytics
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Wirehair fountain-code recovery diagnostics. Live block outcomes,
            burst-length distribution, and decoder stress metrics.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4C · FEC Deep Dive
        </div>
      </div>

      {/* Hero metric strip */}
      <div className="grid grid-cols-2 lg:grid-cols-5 gap-4">
        <MetricCard
          label="Recovery Rate"
          value={recoveryRatePct.toFixed(3)}
          unit="%"
          tone={recoveryRatePct > 99.9 ? "success" : recoveryRatePct > 99 ? "cyan" : "warning"}
          icon={<CheckCircle2 size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatNumber(e.blocksRecovered)} / {formatNumber(e.blocksAttempted)}
            </span>
          }
          spark={history.slice(-60).map((s) =>
            s.attempted > 0 ? (s.recovered / s.attempted) * 100 : 100,
          )}
        />
        <MetricCard
          label="Blocks / sec"
          value={formatNumber(Math.round(attemptedRate))}
          unit="blk/s"
          tone="cyan"
          icon={<Activity size={14} />}
          sub={
            <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
              {formatNumber(Math.round(recoveredRate))} rec · {formatNumber(Math.round(failedRate * 100) / 100)} fail
            </span>
          }
          spark={spark}
        />
        <MetricCard
          label="Failed Blocks"
          value={formatNumber(e.blocksFailed)}
          tone={e.blocksFailed === 0 ? "success" : failureRatePct > 0.1 ? "danger" : "warning"}
          icon={<TriangleAlert size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatPercent(failureRatePct / 100, 4)} of attempts
            </span>
          }
        />
        <MetricCard
          label="BER (est.)"
          value={e.ber != null ? e.ber.toExponential(1) : "—"}
          tone={e.ber != null && e.ber > 1e-6 ? "warning" : "success"}
          icon={<ShieldX size={14} />}
          sub={<span className="text-[color:var(--color-text-secondary)]">From lost symbols</span>}
        />
        <MetricCard
          label="Critical Bursts"
          value={formatNumber(criticalCount)}
          tone={criticalCount === 0 ? "success" : criticalPct > 0.5 ? "danger" : "warning"}
          icon={<Zap size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatPercent(criticalPct / 100, 2)} of all bursts
            </span>
          }
        />
      </div>

      {/* Middle grid: config + outcomes chart */}
      <div className="grid grid-cols-1 xl:grid-cols-[1fr_2fr] gap-4">
        <FecConfigPanel snap={snap} />
        <GlassPanel
          label="Block Outcomes Over Time"
          trailing={
            <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              <Legend color={CYAN} label="Recovered" />
              <Legend color={RED} label="Failed" />
              <span>· last 2 min</span>
            </div>
          }
        >
          {history.length < 2 ? (
            <div className="h-[240px] flex items-center justify-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
              Collecting deltas…
            </div>
          ) : (
            <ReactECharts option={outcomeChart} style={{ height: 240 }} notMerge={false} lazyUpdate />
          )}
        </GlassPanel>
      </div>

      {/* Burst histogram + stress panel */}
      <div className="grid grid-cols-1 xl:grid-cols-[2fr_1fr] gap-4">
        <GlassPanel
          label="Burst-Length Distribution"
          trailing={
            <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              {formatNumber(totalBursts)} total · cyan=recoverable · red=critical
            </span>
          }
        >
          <ReactECharts option={burstChart} style={{ height: 240 }} notMerge={false} lazyUpdate />
        </GlassPanel>
        <StressPanel snap={snap} />
      </div>

      {/* Derivation notes */}
      <GlassPanel label="Derivations">
        <ul className="text-[11px] space-y-1.5 py-1 text-[color:var(--color-text-secondary)]">
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">recovery_rate</span> =
            blocks_recovered / blocks_attempted
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">ber_est</span> =
            lost_symbols / (total_symbols × symbol_size × 8)
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">critical_burst</span> =
            bursts whose length exceeds the configured FEC span (m × depth symbols).
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">blocks/sec</span> is derived
            client-side from the cumulative counter deltas.
          </li>
        </ul>
      </GlassPanel>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Sub-components                                                              */
/* -------------------------------------------------------------------------- */

function FecConfigPanel({ snap }: { snap: TelemetrySnapshot }) {
  const echo = snap.configEcho;
  const k = echo?.k ?? 0;
  const m = echo?.m ?? 0;
  const total = k + m;
  const overhead = total > 0 ? m / total : 0;
  const codeRate = total > 0 ? k / total : 0;

  return (
    <GlassPanel label="FEC Configuration" trailing={
      <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-cyan-300)]">
        {snap.system.configProfile}
      </span>
    }>
      <div className="grid grid-cols-2 gap-3">
        <ConfigTile label="K · Source" value={k || "—"} />
        <ConfigTile label="M · Repair" value={m || "—"} />
        <ConfigTile
          label="Overhead"
          value={total > 0 ? formatPercent(overhead, 1) : "—"}
          tone="cyan"
        />
        <ConfigTile
          label="Code Rate"
          value={total > 0 ? codeRate.toFixed(3) : "—"}
        />
      </div>
      <div className="mt-4 pt-3 border-t border-[color:var(--color-border-hair)]">
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mb-2">
          System
        </div>
        <ul className="text-[11px] space-y-1.5">
          <KV label="Gateway" value={snap.system.gatewayId} mono />
          <KV label="Firmware" value={snap.system.firmware} mono />
          <KV label="Build" value={snap.system.build} mono />
          <KV label="FPGA Accel" value={snap.system.fpgaAccel ? "Enabled" : "Disabled"}
              tone={snap.system.fpgaAccel ? "success" : "neutral"} />
        </ul>
      </div>
    </GlassPanel>
  );
}

function StressPanel({ snap }: { snap: TelemetrySnapshot }) {
  const s = snap.decoderStress;
  if (!s) {
    return (
      <GlassPanel label="Decoder Stress">
        <div className="py-4 text-[11px] text-[color:var(--color-text-muted)] leading-snug">
          Decoder stress counters unavailable from this source.
        </div>
      </GlassPanel>
    );
  }
  const blocksLoss  = s.blocksWithLoss;
  const worstHoles  = s.worstHolesInBlock;
  const totalHoles  = s.totalHolesInBlocks;
  const recoverable = s.recoverableBursts;
  const critical    = s.criticalBursts;
  const exceeding   = s.burstsExceedingFecSpan;

  return (
    <GlassPanel label="Decoder Stress">
      <ul className="text-[11px] space-y-2 py-1">
        <KV label="Blocks with Loss" value={formatNumber(blocksLoss)} />
        <KV label="Worst Holes / Block" value={formatNumber(worstHoles)}
            tone={worstHoles > 3 ? "warning" : "neutral"} />
        <KV label="Total Holes" value={formatNumber(totalHoles)} />
        <li className="border-t border-[color:var(--color-border-hair)] my-2" />
        <KV label="Recoverable Bursts" value={formatNumber(recoverable)} tone="success" />
        <KV label="Critical Bursts" value={formatNumber(critical)}
            tone={critical > 0 ? "danger" : "neutral"} />
        <KV label="Exceeding FEC Span" value={formatNumber(exceeding)}
            tone={exceeding > 0 ? "danger" : "neutral"} />
      </ul>
      <div className="mt-3 text-[10px] text-[color:var(--color-text-muted)] leading-snug">
        Holes are missing-symbol slots in a block after deinterleave. Bursts
        "exceeding FEC span" are too long for the current (m × depth) window to
        recover — tune those up if this counter climbs.
      </div>
    </GlassPanel>
  );
}

function ConfigTile({
  label,
  value,
  tone = "neutral",
}: {
  label: string;
  value: string | number;
  tone?: "neutral" | "cyan" | "success";
}) {
  const color =
    tone === "cyan"
      ? "var(--color-cyan-300)"
      : tone === "success"
      ? "var(--color-success)"
      : "var(--color-text-primary)";
  return (
    <div className="glass rounded-md px-3 py-2 border-[color:var(--color-border-hair)]">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div className="font-display text-lg font-semibold tabular mt-0.5" style={{ color }}>
        {value}
      </div>
    </div>
  );
}

function KV({
  label,
  value,
  mono,
  tone = "neutral",
}: {
  label: string;
  value: string | number;
  mono?: boolean;
  tone?: "neutral" | "cyan" | "success" | "warning" | "danger";
}) {
  const colorMap: Record<string, string> = {
    neutral: "var(--color-text-primary)",
    cyan: "var(--color-cyan-300)",
    success: "var(--color-success)",
    warning: "var(--color-warning)",
    danger: "var(--color-danger)",
  };
  return (
    <li className="flex items-baseline justify-between gap-3">
      <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </span>
      <span
        className={cn("tabular", mono && "font-mono")}
        style={{ color: colorMap[tone] }}
      >
        {value}
      </span>
    </li>
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
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

function avg(a: number[]): number {
  if (a.length === 0) return 0;
  let s = 0;
  for (const v of a) s += v;
  return s / a.length;
}

function buildOutcomesOption(history: Array<{ t: number; recovered: number; failed: number }>): EChartsOption {
  const labels = history.map((s) =>
    new Date(s.t).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }),
  );
  return {
    ...baseOpts,
    legend: {
      show: false,
    },
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: {
        ...baseAxis.axisLabel,
        interval: Math.max(1, Math.floor(history.length / 6)),
      },
    },
    yAxis: {
      type: "value",
      name: "blocks/s",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        name: "Recovered",
        type: "line",
        stack: "outcomes",
        smooth: true,
        showSymbol: false,
        data: history.map((s) => +s.recovered.toFixed(1)),
        lineStyle: { color: CYAN, width: 1.6 },
        areaStyle: {
          color: {
            type: "linear",
            x: 0, y: 0, x2: 0, y2: 1,
            colorStops: [
              { offset: 0, color: "rgba(0, 212, 255, 0.40)" },
              { offset: 1, color: "rgba(0, 212, 255, 0)" },
            ],
          },
        },
      },
      {
        name: "Failed",
        type: "line",
        stack: "outcomes",
        smooth: true,
        showSymbol: false,
        data: history.map((s) => +s.failed.toFixed(2)),
        lineStyle: { color: RED, width: 1.6 },
        areaStyle: {
          color: {
            type: "linear",
            x: 0, y: 0, x2: 0, y2: 1,
            colorStops: [
              { offset: 0, color: "rgba(255, 45, 92, 0.6)" },
              { offset: 1, color: "rgba(255, 45, 92, 0)" },
            ],
          },
        },
      },
    ],
  };
}

function buildBurstOption(snap: TelemetrySnapshot): EChartsOption {
  const labels = snap.burstHistogram.map((b) => b.label);
  const counts = snap.burstHistogram.map((b) => b.count);
  const total = counts.reduce((a, b) => a + b, 0) || 1;
  return {
    ...baseOpts,
    tooltip: {
      trigger: "axis",
      backgroundColor: "rgba(13, 19, 32, 0.95)",
      borderColor: "rgba(0, 212, 255, 0.3)",
      textStyle: { color: "#e8f1ff", fontSize: 11 },
      formatter: (params: unknown) => {
        const arr = Array.isArray(params) ? params : [params];
        const p = arr[0] as { dataIndex: number; value: number; axisValueLabel?: string };
        const pct = (p.value / total) * 100;
        return `burst ${p.axisValueLabel ?? labels[p.dataIndex]}<br/>
          <b>${formatNumber(p.value)}</b> events<br/>
          ${formatPercent(pct / 100, 2)} of all bursts`;
      },
    },
    grid: { left: 54, right: 16, top: 10, bottom: 48 },
    xAxis: {
      type: "category",
      data: labels,
      name: "burst length (symbols)",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      nameLocation: "middle",
      nameGap: 28,
      ...baseAxis,
    },
    yAxis: {
      type: "log",
      name: "count (log)",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
      min: 1,
    },
    series: [
      {
        type: "bar",
        data: snap.burstHistogram.map((b, i) => ({
          value: Math.max(1, b.count),
          itemStyle: {
            color: i < 2 ? CYAN : i < 4 ? BLUE : i < 6 ? AMBER : RED,
            borderRadius: [3, 3, 0, 0],
          },
        })),
        barWidth: "60%",
        label: {
          show: true,
          position: "top",
          color: TEXT_MUTED,
          fontSize: 10,
          fontFamily: "var(--font-mono)",
          formatter: (p) => {
            const v = typeof p.value === "number" ? p.value : 0;
            return v > 1 ? String(v) : "";
          },
        },
      },
    ],
  };
}

