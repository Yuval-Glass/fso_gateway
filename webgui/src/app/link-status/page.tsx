"use client";

import dynamic from "next/dynamic";
import type { EChartsOption } from "echarts";
import { useMemo } from "react";
import {
  Activity,
  AlertTriangle,
  Shield,
  ShieldCheck,
  Signal,
  TriangleAlert,
} from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { PulseRing } from "@/components/primitives/PulseRing";
import { useLinkHistory, type FadeEvent } from "@/lib/useLinkHistory";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatNumber, formatPercent, formatUptime } from "@/lib/utils";
import type { LinkState } from "@/types/telemetry";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

const CYAN  = "#00d4ff";
const GREEN = "#34d399";
const AMBER = "#ffb020";
const RED   = "#ff2d5c";
const AXIS_COLOR = "rgba(255,255,255,0.06)";
const TEXT_MUTED = "#566377";

const baseAxis = {
  axisLine: { lineStyle: { color: AXIS_COLOR } },
  axisTick: { lineStyle: { color: AXIS_COLOR } },
  splitLine: { lineStyle: { color: AXIS_COLOR } },
  axisLabel: { color: TEXT_MUTED, fontSize: 10, fontFamily: "var(--font-mono)" },
};

const baseOpts: Partial<EChartsOption> = {
  grid: { left: 54, right: 20, top: 28, bottom: 28 },
  textStyle: { fontFamily: "var(--font-sans)" },
  tooltip: {
    trigger: "axis",
    backgroundColor: "rgba(13, 19, 32, 0.95)",
    borderColor: "rgba(0, 212, 255, 0.3)",
    textStyle: { color: "#e8f1ff", fontSize: 11 },
    axisPointer: { lineStyle: { color: "rgba(0, 212, 255, 0.4)" } },
  },
};

export default function LinkStatusPage() {
  const { snapshot: snap } = useTelemetry();
  const history = useLinkHistory(snap);

  const qualityChart = useMemo(() => buildQualityOption(history.samples), [history.samples]);
  const lossChart    = useMemo(() => buildLossOption(history.samples),    [history.samples]);

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading link telemetry…
        </div>
      </div>
    );
  }

  const l = snap.link;
  const e = snap.errors;
  const activeFade = history.fades.find((f) => f.tEnd === null) ?? null;
  const lossRatio = e.symbolLossRatio;
  const lossStr =
    lossRatio == null ? "—"
    : lossRatio === 0 ? "0"
    : lossRatio < 1e-4 ? lossRatio.toExponential(1)
    : formatPercent(lossRatio, 3);

  return (
    <div className="flex flex-col gap-5">
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Link Status
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            FSO link health. Quality is derived from FEC block success rate;
            symbol loss is from the deinterleaver&rsquo;s hole counter. There is no
            optical RSSI/SNR sensor in this project — the &ldquo;FSO link&rdquo;
            is simulated by an Ethernet cable between the two gateway NICs.
          </div>
        </div>
      </div>

      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="grid grid-cols-[auto_1fr] items-center gap-8 px-8 py-6">
          <PulseRing state={l.state} qualityPct={l.qualityPct} size={220} />
          <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
            <HeroTile label="State" value={l.state.toUpperCase()} tone={toneForState(l.state)}
                      icon={<Signal size={13} />} />
            <HeroTile label="Quality" value={formatPercent(l.qualityPct / 100, 3)}
                      tone={l.qualityPct > 99 ? "success" : l.qualityPct > 95 ? "cyan" : "warning"}
                      icon={<ShieldCheck size={13} />} />
            <HeroTile label="Symbol Loss" value={lossStr}
                      tone={lossRatio == null ? "muted"
                        : lossRatio > 1e-3 ? "danger"
                        : lossRatio > 1e-4 ? "warning" : "success"}
                      icon={<Shield size={13} />} />
            <HeroTile label="Block Fail Rate" value={formatPercent(e.blockFailRatio, 4)}
                      tone={e.blockFailRatio > 0.01 ? "danger" : e.blockFailRatio > 0.001 ? "warning" : "success"}
                      icon={<TriangleAlert size={13} />} />
            <HeroTile label="Blocks Attempted" value={formatNumber(e.blocksAttempted)}
                      icon={<Activity size={13} />} />
            <HeroTile label="Session Uptime" value={formatUptime(l.uptimeSec)} tone="cyan"
                      icon={<ShieldCheck size={13} />} />
          </div>
        </div>
      </GlassPanel>

      <div className="grid grid-cols-1 xl:grid-cols-2 gap-4">
        <GlassPanel label="Quality Over Time"
          trailing={<span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">derived from blocks_recovered / attempted</span>}>
          {history.samples.length < 2
            ? <EmptyChart />
            : <ReactECharts option={qualityChart} style={{ height: 220 }} notMerge={false} lazyUpdate />}
        </GlassPanel>

        <GlassPanel label="Symbol Loss Ratio"
          trailing={<span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">lost_symbols / total_symbols</span>}>
          {history.samples.length < 2
            ? <EmptyChart />
            : <ReactECharts option={lossChart} style={{ height: 220 }} notMerge={false} lazyUpdate />}
        </GlassPanel>
      </div>

      <div className="grid grid-cols-1 xl:grid-cols-[1fr_2fr] gap-4">
        <StabilityPanel
          uptimeRatio={history.uptimeRatio}
          observedMs={history.observedMs}
          streakMs={history.streakMs}
          fadeCount={history.fades.length}
          activeFade={activeFade}
          currentState={l.state}
        />
        <FadeTimelinePanel fades={history.fades} />
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */

function HeroTile({
  label, value, tone = "cyan", icon,
}: {
  label: string; value: string;
  tone?: "cyan" | "success" | "warning" | "danger" | "muted";
  icon?: React.ReactNode;
}) {
  const toneColor =
    tone === "success" ? "var(--color-success)"
    : tone === "warning" ? "var(--color-warning)"
    : tone === "danger" ? "var(--color-danger)"
    : tone === "muted" ? "var(--color-text-muted)"
    : "var(--color-cyan-300)";
  return (
    <div className="glass rounded-md px-3 py-2.5 border-[color:var(--color-border-hair)]">
      <div className="flex items-center justify-between mb-1">
        <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">{label}</span>
        {icon && <span style={{ color: toneColor, opacity: 0.7 }}>{icon}</span>}
      </div>
      <div className="font-display text-base font-semibold tabular truncate" style={{ color: toneColor }}>
        {value}
      </div>
    </div>
  );
}

function StabilityPanel({
  uptimeRatio, observedMs, streakMs, fadeCount, activeFade, currentState,
}: {
  uptimeRatio: number; observedMs: number; streakMs: number; fadeCount: number;
  activeFade: FadeEvent | null; currentState: LinkState;
}) {
  return (
    <GlassPanel label="Link Stability (this session)">
      <div className="grid grid-cols-2 gap-3">
        <KV label="Session Uptime" value={formatPercent(uptimeRatio, 2)}
            tone={uptimeRatio > 0.99 ? "success" : uptimeRatio > 0.95 ? "cyan" : "warning"} />
        <KV label="Observed For" value={msHuman(observedMs)} />
        <KV label={currentState === "online" ? "Stable For" : "In Current State"}
            value={msHuman(streakMs)}
            tone={currentState === "online" ? "success" : "warning"} />
        <KV label="Fade Events" value={formatNumber(fadeCount)}
            tone={fadeCount === 0 ? "success" : fadeCount < 3 ? "cyan" : "warning"} />
      </div>
      {activeFade && (
        <div
          className="mt-4 px-3 py-2 rounded-md border flex items-start gap-2 text-[11px]"
          style={{
            borderColor: "rgba(255, 176, 32, 0.5)",
            background: "rgba(255, 176, 32, 0.08)",
            color: "var(--color-warning)",
          }}
        >
          <AlertTriangle size={13} className="mt-0.5 shrink-0" />
          <span>
            Active fade — dropped to <b>{activeFade.toState.toUpperCase()}</b>,
            lowest quality <b>{activeFade.lowestQualityPct.toFixed(1)}%</b>, running for{" "}
            <b>{msHuman(Date.now() - activeFade.tStart)}</b>.
          </span>
        </div>
      )}
      <div className="mt-4 text-[10px] leading-snug text-[color:var(--color-text-muted)]">
        Samples are collected since the page loaded. Cumulative session uptime
        is shown in the top-bar.
      </div>
    </GlassPanel>
  );
}

function FadeTimelinePanel({ fades }: { fades: FadeEvent[] }) {
  if (fades.length === 0) {
    return (
      <GlassPanel label="Fade Event Log">
        <div className="flex flex-col items-center justify-center py-10 text-center gap-2">
          <div
            className="w-10 h-10 rounded-full glass glass-cyan flex items-center justify-center"
            style={{ boxShadow: "0 0 20px rgba(0, 212, 255, 0.2)" }}
          >
            <ShieldCheck size={18} className="text-[color:var(--color-cyan-300)]" />
          </div>
          <div className="text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
            No fade events observed this session
          </div>
        </div>
      </GlassPanel>
    );
  }
  return (
    <GlassPanel label="Fade Event Log" trailing={
      <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">Newest first</span>
    }>
      <ul className="space-y-1.5">
        {[...fades].reverse().slice(0, 8).map((f) => {
          const tStart = new Date(f.tStart).toLocaleTimeString([], {
            hour: "2-digit", minute: "2-digit", second: "2-digit",
          });
          const ongoing = f.tEnd === null;
          const toneColor = f.toState === "offline" ? RED : AMBER;
          return (
            <li key={f.id} className="grid grid-cols-[auto_auto_auto_1fr_auto] items-center gap-3 text-xs py-1">
              <AlertTriangle size={12} style={{ color: toneColor, flexShrink: 0 }} />
              <span className="font-mono tabular text-[color:var(--color-text-muted)]">{tStart}</span>
              <span className="font-semibold tracking-[0.15em] uppercase text-[10px]" style={{ color: toneColor }}>
                {f.fromState} → {f.toState}
              </span>
              <span className="text-[color:var(--color-text-secondary)] truncate">
                lowest quality {f.lowestQualityPct.toFixed(1)}%
              </span>
              <span className="font-mono tabular text-[10px]"
                    style={{ color: ongoing ? toneColor : "var(--color-text-secondary)" }}>
                {ongoing ? "ongoing" : msHuman(f.durationMs ?? 0)}
              </span>
            </li>
          );
        })}
      </ul>
    </GlassPanel>
  );
}

function KV({
  label, value, tone = "neutral",
}: {
  label: string; value: string;
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
    <div className="glass rounded-md px-3 py-2">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">{label}</div>
      <div className="font-display text-base font-semibold tabular mt-0.5" style={{ color: colorMap[tone] }}>
        {value}
      </div>
    </div>
  );
}

function EmptyChart() {
  return (
    <div className="h-[220px] flex items-center justify-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
      Collecting samples…
    </div>
  );
}

function toneForState(state: LinkState): "success" | "warning" | "danger" {
  return state === "online" ? "success" : state === "degraded" ? "warning" : "danger";
}

function msHuman(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return "—";
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  const rs = s % 60;
  if (m < 60) return `${m}m ${rs.toString().padStart(2, "0")}s`;
  const h = Math.floor(m / 60);
  const rm = m % 60;
  return `${h}h ${rm.toString().padStart(2, "0")}m`;
}

function buildQualityOption(samples: Array<{ t: number; qualityPct: number }>): EChartsOption {
  const labels = samples.map((s) => new Date(s.t).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }));
  return {
    ...baseOpts,
    xAxis: {
      type: "category", data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: {
      type: "value", name: "%",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      min: 80, max: 100,
      ...baseAxis,
    },
    series: [{
      name: "Quality",
      type: "line", smooth: true, showSymbol: false,
      data: samples.map((s) => +s.qualityPct.toFixed(2)),
      lineStyle: { color: GREEN, width: 1.8 },
      areaStyle: {
        color: {
          type: "linear", x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [
            { offset: 0, color: "rgba(52, 211, 153, 0.3)" },
            { offset: 1, color: "rgba(52, 211, 153, 0)" },
          ],
        },
      },
    }],
  };
}

function buildLossOption(samples: Array<{ t: number; symbolLossRatio: number | null }>): EChartsOption {
  const labels = samples.map((s) => new Date(s.t).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }));
  const data = samples.map((s) => s.symbolLossRatio ?? 0);
  return {
    ...baseOpts,
    xAxis: {
      type: "category", data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: {
      ...baseAxis,
      type: "value", name: "ratio",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      axisLabel: { ...baseAxis.axisLabel, formatter: (v: number) => v === 0 ? "0" : v.toExponential(0) },
    },
    series: [{
      name: "Symbol loss",
      type: "line", smooth: true, showSymbol: false,
      data,
      lineStyle: { color: AMBER, width: 1.6 },
      areaStyle: {
        color: {
          type: "linear", x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [
            { offset: 0, color: "rgba(255, 176, 32, 0.3)" },
            { offset: 1, color: "rgba(255, 176, 32, 0)" },
          ],
        },
      },
    }],
  };
}
