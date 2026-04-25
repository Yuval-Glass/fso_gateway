"use client";

import { useMemo } from "react";
import { Boxes, Grid3x3, Network, Shield, Zap } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { FieldHint } from "@/components/primitives/FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
import { useTelemetry } from "@/lib/useTelemetry";
import { useConfig } from "@/lib/useConfig";
import { useDaemon } from "@/lib/useDaemon";
import { cn, formatBytes, formatNumber, formatPercent } from "@/lib/utils";

export default function InterleaverPage() {
  const { snapshot: snap } = useTelemetry();
  // The matrix should reflect what the user is configuring, not what the
  // daemon happens to be running with. Prefer the editable draft, fall back
  // to configEcho (the running daemon's config) when no draft is loaded.
  const cfgState = useConfig();
  const { status: daemonStatus } = useDaemon();

  // Active parameters: draft > configEcho > zeros.
  const draft = cfgState.draft;
  const echo = snap?.configEcho;
  const k = draft?.k ?? echo?.k ?? 0;
  const m = draft?.m ?? echo?.m ?? 0;
  const depth = draft?.depth ?? echo?.depth ?? 0;
  const symSize = draft?.symbol_size ?? echo?.symbolSize ?? 0;
  const haveCfg = (k + m) > 0 && depth > 0;

  // Only warn about a mismatch when the daemon is actually running with
  // different params. If it's stopped, configEcho is just stale cached
  // data from a previous run and the warning is noise.
  const daemonRunning = daemonStatus?.state === "running";
  const echoMismatch = daemonRunning && !!echo && (
    echo.k !== k || echo.m !== m || echo.depth !== depth || echo.symbolSize !== symSize
  );

  const matrixCells = useMemo(() => {
    if (!haveCfg) return null;
    return computeMatrixCells(depth, k + m);
  }, [depth, k, m, haveCfg]);

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading interleaver telemetry…
        </div>
      </div>
    );
  }

  const total = k + m;
  const cellCount = depth * total;
  const recoverySpanSymbols = m * depth;
  const matrixBytes = cellCount * symSize;

  const stress = snap.decoderStress;
  const worstHoles = stress?.worstHolesInBlock ?? 0;
  const recoverable = stress?.recoverableBursts ?? 0;
  const critical = stress?.criticalBursts ?? 0;
  const exceeding = stress?.burstsExceedingFecSpan ?? 0;

  const totalBursts = snap.burstHistogram.reduce((a, b) => a + b.count, 0);
  const withinSpan = snap.burstHistogram
    .filter((b) => burstUpperBound(b.label) <= recoverySpanSymbols)
    .reduce((a, b) => a + b.count, 0);
  const coverage = totalBursts > 0 ? withinSpan / totalBursts : 1;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Interleaver
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Matrix interleaver visualization, burst coverage, and decoder health.
            Depth × (K+M) defines the recovery window.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4G · Interleaver View
        </div>
      </div>

      {echoMismatch && (
        <div
          className="px-3 py-2 rounded-md border text-[11px] flex items-start gap-2"
          style={{
            borderColor: "rgba(255, 176, 32, 0.5)",
            background: "rgba(255, 176, 32, 0.08)",
            color: "var(--color-warning)",
          }}
        >
          <Zap size={13} className="mt-0.5 shrink-0" />
          <span>
            Showing edited config (K={k}, M={m}, depth={depth}, sym={symSize}). The
            running daemon still uses {echo?.k}/{echo?.m}/{echo?.depth}/{echo?.symbolSize}.
            Save and restart the gateway from <span className="font-mono">/configuration</span> to apply.
          </span>
        </div>
      )}

      {/* Top strip */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <MetricCard
          label="Matrix"
          value={haveCfg ? `${depth} × ${total}` : "—"}
          tone="cyan"
          icon={<Grid3x3 size={14} />}
          hintId="interleaver.matrix"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatNumber(cellCount)} cells · {formatBytes(matrixBytes, 1)}
            </span>
          }
        />
        <MetricCard
          label="Recovery Span"
          value={haveCfg ? formatNumber(recoverySpanSymbols) : "—"}
          unit="symbols"
          tone="success"
          icon={<Shield size={14} />}
          hintId="interleaver.recoverySpan"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              m ({m}) × depth ({depth})
            </span>
          }
        />
        <MetricCard
          label="Burst Coverage"
          value={formatPercent(coverage, 2)}
          tone={coverage > 0.98 ? "success" : coverage > 0.9 ? "cyan" : "warning"}
          icon={<Network size={14} />}
          hintId="interleaver.burstCoverage"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {formatNumber(withinSpan)} / {formatNumber(totalBursts)} bursts within span
            </span>
          }
        />
        <MetricCard
          label="Exceeding Span"
          value={formatNumber(exceeding)}
          tone={exceeding > 0 ? "danger" : "success"}
          icon={<Zap size={14} />}
          hintId="stress.exceedingFecSpan"
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              Critical: {formatNumber(critical)} · Recoverable: {formatNumber(recoverable)}
            </span>
          }
        />
      </div>

      {/* Main row: matrix + derivations */}
      <div className="grid grid-cols-1 xl:grid-cols-[2fr_1fr] gap-4">
        <GlassPanel
          label="Matrix Layout"
          trailing={
            <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              <Legend color="var(--color-cyan-500)" label="Data (K)" />
              <Legend color="var(--color-warning)" label="Repair (M)" />
              <Legend color="var(--color-text-dim)" label="Empty" />
            </div>
          }
        >
          {!matrixCells ? (
            <div className="h-[260px] flex items-center justify-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
              Config unavailable
            </div>
          ) : (
            <MatrixCanvas
              rows={matrixCells.rows}
              cols={matrixCells.cols}
              k={k}
              cellsFlat={matrixCells.cells}
            />
          )}
        </GlassPanel>

        <GlassPanel label="Derivations & Knobs">
          <ul className="text-[11px] space-y-2 py-1">
            <KV label="Depth (rows)" value={formatNumber(depth)} tone="cyan" hintId="interleaver.depth" />
            <KV label="Block width (K+M)" value={formatNumber(total)} hintId="interleaver.blockWidth" />
            <KV label="Symbol size" value={symSize > 0 ? `${formatNumber(symSize)} B` : "—"} hintId="interleaver.symbolSize" />
            <KV label="Symbols per matrix" value={formatNumber(cellCount)} hintId="interleaver.cells" />
            <KV label="Bytes per matrix" value={formatBytes(matrixBytes, 1)} hintId="interleaver.matrixBytes" />
            <li className="border-t border-[color:var(--color-border-hair)] my-2" />
            <KV label="Max burst recovered" value={formatNumber(recoverySpanSymbols)} tone="success" hintId="interleaver.recoverySpan" />
            <KV label="Worst holes / block" value={formatNumber(worstHoles)}
                tone={worstHoles > m ? "danger" : worstHoles > m / 2 ? "warning" : "neutral"} hintId="stress.worstHolesInBlock" />
          </ul>
          <div className="mt-3 text-[10px] leading-relaxed text-[color:var(--color-text-muted)]">
            Each column is a FEC block ({k} data + {m} repair symbols). Column-major
            read-out spreads a contiguous burst across {depth} different blocks, so a
            burst of up to <span className="text-[color:var(--color-cyan-300)]">{recoverySpanSymbols}</span> symbols
            maps to at most {m} holes per block — fully recoverable.
          </div>
        </GlassPanel>
      </div>

      {/* Burst-coverage visualization as a stacked bar */}
      <GlassPanel
        label="Burst Coverage vs Recovery Span"
        hintId="interleaver.burstCoverage"
        trailing={
          <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            span = {recoverySpanSymbols} symbols
          </span>
        }
      >
        <BurstCoverageBar
          buckets={snap.burstHistogram}
          spanSymbols={recoverySpanSymbols}
          total={totalBursts}
        />
      </GlassPanel>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Matrix canvas                                                               */
/* -------------------------------------------------------------------------- */

interface MatrixCellData {
  row: number;
  col: number;
  kind: "data" | "repair";
}

interface ComputedCells {
  rows: number;
  cols: number;
  cells: MatrixCellData[];
}

// Above this many cells we stop rendering one rect per cell — the DOM cost
// crushes the renderer (and ultimately page navigation, since the unmount
// has to dispose every node). For dense grids we collapse each row into
// just two rects (a cyan K-band and an amber M-band).
const PER_CELL_LIMIT = 2048;

function computeMatrixCells(depth: number, total: number): ComputedCells {
  // No clamping — MatrixCanvas uses a responsive viewBox so it can render
  // any K+M × depth matrix that the daemon accepts.
  const rows = Math.max(1, depth);
  const cols = Math.max(1, total);
  // Skip per-cell allocation when the grid is dense (the canvas will render
  // a per-row aggregate view in that case). Avoids holding ~92k objects in
  // memory for depth=1024 × K+M=90.
  if (rows * cols > PER_CELL_LIMIT) {
    return { rows, cols, cells: [] };
  }
  const cells: MatrixCellData[] = [];
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      cells.push({ row: r, col: c, kind: "data" });
    }
  }
  return { rows, cols, cells };
}

function MatrixCanvas({
  rows,
  cols,
  k,
  cellsFlat,
}: {
  rows: number;
  cols: number;
  k: number;
  cellsFlat: MatrixCellData[];
}) {
  // viewBox-coordinate sizes — the SVG itself scales to its container,
  // so these only define cell:gap proportions, not actual pixel sizes.
  const CELL = 10;
  const GAP = 2;
  const vbW = cols * (CELL + GAP);
  const vbH = rows * (CELL + GAP);
  const cellCount = rows * cols;
  const dense = cellCount > PER_CELL_LIMIT;
  // Disable per-cell breathe animation when the grid is dense.
  const animate = cellCount <= 256;

  return (
    <div className="flex flex-col items-center gap-3 py-3 w-full">
      <div
        className="relative w-full"
        style={{
          maxWidth: 1100,
          background:
            "radial-gradient(ellipse at center, rgba(0, 212, 255, 0.08), transparent 70%)",
        }}
      >
        <svg
          viewBox={`0 0 ${vbW} ${vbH}`}
          preserveAspectRatio="xMidYMid meet"
          style={{ width: "100%", maxHeight: 320, display: "block" }}
          shapeRendering="optimizeSpeed"
        >
          {dense
            ? // Per-row mode: two rects per row (K band + M band).
              // 2 × rows = at most ~2048 elements even for depth=1024.
              Array.from({ length: rows }, (_, r) => {
                const y = r * (CELL + GAP);
                const dataW = k * (CELL + GAP) - GAP;
                const repairX = k * (CELL + GAP);
                const repairW = (cols - k) * (CELL + GAP) - GAP;
                return (
                  <g key={r}>
                    {k > 0 && (
                      <rect
                        x={0}
                        y={y}
                        width={dataW}
                        height={CELL}
                        fill="rgba(0, 212, 255, 0.20)"
                      />
                    )}
                    {cols > k && (
                      <rect
                        x={repairX}
                        y={y}
                        width={repairW}
                        height={CELL}
                        fill="rgba(255, 176, 32, 0.25)"
                      />
                    )}
                  </g>
                );
              })
            : cellsFlat.map((cell) => {
                const isRepair = cell.col >= k;
                const fill = isRepair ? "rgba(255, 176, 32, 0.22)" : "rgba(0, 212, 255, 0.18)";
                const stroke = isRepair ? "rgba(255, 176, 32, 0.5)" : "rgba(0, 212, 255, 0.45)";
                const x = cell.col * (CELL + GAP);
                const y = cell.row * (CELL + GAP);
                return (
                  <rect
                    key={`${cell.row}-${cell.col}`}
                    x={x}
                    y={y}
                    width={CELL}
                    height={CELL}
                    fill={fill}
                    stroke={stroke}
                    strokeWidth={0.6}
                    rx={1}
                  >
                    {animate && (
                      <animate
                        attributeName="opacity"
                        dur={`${2 + (cell.row * 0.13 + cell.col * 0.07) % 2}s`}
                        repeatCount="indefinite"
                        values="0.55; 1; 0.55"
                        keyTimes="0; 0.5; 1"
                      />
                    )}
                  </rect>
                );
              })}
        </svg>
      </div>
      <div className="flex gap-6 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] flex-wrap justify-center">
        <span>
          rows = depth (<span className="text-[color:var(--color-cyan-300)]">{rows}</span>)
        </span>
        <span>
          cols = K+M (<span className="text-[color:var(--color-cyan-300)]">{cols}</span>)
        </span>
        <span>
          K = <span className="text-[color:var(--color-cyan-300)]">{k}</span>
          · repair starts at col {k}
        </span>
        {dense && (
          <span className="text-[color:var(--color-text-muted)]">
            · {rows.toLocaleString()}×{cols.toLocaleString()} = aggregate view
          </span>
        )}
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Burst coverage bar                                                          */
/* -------------------------------------------------------------------------- */

function BurstCoverageBar({
  buckets,
  spanSymbols,
  total,
}: {
  buckets: Array<{ label: string; count: number }>;
  spanSymbols: number;
  total: number;
}) {
  if (total === 0) {
    return (
      <div className="py-6 text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] text-center">
        No bursts observed yet
      </div>
    );
  }
  return (
    <div className="py-3">
      <div className="flex h-10 rounded-md overflow-hidden ring-1 ring-[color:var(--color-border-hair)]">
        {buckets.map((b) => {
          const pct = (b.count / total) * 100;
          if (pct <= 0) return null;
          const within = burstUpperBound(b.label) <= spanSymbols;
          const bg = within
            ? "linear-gradient(180deg, rgba(0, 212, 255, 0.55), rgba(0, 212, 255, 0.35))"
            : "linear-gradient(180deg, rgba(255, 45, 92, 0.6), rgba(255, 45, 92, 0.35))";
          return (
            <div
              key={b.label}
              className="flex items-center justify-center text-[10px] font-mono"
              style={{
                flexBasis: `${pct}%`,
                background: bg,
                color: within ? "var(--color-cyan-50)" : "#ffe0e7",
                textShadow: "0 0 4px rgba(0,0,0,0.5)",
                borderRight: "1px solid rgba(255,255,255,0.08)",
              }}
              title={`${b.label}: ${b.count} (${pct.toFixed(1)}%)`}
            >
              {pct > 6 && <span>{b.label}</span>}
            </div>
          );
        })}
      </div>
      <div className="mt-2 flex items-center justify-between text-[10px] tracking-[0.18em] uppercase">
        <span className="text-[color:var(--color-cyan-300)]">
          ← within FEC span (recoverable)
        </span>
        <span className="text-[color:var(--color-danger)]">
          exceeding span →
        </span>
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

function burstUpperBound(label: string): number {
  // Buckets come from C code: "1", "2-5", "6-10", "11-50", "51-100",
  // "101-500", "501+" — take the upper bound for span comparison.
  if (label.endsWith("+")) return Number.POSITIVE_INFINITY;
  const parts = label.split("-");
  const last = parts[parts.length - 1];
  const n = parseInt(last, 10);
  return Number.isFinite(n) ? n : Number.POSITIVE_INFINITY;
}

function KV({
  label,
  value,
  tone = "neutral",
  hintId,
}: {
  label: string;
  value: string;
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
      <span className={cn("tabular font-mono")} style={{ color: colorMap[tone] }}>
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
