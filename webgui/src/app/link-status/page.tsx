"use client";

import dynamic from "next/dynamic";
import type { EChartsOption } from "echarts";
import { useMemo } from "react";
import {
  Activity,
  AlertTriangle,
  Radio,
  ShieldCheck,
  Signal,
  Waves,
  Zap,
} from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { PulseRing } from "@/components/primitives/PulseRing";
import { useLinkHistory, type FadeEvent, type LinkSample } from "@/lib/useLinkHistory";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatNumber, formatPercent, formatUptime } from "@/lib/utils";
import type { LinkState } from "@/types/telemetry";

const ReactECharts = dynamic(() => import("echarts-for-react"), { ssr: false });

const CYAN  = "#00d4ff";
const BLUE  = "#5aa0ff";
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
  grid: { left: 54, right: 48, top: 28, bottom: 28 },
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

  const rssiSnrChart = useMemo(() => buildRssiSnrOption(history.samples), [history.samples]);
  const berChart = useMemo(() => buildBerOption(history.samples), [history.samples]);
  const latencyChart = useMemo(() => buildLatencyOption(history.samples), [history.samples]);

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
  const hasOptics = l.rssiDbm != null && l.snrDb != null;
  const activeFade = history.fades.find((f) => f.tEnd === null) ?? null;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Link Status
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Deep dive into FSO optical link health. Tracks RSSI, SNR, BER, latency,
            and link stability with fade-event detection.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4E · Link Diagnostics
        </div>
      </div>

      {/* Hero: PulseRing + stat tiles */}
      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="grid grid-cols-[auto_1fr] items-center gap-8 px-8 py-6">
          <PulseRing state={l.state} qualityPct={l.qualityPct} size={220} />
          <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
            <HeroTile
              label="State"
              value={l.state.toUpperCase()}
              tone={toneForState(l.state)}
              icon={<Signal size={13} />}
            />
            <HeroTile
              label="RSSI"
              value={l.rssiDbm != null ? `${l.rssiDbm.toFixed(1)} dBm` : "N/A"}
              tone={l.rssiDbm == null ? "muted" : l.rssiDbm > -25 ? "success" : l.rssiDbm > -30 ? "cyan" : "warning"}
              icon={<Radio size={13} />}
            />
            <HeroTile
              label="SNR"
              value={l.snrDb != null ? `${l.snrDb.toFixed(1)} dB` : "N/A"}
              tone={l.snrDb == null ? "muted" : l.snrDb > 25 ? "success" : l.snrDb > 15 ? "cyan" : "warning"}
              icon={<Waves size={13} />}
            />
            <HeroTile
              label="BER (est.)"
              value={l.berEstimate != null ? l.berEstimate.toExponential(1) : "N/A"}
              tone={l.berEstimate == null ? "muted" : l.berEstimate < 1e-7 ? "success" : l.berEstimate < 1e-5 ? "cyan" : "warning"}
              icon={<Zap size={13} />}
            />
            <HeroTile
              label="Latency (avg)"
              value={`${l.latencyMsAvg.toFixed(2)} ms`}
              tone={l.latencyMsAvg < 2 ? "success" : l.latencyMsAvg < 4 ? "cyan" : "warning"}
              icon={<Activity size={13} />}
            />
            <HeroTile
              label="Uptime"
              value={formatUptime(l.uptimeSec)}
              tone="cyan"
              icon={<ShieldCheck size={13} />}
            />
          </div>
        </div>
      </GlassPanel>

      {/* Charts */}
      <div className="grid grid-cols-1 xl:grid-cols-2 gap-4">
        <GlassPanel
          label="Optical Signal — RSSI & SNR"
          trailing={
            <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              <Legend color={CYAN} label="RSSI (dBm)" />
              <Legend color={BLUE} label="SNR (dB)" />
            </div>
          }
        >
          {!hasOptics ? (
            <NoOpticsBanner />
          ) : history.samples.length < 2 ? (
            <EmptyChartState msg="Collecting samples…" />
          ) : (
            <ReactECharts option={rssiSnrChart} style={{ height: 220 }} notMerge={false} lazyUpdate />
          )}
        </GlassPanel>

        <GlassPanel
          label="BER (log-scale)"
          trailing={
            <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              Derived from lost symbols
            </span>
          }
        >
          {history.samples.length < 2 ? (
            <EmptyChartState msg="Collecting samples…" />
          ) : (
            <ReactECharts option={berChart} style={{ height: 220 }} notMerge={false} lazyUpdate />
          )}
        </GlassPanel>
      </div>

      <GlassPanel
        label="Latency — avg & max"
        trailing={
          <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            <Legend color={GREEN} label="Avg" />
            <Legend color={AMBER} label="Max" />
          </div>
        }
      >
        {history.samples.length < 2 ? (
          <EmptyChartState msg="Collecting samples…" />
        ) : (
          <ReactECharts option={latencyChart} style={{ height: 220 }} notMerge={false} lazyUpdate />
        )}
      </GlassPanel>

      {/* Stability + Fade timeline */}
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
/* Sub-components                                                              */
/* -------------------------------------------------------------------------- */

function HeroTile({
  label,
  value,
  tone = "cyan",
  icon,
}: {
  label: string;
  value: string;
  tone?: "cyan" | "success" | "warning" | "danger" | "muted";
  icon?: React.ReactNode;
}) {
  const toneColor =
    tone === "success" ? "var(--color-success)"
    : tone === "warning" ? "var(--color-warning)"
    : tone === "danger" ? "var(--color-danger)"
    : tone === "muted" ? "var(--color-text-muted)" : "var(--color-cyan-300)";
  return (
    <div className="glass rounded-md px-3 py-2.5 border-[color:var(--color-border-hair)]">
      <div className="flex items-center justify-between mb-1">
        <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          {label}
        </span>
        {icon && <span style={{ color: toneColor, opacity: 0.7 }}>{icon}</span>}
      </div>
      <div
        className="font-display text-base font-semibold tabular truncate"
        style={{ color: toneColor }}
      >
        {value}
      </div>
    </div>
  );
}

function StabilityPanel({
  uptimeRatio,
  observedMs,
  streakMs,
  fadeCount,
  activeFade,
  currentState,
}: {
  uptimeRatio: number;
  observedMs: number;
  streakMs: number;
  fadeCount: number;
  activeFade: FadeEvent | null;
  currentState: LinkState;
}) {
  return (
    <GlassPanel label="Link Stability">
      <div className="grid grid-cols-2 gap-3">
        <KeyValue label="Session Uptime" value={formatPercent(uptimeRatio, 2)}
          tone={uptimeRatio > 0.99 ? "success" : uptimeRatio > 0.95 ? "cyan" : "warning"} />
        <KeyValue label="Observed For" value={msHuman(observedMs)} />
        <KeyValue
          label={currentState === "online" ? "Stable For" : "In Current State"}
          value={msHuman(streakMs)}
          tone={currentState === "online" ? "success" : "warning"}
        />
        <KeyValue label="Fade Events" value={formatNumber(fadeCount)}
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
            Active fade event — dropped to <b>{activeFade.toState.toUpperCase()}</b>,
            lowest quality <b>{activeFade.lowestQualityPct.toFixed(1)}%</b>, running for{" "}
            <b>{msHuman(Date.now() - activeFade.tStart)}</b>.
          </span>
        </div>
      )}
      <div className="mt-4 text-[10px] leading-snug text-[color:var(--color-text-muted)]">
        Stability is measured within the current session (samples collected since page
        load). Cumulative uptime is shown in the top-bar.
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
      <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        Newest first
      </span>
    }>
      <ul className="space-y-1.5">
        {[...fades].reverse().slice(0, 8).map((f) => {
          const tStart = new Date(f.tStart).toLocaleTimeString([], {
            hour: "2-digit", minute: "2-digit", second: "2-digit",
          });
          const ongoing = f.tEnd === null;
          const toneColor =
            f.toState === "offline" ? RED : AMBER;
          return (
            <li key={f.id} className="grid grid-cols-[auto_auto_auto_1fr_auto] items-center gap-3 text-xs py-1">
              <AlertTriangle size={12} style={{ color: toneColor, flexShrink: 0 }} />
              <span className="font-mono tabular text-[color:var(--color-text-muted)]">{tStart}</span>
              <span
                className="font-semibold tracking-[0.15em] uppercase text-[10px]"
                style={{ color: toneColor }}
              >
                {f.fromState} → {f.toState}
              </span>
              <span className="text-[color:var(--color-text-secondary)] truncate">
                lowest quality {f.lowestQualityPct.toFixed(1)}%
              </span>
              <span
                className="font-mono tabular text-[10px]"
                style={{
                  color: ongoing ? toneColor : "var(--color-text-secondary)",
                }}
              >
                {ongoing ? "ongoing" : msHuman(f.durationMs ?? 0)}
              </span>
            </li>
          );
        })}
      </ul>
    </GlassPanel>
  );
}

function KeyValue({
  label,
  value,
  tone = "neutral",
}: {
  label: string;
  value: string;
  tone?: "neutral" | "cyan" | "success" | "warning" | "danger";
}) {
  const color =
    tone === "success" ? "var(--color-success)"
    : tone === "warning" ? "var(--color-warning)"
    : tone === "danger" ? "var(--color-danger)"
    : tone === "cyan" ? "var(--color-cyan-300)" : "var(--color-text-primary)";
  return (
    <div className="glass rounded-md px-3 py-2">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div className="font-display text-base font-semibold tabular mt-0.5" style={{ color }}>
        {value}
      </div>
    </div>
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

function EmptyChartState({ msg }: { msg: string }) {
  return (
    <div className="h-[220px] flex items-center justify-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
      {msg}
    </div>
  );
}

function NoOpticsBanner() {
  return (
    <div className="h-[220px] flex flex-col items-center justify-center gap-2 text-center">
      <Waves size={22} className="text-[color:var(--color-text-muted)]" />
      <div className="text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        Optical sensors not connected
      </div>
      <div className="text-[10px] text-[color:var(--color-text-muted)] max-w-sm">
        RSSI/SNR come from the FSO transceiver hardware. The bridge is connected to
        the gateway daemon but the daemon does not expose optics telemetry yet.
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

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

function buildRssiSnrOption(samples: LinkSample[]): EChartsOption {
  const labels = samples.map((s) => new Date(s.t).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }));
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: [
      {
        type: "value",
        name: "RSSI (dBm)",
        nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
        position: "left",
        ...baseAxis,
      },
      {
        type: "value",
        name: "SNR (dB)",
        nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
        position: "right",
        ...baseAxis,
        splitLine: { show: false },
      },
    ],
    series: [
      {
        name: "RSSI (dBm)",
        type: "line",
        smooth: true,
        showSymbol: false,
        yAxisIndex: 0,
        data: samples.map((s) => s.rssiDbm != null ? +s.rssiDbm.toFixed(2) : null),
        lineStyle: { color: CYAN, width: 1.8 },
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
        name: "SNR (dB)",
        type: "line",
        smooth: true,
        showSymbol: false,
        yAxisIndex: 1,
        data: samples.map((s) => s.snrDb != null ? +s.snrDb.toFixed(2) : null),
        lineStyle: { color: BLUE, width: 1.5 },
      },
    ],
  };
}

function buildBerOption(samples: LinkSample[]): EChartsOption {
  const labels = samples.map((s) => new Date(s.t).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }));
  // Use 1e-15 as floor for log-scale plotting of zeros.
  const data = samples.map((s) => {
    const v = s.berEstimate;
    return v != null && v > 0 ? v : 1e-15;
  });
  return {
    ...baseOpts,
    grid: { left: 60, right: 16, top: 28, bottom: 28 },
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: {
      type: "log",
      name: "BER",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
      min: 1e-12,
      max: 1e-3,
    },
    series: [
      {
        name: "BER",
        type: "line",
        smooth: true,
        showSymbol: false,
        data,
        lineStyle: { color: AMBER, width: 1.6 },
      },
    ],
  };
}

function buildLatencyOption(samples: LinkSample[]): EChartsOption {
  const labels = samples.map((s) => new Date(s.t).toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }));
  return {
    ...baseOpts,
    grid: { left: 54, right: 16, top: 28, bottom: 28 },
    xAxis: {
      type: "category",
      data: labels,
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: {
      type: "value",
      name: "ms",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        name: "Avg",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +s.latencyAvgMs.toFixed(2)),
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
      },
      {
        name: "Max",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +s.latencyMaxMs.toFixed(2)),
        lineStyle: { color: AMBER, width: 1.2, type: "dashed" },
      },
    ],
  };
}
