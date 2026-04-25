"use client";

import dynamic from "next/dynamic";
import type { EChartsOption } from "echarts";
import { useMemo, useState } from "react";
import {
  Calendar,
  Check,
  Clock,
  Database,
  Download,
  FileDown,
  FileText,
  Inbox,
  Play,
  Square,
  Trash2,
  Zap,
} from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { TactileButton } from "@/components/primitives/TactileButton";
import { FieldHint } from "@/components/primitives/FieldHint";
import { useRunDetail, useRuns } from "@/lib/useRuns";
import { useExperimentDetail, useExperiments, type ExperimentSummary } from "@/lib/useExperiments";
import {
  cn,
  formatBitrate,
  formatNumber,
  formatPercent,
  formatUptime,
} from "@/lib/utils";
import type { RunSample, RunSummary } from "@/types/runs";

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
  grid: { left: 58, right: 16, top: 28, bottom: 28 },
  textStyle: { fontFamily: "var(--font-sans)" },
  tooltip: {
    trigger: "axis",
    backgroundColor: "rgba(13, 19, 32, 0.95)",
    borderColor: "rgba(0, 212, 255, 0.3)",
    textStyle: { color: "#e8f1ff", fontSize: 11 },
    axisPointer: { lineStyle: { color: "rgba(0, 212, 255, 0.4)" } },
  },
};

type AnalyticsTab = "runs" | "experiments";

export default function AnalyticsPage() {
  const [tab, setTab] = useState<AnalyticsTab>("runs");
  const runs = useRuns();
  const [selectedId, setSelectedId] = useState<number | null>(null);

  // Default selection: active run, else newest. Re-pick if selection invalid.
  const effectiveSelected = useMemo(() => {
    if (selectedId !== null && runs.runs.some((r) => r.id === selectedId)) {
      return selectedId;
    }
    return runs.activeRunId ?? (runs.runs[0]?.id ?? null);
  }, [selectedId, runs.activeRunId, runs.runs]);

  const detail = useRunDetail(effectiveSelected);

  const throughputChart = useMemo(
    () => buildThroughputChart(detail.samples),
    [detail.samples],
  );
  const qualityChart = useMemo(
    () => buildQualityChart(detail.samples),
    [detail.samples],
  );
  const fecChart = useMemo(
    () => buildFecRateChart(detail.samples),
    [detail.samples],
  );

  const run = detail.detail?.run;

  if (tab === "experiments") {
    return (
      <div className="flex flex-col gap-5">
        <AnalyticsHeader tab={tab} onTab={setTab} />
        <ExperimentsView />
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-5">
      <AnalyticsHeader tab={tab} onTab={setTab} />

      <div className="grid grid-cols-1 xl:grid-cols-[320px_1fr] gap-4">
        {/* LEFT: run list */}
        <RunList
          runs={runs.runs}
          selectedId={effectiveSelected}
          activeId={runs.activeRunId}
          onSelect={setSelectedId}
          onStartNew={async () => {
            const r = await runs.startNewRun();
            if (r) setSelectedId(r.id);
          }}
          onEnd={async (id) => {
            await runs.endRun(id);
          }}
          onDelete={async (id) => {
            const ok = window.confirm(`Delete run #${id}? This cannot be undone.`);
            if (!ok) return;
            const deleted = await runs.deleteRun(id);
            if (deleted && selectedId === id) setSelectedId(null);
          }}
          error={runs.error}
        />

        {/* RIGHT: selected run detail */}
        <div className="flex flex-col gap-4 min-w-0">
          {!run ? (
            <GlassPanel>
              <div className="py-10 text-center text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
                {runs.loading ? "Loading runs…" : "No run selected"}
              </div>
            </GlassPanel>
          ) : (
            <>
              <RunSummaryCard run={run} samples={detail.samples} />
              <div className="grid grid-cols-1 gap-4">
                <GlassPanel
                  label="Throughput Over Run"
                  hintId="traffic.txBps"
                  trailing={
                    <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
                      <Legend color={CYAN} label="TX" />
                      <Legend color={BLUE} label="RX" />
                    </div>
                  }
                >
                  {detail.samples.length < 2 ? (
                    <EmptyChart />
                  ) : (
                    <ReactECharts option={throughputChart} style={{ height: 220 }} notMerge={false} lazyUpdate />
                  )}
                </GlassPanel>
                <div className="grid grid-cols-1 xl:grid-cols-2 gap-4">
                  <GlassPanel label="Link Quality" hintId="link.qualityPct">
                    {detail.samples.length < 2 ? <EmptyChart /> : (
                      <ReactECharts option={qualityChart} style={{ height: 200 }} notMerge={false} lazyUpdate />
                    )}
                  </GlassPanel>
                  <GlassPanel
                    label="FEC Recovery Rate"
                    hintId="errors.fecSuccessRate"
                    trailing={
                      <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
                        Per-sample from counter deltas
                      </span>
                    }
                  >
                    {detail.samples.length < 2 ? <EmptyChart /> : (
                      <ReactECharts option={fecChart} style={{ height: 200 }} notMerge={false} lazyUpdate />
                    )}
                  </GlassPanel>
                </div>
              </div>
            </>
          )}
        </div>
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Sub-components                                                              */
/* -------------------------------------------------------------------------- */

function AnalyticsHeader({
  tab,
  onTab,
}: {
  tab: AnalyticsTab;
  onTab: (t: AnalyticsTab) => void;
}) {
  return (
    <div className="flex items-end justify-between gap-4 flex-wrap">
      <div>
        <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
          Mission Control
        </div>
        <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
          Analytics
        </h2>
        <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
          {tab === "runs"
            ? "Persisted bridge sessions — throughput, link quality, and FEC outcomes from the running daemon."
            : "Experiment artefacts dropped by scripts/two_machine_run_test.sh into build/stats/."}
        </div>
      </div>
      <div className="flex items-center rounded-md border border-[color:var(--color-border-subtle)] overflow-hidden">
        {(["runs", "experiments"] as AnalyticsTab[]).map((t) => (
          <button
            key={t}
            onClick={() => onTab(t)}
            className={cn(
              "px-3 py-1.5 text-[10px] font-semibold tracking-[0.2em] uppercase transition-colors",
              tab === t
                ? "bg-[color:var(--color-cyan-900)]/60 text-[color:var(--color-cyan-300)]"
                : "text-[color:var(--color-text-secondary)] hover:bg-white/[0.03]",
            )}
          >
            {t === "runs" ? "Bridge Runs" : "Experiments"}
          </button>
        ))}
      </div>
    </div>
  );
}

function ExperimentsView() {
  const list = useExperiments();
  const [selected, setSelected] = useState<string | null>(null);
  const detail = useExperimentDetail(selected);

  // Auto-select most recent on first load.
  if (selected === null && list.list.length > 0) {
    setTimeout(() => setSelected(list.list[0].name), 0);
  }

  return (
    <div className="grid grid-cols-1 xl:grid-cols-[320px_1fr] gap-4">
      <GlassPanel
        label="Experiment Files"
        trailing={
          <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            {list.list.length} {list.list.length === 1 ? "file" : "files"}
          </span>
        }
      >
        {list.error && (
          <div className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-danger)] py-1 mb-2">
            {list.error}
          </div>
        )}
        {list.list.length === 0 ? (
          <div className="flex flex-col items-center gap-2 py-8 text-center">
            <Inbox size={20} className="text-[color:var(--color-text-muted)]" />
            <div className="text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
              No experiment files yet
            </div>
            <div className="text-[10px] text-[color:var(--color-text-muted)] max-w-xs">
              Generated by{" "}
              <span className="font-mono">scripts/two_machine_run_test.sh</span>{" "}
              into <span className="font-mono">build/stats/</span>. Override the
              path with the <span className="font-mono">FSO_EXPERIMENTS_DIR</span>{" "}
              env var on the bridge.
            </div>
          </div>
        ) : (
          <ul className="space-y-1.5 max-h-[520px] overflow-y-auto">
            {list.list.map((it) => (
              <ExperimentListItem
                key={it.name}
                item={it}
                selected={it.name === selected}
                onSelect={() => setSelected(it.name)}
              />
            ))}
          </ul>
        )}
      </GlassPanel>

      <div className="flex flex-col gap-4 min-w-0">
        {!detail.detail && !detail.loading ? (
          <GlassPanel>
            <div className="py-10 text-center text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
              {list.list.length === 0 ? "Nothing to display" : "Pick an experiment to inspect"}
            </div>
          </GlassPanel>
        ) : detail.loading ? (
          <GlassPanel>
            <div className="py-10 text-center text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-cyan-300)] breathe">
              Loading…
            </div>
          </GlassPanel>
        ) : detail.detail ? (
          <ExperimentDetailView detail={detail.detail} />
        ) : null}
      </div>
    </div>
  );
}

function ExperimentListItem({
  item,
  selected,
  onSelect,
}: {
  item: ExperimentSummary;
  selected: boolean;
  onSelect: () => void;
}) {
  const ts = item.ts;
  const friendly =
    ts.length === 15
      ? `${ts.slice(0, 4)}-${ts.slice(4, 6)}-${ts.slice(6, 8)} ${ts.slice(9, 11)}:${ts.slice(11, 13)}:${ts.slice(13, 15)}`
      : ts;
  return (
    <li>
      <button
        onClick={onSelect}
        className={cn(
          "w-full text-left px-2.5 py-2 rounded border transition-all",
          selected
            ? "border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40"
            : "border-[color:var(--color-border-subtle)] bg-white/[0.02] hover:bg-white/[0.04]",
        )}
      >
        <div className="text-[11px] font-semibold text-[color:var(--color-text-primary)] truncate">
          {item.name}
        </div>
        <div className="flex items-center gap-3 mt-1 text-[10px] font-mono tabular text-[color:var(--color-text-muted)]">
          <span className="flex items-center gap-1">
            <Calendar size={9} /> {friendly}
          </span>
          <span>{(item.size / 1024).toFixed(1)} KB</span>
        </div>
      </button>
    </li>
  );
}

function ExperimentDetailView({
  detail,
}: {
  detail: NonNullable<ReturnType<typeof useExperimentDetail>["detail"]>;
}) {
  const s = detail.summary;
  const params = s.params || {};
  return (
    <>
      <GlassPanel variant="raised" padded={false}>
        <div className="flex items-start justify-between px-5 py-4 border-b border-[color:var(--color-border-hair)]">
          <div>
            <div className="flex items-center gap-2">
              <FileText size={14} className="text-[color:var(--color-cyan-300)]" />
              <span className="font-mono text-sm font-semibold text-[color:var(--color-text-primary)]">
                {detail.name}
              </span>
              {s.mode && (
                <span
                  className="text-[9px] font-bold tracking-[0.22em] uppercase px-2 py-0.5 rounded-sm"
                  style={{
                    color: s.mode.startsWith("BASELINE") ? "var(--color-warning)" : "var(--color-cyan-300)",
                    background: s.mode.startsWith("BASELINE")
                      ? "rgba(255, 176, 32, 0.12)"
                      : "rgba(0, 212, 255, 0.12)",
                    border: s.mode.startsWith("BASELINE")
                      ? "1px solid rgba(255, 176, 32, 0.4)"
                      : "1px solid rgba(0, 212, 255, 0.4)",
                  }}
                >
                  {s.mode}
                </span>
              )}
            </div>
            <div className="text-[11px] font-mono tabular text-[color:var(--color-text-muted)] mt-1">
              {(detail.size / 1024).toFixed(1)} KB · {new Date(detail.mtime).toLocaleString()}
            </div>
          </div>
        </div>
        <div className="grid grid-cols-2 md:grid-cols-4 gap-3 p-4">
          <ResultTile
            label="Throughput"
            value={s.throughput ? `${s.throughput.value.toFixed(2)} ${s.throughput.unit}` : "—"}
            tone="cyan"
            icon={<Zap size={14} />}
          />
          <ResultTile
            label="Total Data"
            value={s.totalData ? `${s.totalData.value.toFixed(2)} ${s.totalData.unit}` : "—"}
            icon={<Download size={14} />}
          />
          <ResultTile
            label="Loss %"
            value={s.lossPct != null ? `${s.lossPct.toFixed(2)}%` : "—"}
            tone={s.lossPct != null && s.lossPct > 1 ? "warning" : "success"}
            icon={<Inbox size={14} />}
          />
          <ResultTile
            label="Protocol"
            value={s.protocol || "—"}
            icon={<FileText size={14} />}
          />
        </div>
      </GlassPanel>

      <GlassPanel label="Parameters">
        <ul className="grid grid-cols-2 md:grid-cols-3 gap-x-4 gap-y-1.5 py-1">
          {Object.entries(params).map(([k, v]) => (
            <li key={k} className="flex items-baseline justify-between gap-2 text-[11px]">
              <span className="font-mono text-[color:var(--color-text-muted)] tracking-[0.05em]">{k}</span>
              <span className="font-mono tabular text-[color:var(--color-text-primary)]">{v}</span>
            </li>
          ))}
          {Object.keys(params).length === 0 && (
            <li className="text-[10px] text-[color:var(--color-text-muted)]">
              No parameters parsed.
            </li>
          )}
        </ul>
      </GlassPanel>

      <GlassPanel label="Raw Report" trailing={
        <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
          full text from build/stats/
        </span>
      }>
        <pre className="font-mono text-[10.5px] leading-relaxed whitespace-pre-wrap max-h-[480px] overflow-auto bg-black/25 p-3 rounded text-[color:var(--color-text-primary)]">
{detail.text}
        </pre>
      </GlassPanel>
    </>
  );
}

function ResultTile({
  label,
  value,
  tone = "neutral",
  icon,
}: {
  label: string;
  value: string;
  tone?: "neutral" | "cyan" | "success" | "warning";
  icon?: React.ReactNode;
}) {
  const color =
    tone === "cyan" ? "var(--color-cyan-300)"
    : tone === "success" ? "var(--color-success)"
    : tone === "warning" ? "var(--color-warning)"
    : "var(--color-text-primary)";
  return (
    <div className="glass rounded-md px-3 py-2.5 border-[color:var(--color-border-hair)]">
      <div className="flex items-center justify-between mb-1">
        <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          {label}
        </span>
        {icon && <span style={{ color, opacity: 0.7 }}>{icon}</span>}
      </div>
      <div className="font-display text-base font-semibold tabular truncate" style={{ color }}>
        {value}
      </div>
    </div>
  );
}

function RunList({
  runs,
  selectedId,
  activeId,
  onSelect,
  onStartNew,
  onEnd,
  onDelete,
  error,
}: {
  runs: RunSummary[];
  selectedId: number | null;
  activeId: number | null;
  onSelect: (id: number) => void;
  onStartNew: () => Promise<void>;
  onEnd: (id: number) => Promise<void>;
  onDelete: (id: number) => Promise<void>;
  error: string | null;
}) {
  return (
    <GlassPanel
      label="Runs"
      trailing={
        <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
          {runs.length} total
        </span>
      }
    >
      <div className="flex gap-2 mb-3">
        <TactileButton
          variant="primary"
          icon={<Play size={12} />}
          onClick={onStartNew}
          className="flex-1"
        >
          Start New Run
        </TactileButton>
      </div>
      {error && (
        <div className="mb-2 px-2 py-1 rounded text-[10px] tracking-[0.15em] uppercase text-[color:var(--color-danger)] border border-[color:var(--color-border-danger)] bg-[color:var(--color-danger)]/10">
          {error}
        </div>
      )}
      {runs.length === 0 ? (
        <div className="py-8 text-center text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          No runs recorded yet
        </div>
      ) : (
        <ul className="space-y-1.5 max-h-[520px] overflow-y-auto">
          {runs.map((r) => (
            <RunListItem
              key={r.id}
              run={r}
              selected={r.id === selectedId}
              isActive={r.id === activeId}
              onSelect={() => onSelect(r.id)}
              onEnd={() => onEnd(r.id)}
              onDelete={() => onDelete(r.id)}
            />
          ))}
        </ul>
      )}
    </GlassPanel>
  );
}

function RunListItem({
  run,
  selected,
  isActive,
  onSelect,
  onEnd,
  onDelete,
}: {
  run: RunSummary;
  selected: boolean;
  isActive: boolean;
  onSelect: () => void;
  onEnd: () => void;
  onDelete: () => void;
}) {
  const started = new Date(run.startedAt);
  const duration = (run.endedAt ?? Date.now()) - run.startedAt;
  const timeStr = started.toLocaleTimeString([], {
    hour: "2-digit", minute: "2-digit",
  });
  const dateStr = started.toLocaleDateString([], { month: "short", day: "numeric" });

  return (
    <li>
      <button
        onClick={onSelect}
        className={cn(
          "w-full text-left px-2.5 py-2 rounded border transition-all",
          selected
            ? "border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40"
            : "border-[color:var(--color-border-subtle)] bg-white/[0.02] hover:bg-white/[0.04]",
        )}
      >
        <div className="flex items-center justify-between gap-2">
          <span className="text-[11px] font-semibold text-[color:var(--color-text-primary)] truncate">
            {run.name}
          </span>
          {isActive && (
            <span
              className="text-[8px] font-bold tracking-[0.22em] uppercase px-1.5 py-0.5 rounded-sm"
              style={{
                color: "var(--color-success)",
                background: "rgba(52, 211, 153, 0.12)",
                border: "1px solid rgba(52, 211, 153, 0.4)",
              }}
            >
              live
            </span>
          )}
        </div>
        <div className="flex items-center gap-3 mt-1 text-[10px] font-mono tabular text-[color:var(--color-text-muted)]">
          <span className="flex items-center gap-1">
            <Calendar size={9} /> {dateStr} {timeStr}
          </span>
          <span className="flex items-center gap-1">
            <Clock size={9} /> {formatUptime(duration / 1000)}
          </span>
        </div>
        <div className="mt-1 flex items-center justify-between">
          <span className="text-[10px] tracking-[0.15em] uppercase text-[color:var(--color-text-muted)]">
            {formatNumber(run.sampleCount)} samples
          </span>
          {selected && (
            <span className="flex items-center gap-1">
              {isActive && (
                <span
                  onClick={(e) => { e.stopPropagation(); onEnd(); }}
                  role="button"
                  tabIndex={0}
                  onKeyDown={(e) => { if (e.key === "Enter") onEnd(); }}
                  className="p-1 rounded text-[color:var(--color-warning)] hover:bg-[color:var(--color-warning)]/10"
                  title="End this run"
                >
                  <Square size={10} />
                </span>
              )}
              {!isActive && (
                <span
                  onClick={(e) => { e.stopPropagation(); onDelete(); }}
                  role="button"
                  tabIndex={0}
                  onKeyDown={(e) => { if (e.key === "Enter") onDelete(); }}
                  className="p-1 rounded text-[color:var(--color-danger)] hover:bg-[color:var(--color-danger)]/10"
                  title="Delete this run"
                >
                  <Trash2 size={10} />
                </span>
              )}
            </span>
          )}
        </div>
      </button>
    </li>
  );
}

function RunSummaryCard({ run, samples }: { run: RunSummary; samples: RunSample[] }) {
  // Derive everything from samples (already downsampled but representative).
  const txPeakBps = Math.max(0, ...samples.map((s) => s.txBps ?? 0));
  const rxPeakBps = Math.max(0, ...samples.map((s) => s.rxBps ?? 0));
  const txAvgBps = avg(samples.map((s) => s.txBps ?? 0));
  const rxAvgBps = avg(samples.map((s) => s.rxBps ?? 0));
  const qualityAvg = avg(samples.map((s) => s.linkQualityPct ?? 100));
  const qualityMin = samples.length > 0
    ? Math.min(...samples.map((s) => s.linkQualityPct ?? 100))
    : 100;

  const last = samples[samples.length - 1];
  const blocksAttempted = last?.blocksAttempted ?? 0;
  const blocksRecovered = last?.blocksRecovered ?? 0;
  const blocksFailed = last?.blocksFailed ?? 0;
  const recoveryPct = blocksAttempted > 0 ? (blocksRecovered / blocksAttempted) * 100 : 100;

  const duration = (run.endedAt ?? Date.now()) - run.startedAt;
  const exportUrl = `${bridgeBase()}/api/runs/${run.id}/export.csv`;

  return (
    <GlassPanel variant="raised" padded={false}>
      <div className="flex items-start justify-between px-5 py-4 border-b border-[color:var(--color-border-hair)]">
        <div>
          <div className="flex items-center gap-2">
            <span className="font-display text-base font-semibold text-[color:var(--color-text-primary)]">
              {run.name}
            </span>
            {run.active ? (
              <span
                className="text-[9px] font-bold tracking-[0.22em] uppercase px-2 py-0.5 rounded-sm"
                style={{
                  color: "var(--color-success)",
                  background: "rgba(52, 211, 153, 0.12)",
                  border: "1px solid rgba(52, 211, 153, 0.4)",
                }}
              >
                Recording
              </span>
            ) : (
              <span className="text-[9px] font-bold tracking-[0.22em] uppercase px-2 py-0.5 rounded-sm text-[color:var(--color-text-muted)] border border-[color:var(--color-border-subtle)]">
                Ended
              </span>
            )}
          </div>
          <div className="text-[11px] font-mono tabular text-[color:var(--color-text-muted)] mt-1">
            {new Date(run.startedAt).toLocaleString()} · {formatUptime(duration / 1000)}
          </div>
        </div>
        <div className="flex items-center gap-2">
          <a
            href={exportUrl}
            download={`run-${run.id}.csv`}
            className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-md border text-[10px] font-semibold tracking-[0.18em] uppercase bg-white/[0.03] border-[color:var(--color-border-subtle)] text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-text-primary)] hover:bg-white/[0.06]"
          >
            <FileDown size={12} /> CSV
          </a>
          <FieldHint id="run.exportCsv" size={11} />
        </div>
      </div>
      <div className="grid grid-cols-2 md:grid-cols-5 gap-3 p-4">
        <MetricCard
          label="Samples"
          value={formatNumber(run.sampleCount)}
          icon={<Database size={14} />}
          hintId="run.sampleCount"
          sub={<span className="text-[color:var(--color-text-secondary)]">{samples.length} plotted</span>}
        />
        <MetricCard
          label="TX Peak"
          value={formatBitrate(txPeakBps).value}
          unit={formatBitrate(txPeakBps).unit}
          tone="cyan"
          icon={<Zap size={14} />}
          hintId="run.peakThroughput"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              Avg {formatBitrate(txAvgBps).value} {formatBitrate(txAvgBps).unit}
            </span>
          }
        />
        <MetricCard
          label="RX Peak"
          value={formatBitrate(rxPeakBps).value}
          unit={formatBitrate(rxPeakBps).unit}
          tone="cyan"
          icon={<Download size={14} />}
          hintId="run.peakThroughput"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              Avg {formatBitrate(rxAvgBps).value} {formatBitrate(rxAvgBps).unit}
            </span>
          }
        />
        <MetricCard
          label="Quality (avg)"
          value={formatPercent(qualityAvg / 100, 2)}
          tone={qualityAvg > 99 ? "success" : qualityAvg > 95 ? "cyan" : "warning"}
          icon={<Check size={14} />}
          hintId="run.avgQuality"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              Min {formatPercent(qualityMin / 100, 2)}
            </span>
          }
        />
        <MetricCard
          label="FEC Recovery"
          value={formatPercent(recoveryPct / 100, 3)}
          tone={recoveryPct > 99.9 ? "success" : recoveryPct > 99 ? "cyan" : "warning"}
          icon={<Check size={14} />}
          hintId="errors.fecSuccessRate"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatNumber(blocksFailed)} failed
            </span>
          }
        />
      </div>
    </GlassPanel>
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

function EmptyChart() {
  return (
    <div className="h-[200px] flex items-center justify-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
      Not enough samples yet
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Helpers + chart builders                                                    */
/* -------------------------------------------------------------------------- */

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

function avg(a: number[]): number {
  if (a.length === 0) return 0;
  let s = 0;
  for (const v of a) s += v;
  return s / a.length;
}

function fmtX(ts: number): string {
  const d = new Date(ts);
  return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function buildThroughputChart(samples: RunSample[]): EChartsOption {
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: samples.map((s) => fmtX(s.t)),
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 8)) },
    },
    yAxis: {
      type: "value",
      name: "Mbps",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
    },
    series: [
      {
        name: "TX",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +(((s.txBps ?? 0) / 1e6).toFixed(1))),
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
        name: "RX",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +(((s.rxBps ?? 0) / 1e6).toFixed(1))),
        lineStyle: { color: BLUE, width: 1.4 },
      },
    ],
  };
}

function buildQualityChart(samples: RunSample[]): EChartsOption {
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: samples.map((s) => fmtX(s.t)),
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(samples.length / 6)) },
    },
    yAxis: {
      type: "value",
      name: "%",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
      min: 80,
      max: 100,
    },
    series: [
      {
        name: "Quality",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: samples.map((s) => +(s.linkQualityPct ?? 0).toFixed(2)),
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
    ],
  };
}

function buildFecRateChart(samples: RunSample[]): EChartsOption {
  // Recovery rate per-sample from cumulative counters. Handle counter resets
  // (new daemon run within the same session) by flooring negative deltas.
  const rates: Array<{ t: number; rate: number }> = [];
  for (let i = 1; i < samples.length; i++) {
    const prev = samples[i - 1];
    const cur = samples[i];
    const dAtt = (cur.blocksAttempted ?? 0) - (prev.blocksAttempted ?? 0);
    const dRec = (cur.blocksRecovered ?? 0) - (prev.blocksRecovered ?? 0);
    if (dAtt <= 0) continue;
    rates.push({ t: cur.t, rate: (dRec / dAtt) * 100 });
  }
  return {
    ...baseOpts,
    xAxis: {
      type: "category",
      data: rates.map((r) => fmtX(r.t)),
      ...baseAxis,
      axisLabel: { ...baseAxis.axisLabel, interval: Math.max(1, Math.floor(rates.length / 6)) },
    },
    yAxis: {
      type: "value",
      name: "%",
      nameTextStyle: { color: TEXT_MUTED, fontSize: 10 },
      ...baseAxis,
      min: 90,
      max: 100,
    },
    series: [
      {
        name: "Recovery",
        type: "line",
        smooth: true,
        showSymbol: false,
        data: rates.map((r) => +r.rate.toFixed(3)),
        lineStyle: { color: AMBER, width: 1.6 },
      },
    ],
  };
}
