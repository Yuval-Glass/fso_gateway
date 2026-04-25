"use client";

import dynamic from "next/dynamic";
import { useMemo, useState } from "react";
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
import { ChartZoomModal } from "@/components/primitives/ChartZoomModal";
import { FieldHint } from "@/components/primitives/FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
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

type FecChartId = "outcomes" | "burst";

export default function FecAnalyticsPage() {
  const { snapshot: snap } = useTelemetry();
  const history = useFecHistory(snap);
  const [zoomed, setZoomed] = useState<FecChartId | null>(null);

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
          hintId="errors.fecSuccessRate"
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
          hintId="fec.blocksPerSec"
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
          hintId="errors.blocksFailed"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatPercent(failureRatePct / 100, 4)} of attempts
            </span>
          }
        />
        <MetricCard
          label="Symbol Loss"
          value={
            e.symbolLossRatio == null ? "—"
            : e.symbolLossRatio === 0 ? "0"
            : e.symbolLossRatio < 1e-4 ? e.symbolLossRatio.toExponential(1)
            : formatPercent(e.symbolLossRatio, 3)
          }
          tone={
            e.symbolLossRatio == null ? "neutral"
            : e.symbolLossRatio > 1e-3 ? "danger"
            : e.symbolLossRatio > 1e-4 ? "warning"
            : "success"
          }
          icon={<ShieldX size={14} />}
          hintId="errors.symbolLossRatio"
          sub={<span className="text-[color:var(--color-text-secondary)]">lost_symbols / total</span>}
        />
        <MetricCard
          label="Critical Bursts"
          value={formatNumber(criticalCount)}
          tone={criticalCount === 0 ? "success" : criticalPct > 0.5 ? "danger" : "warning"}
          icon={<Zap size={14} />}
          hintId="fec.criticalBursts"
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
          hintId="errors.blocksRecovered"
          onBodyClick={history.length >= 2 ? () => setZoomed("outcomes") : undefined}
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
          hintId="burst.histogram"
          onBodyClick={() => setZoomed("burst")}
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

      {/* Deinterleaver live state + Block lifecycle event feed */}
      <div className="grid grid-cols-1 xl:grid-cols-[1fr_2fr] gap-4">
        <DeinterleaverPanel snap={snap} />
        <BlockEventFeed snap={snap} />
      </div>

      {/* Derivation notes */}
      <GlassPanel label="Derivations">
        <ul className="text-[11px] space-y-1.5 py-1 text-[color:var(--color-text-secondary)]">
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">recovery_rate</span> =
            blocks_recovered / blocks_attempted (both from stats_container).
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">symbol_loss</span> =
            lost_symbols / total_symbols (aggregated from per-block holes).
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">critical_burst</span> =
            bursts whose length exceeds the configured FEC span
            (<span className="font-mono">m × depth</span> symbols).
          </li>
          <li>
            <span className="font-mono text-[color:var(--color-text-primary)]">blocks/sec</span> is derived
            client-side from the cumulative counter deltas.
          </li>
          <li>
            The burst histogram only tracks per-block hole counts; wire-level
            burst patterns across blocks are not instrumented.
          </li>
        </ul>
      </GlassPanel>

      {zoomed && (
        <ChartZoomModal
          title={zoomed === "outcomes" ? "Block Outcomes Over Time" : "Burst-Length Distribution"}
          onClose={() => setZoomed(null)}
        >
          <ReactECharts
            option={zoomed === "outcomes" ? outcomeChart : burstChart}
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
        {echo ? `K=${echo.k} M=${echo.m} depth=${echo.depth}` : "—"}
      </span>
    }>
      <div className="grid grid-cols-2 gap-3">
        <ConfigTile label="K · Source" value={k || "—"} hintId="config.k" />
        <ConfigTile label="M · Repair" value={m || "—"} hintId="config.m" />
        <ConfigTile
          label="Overhead"
          value={total > 0 ? formatPercent(overhead, 1) : "—"}
          tone="cyan"
          hintId="config.overhead"
        />
        <ConfigTile
          label="Code Rate"
          value={total > 0 ? codeRate.toFixed(3) : "—"}
          hintId="config.codeRate"
        />
      </div>
      <div className="mt-4 pt-3 border-t border-[color:var(--color-border-hair)]">
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mb-2">
          Interfaces
        </div>
        <ul className="text-[11px] space-y-1.5">
          <KV label="LAN" value={echo?.lanIface ?? "—"} mono hintId="config.lanIface" />
          <KV label="FSO" value={echo?.fsoIface ?? "—"} mono hintId="config.fsoIface" />
          <KV label="Symbol size" value={echo ? `${echo.symbolSize} B` : "—"} mono hintId="config.symbolSize" />
          <KV label="Symbol CRC" value={echo?.internalSymbolCrc ? "CRC-32C" : "Disabled"}
              tone={echo?.internalSymbolCrc ? "success" : "neutral"} hintId="config.internalSymbolCrc" />
        </ul>
      </div>
    </GlassPanel>
  );
}

function DeinterleaverPanel({ snap }: { snap: TelemetrySnapshot }) {
  const dil = snap.dilStats;
  if (!dil) {
    return (
      <GlassPanel label="Deinterleaver State">
        <div className="py-4 text-[11px] text-[color:var(--color-text-muted)] leading-snug">
          Live deinterleaver state requires a fso-gw-stats/2 frame from the
          control_server. Not available from this source.
        </div>
      </GlassPanel>
    );
  }
  return (
    <GlassPanel label="Deinterleaver State"
      trailing={
        <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
          live · slot FSM
        </span>
      }
    >
      <div className="grid grid-cols-2 gap-2 mb-3">
        <SlotTile label="Active" value={dil.activeBlocks} tone="cyan" hintId="dil.active" />
        <SlotTile label="Ready" value={dil.readyCount} tone={dil.readyCount > 0 ? "warning" : "success"} hintId="dil.ready" />
      </div>
      <ul className="text-[11px] space-y-1.5 py-1">
        <KV label="Blocks ready (cum)"  value={formatNumber(dil.blocksReady)} tone="success" hintId="dil.blocksReady" />
        <KV label="Failed · timeout"    value={formatNumber(dil.blocksFailedTimeout)}
            tone={dil.blocksFailedTimeout > 0 ? "warning" : "neutral"} hintId="dil.failedTimeout" />
        <KV label="Failed · holes"      value={formatNumber(dil.blocksFailedHoles)}
            tone={dil.blocksFailedHoles > 0 ? "danger" : "neutral"} hintId="dil.failedHoles" />
        <li className="border-t border-[color:var(--color-border-hair)] my-2" />
        <KV label="Dropped · duplicate" value={formatNumber(dil.droppedDuplicate)} hintId="dil.droppedDuplicate" />
        <KV label="Dropped · frozen"    value={formatNumber(dil.droppedFrozen)} hintId="dil.droppedFrozen" />
        <KV label="Dropped · erasure"   value={formatNumber(dil.droppedErasure)} hintId="dil.droppedErasure" />
        <KV label="Dropped · CRC fail"  value={formatNumber(dil.droppedCrcFail)}
            tone={dil.droppedCrcFail > 0 ? "warning" : "neutral"} hintId="dil.droppedCrcFail" />
        <li className="border-t border-[color:var(--color-border-hair)] my-2" />
        <KV label="Evicted · filling"   value={formatNumber(dil.evictedFilling)}
            tone={dil.evictedFilling > 0 ? "warning" : "neutral"} hintId="dil.evictedFilling" />
        <KV label="Evicted · done"      value={formatNumber(dil.evictedDone)}
            tone={dil.evictedDone > 0 ? "warning" : "neutral"} hintId="dil.evictedDone" />
      </ul>
      <div className="mt-3 text-[10px] text-[color:var(--color-text-muted)] leading-snug">
        FSM: <span className="font-mono">EMPTY → FILLING → READY_TO_DECODE → EMPTY</span>.
        <span className="font-mono"> active</span> = slots in FILLING + READY;
        <span className="font-mono"> ready</span> = slots awaiting drain.
        Eviction means a slot was reclaimed before its block was decoded — typically
        because the depth × 4 slot pool was exhausted.
      </div>
    </GlassPanel>
  );
}

function SlotTile({ label, value, tone, hintId }: { label: string; value: number; tone: "cyan" | "success" | "warning"; hintId?: FieldHintId }) {
  const color = tone === "warning" ? "var(--color-warning)"
              : tone === "success" ? "var(--color-success)"
              : "var(--color-cyan-300)";
  return (
    <div className="glass rounded-md px-3 py-2 border-[color:var(--color-border-hair)]">
      <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </div>
      <div className="font-display text-2xl font-bold tabular mt-0.5" style={{ color }}>
        {value}
      </div>
    </div>
  );
}

function BlockEventFeed({ snap }: { snap: TelemetrySnapshot }) {
  const events = snap.blockEvents ?? [];
  const last = events.slice(0, 80);

  const reasonStyle: Record<string, { color: string; label: string }> = {
    SUCCESS:         { color: "var(--color-success)",   label: "OK" },
    DECODE_FAILED:   { color: "var(--color-danger)",    label: "DECODE" },
    TIMEOUT:         { color: "var(--color-warning)",   label: "TIMEOUT" },
    TOO_MANY_HOLES:  { color: "var(--color-danger)",    label: "HOLES" },
    EVICTED_FILLING: { color: "var(--color-warning)",   label: "EVICT-F" },
    EVICTED_READY:   { color: "var(--color-warning)",   label: "EVICT-R" },
    NONE:            { color: "var(--color-text-muted)", label: "NONE" },
    UNKNOWN:         { color: "var(--color-text-muted)", label: "?" },
  };

  return (
    <GlassPanel label="Block Lifecycle Events" hintId="block.eventFeed"
      trailing={
        <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
          drained from deinterleaver callback ring
        </span>
      }
    >
      {last.length === 0 ? (
        <div className="py-8 text-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
          No block events yet
        </div>
      ) : (
        <ul className="font-mono text-[11px] max-h-[320px] overflow-y-auto space-y-0.5">
          {last.map((e, i) => {
            const t = new Date(e.t).toISOString().slice(11, 23);
            const meta = reasonStyle[e.reason] ?? reasonStyle.UNKNOWN;
            return (
              <li
                key={`${e.blockId}-${e.t}-${i}`}
                className="grid grid-cols-[auto_auto_auto_1fr] gap-x-3 px-1 -mx-1 rounded hover:bg-white/[0.03]"
              >
                <span className="tabular text-[color:var(--color-text-muted)]">{t}</span>
                <span className="font-semibold tracking-[0.1em]" style={{ color: meta.color }}>
                  {meta.label.padEnd(7, " ")}
                </span>
                <span className="text-[color:var(--color-text-secondary)] tabular">
                  block_id={e.blockId}
                </span>
                <span className="text-[color:var(--color-text-muted)]">
                  {e.evicted ? "(evicted)" : ""}
                </span>
              </li>
            );
          })}
        </ul>
      )}
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
        <KV label="Blocks with Loss" value={formatNumber(blocksLoss)} hintId="stress.blocksWithLoss" />
        <KV label="Worst Holes / Block" value={formatNumber(worstHoles)}
            tone={worstHoles > 3 ? "warning" : "neutral"} hintId="stress.worstHolesInBlock" />
        <KV label="Total Holes" value={formatNumber(totalHoles)} hintId="stress.totalHolesInBlocks" />
        <li className="border-t border-[color:var(--color-border-hair)] my-2" />
        <KV label="Recoverable Bursts" value={formatNumber(recoverable)} tone="success" hintId="stress.recoverableBursts" />
        <KV label="Critical Bursts" value={formatNumber(critical)}
            tone={critical > 0 ? "danger" : "neutral"} hintId="stress.criticalBursts" />
        <KV label="Exceeding FEC Span" value={formatNumber(exceeding)}
            tone={exceeding > 0 ? "danger" : "neutral"} hintId="stress.exceedingFecSpan" />
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
  hintId,
}: {
  label: string;
  value: string | number;
  tone?: "neutral" | "cyan" | "success";
  hintId?: FieldHintId;
}) {
  const color =
    tone === "cyan"
      ? "var(--color-cyan-300)"
      : tone === "success"
      ? "var(--color-success)"
      : "var(--color-text-primary)";
  return (
    <div className="glass rounded-md px-3 py-2 border-[color:var(--color-border-hair)]">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
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
  hintId,
}: {
  label: string;
  value: string | number;
  mono?: boolean;
  tone?: "neutral" | "cyan" | "success" | "warning" | "danger";
  hintId?: FieldHintId;
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
      <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
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

// Bin extents (inclusive). 501+ is open-ended — capped at 5000 to keep the
// rightmost bar finite on a log axis.
const BURST_BIN_RANGES: Array<[number, number]> = [
  [1, 1],
  [2, 5],
  [6, 10],
  [11, 50],
  [51, 100],
  [101, 500],
  [501, 5000],
];
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
        return `burst ${label}<br/>
          <b>${formatNumber(real)}</b> events<br/>
          ${formatPercent(pct / 100, 2)} of all bursts`;
      },
    },
    grid: { left: 54, right: 16, top: 10, bottom: 48 },
    xAxis: {
      type: "log",
      logBase: 10,
      min: BURST_X_MIN,
      max: BURST_X_MAX,
      name: "burst length (symbols, log)",
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
        type: "custom",
        renderItem: (_params, api) => {
          const v = api.value(1) as number;
          const i = api.value(2) as number;
          const [lo, hi] = BURST_BIN_RANGES[i];
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

