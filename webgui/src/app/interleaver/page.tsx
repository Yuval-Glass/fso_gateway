"use client";

import { useMemo } from "react";
import { Boxes, Grid3x3, Network, Shield, Zap } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatBytes, formatNumber, formatPercent } from "@/lib/utils";

export default function InterleaverPage() {
  const { snapshot: snap } = useTelemetry();

  const matrixCells = useMemo(() => {
    if (!snap?.configEcho) return null;
    return computeMatrixCells(snap.configEcho.depth, snap.configEcho.k + snap.configEcho.m);
  }, [snap?.configEcho?.depth, snap?.configEcho?.k, snap?.configEcho?.m]);

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading interleaver telemetry…
        </div>
      </div>
    );
  }

  const cfg = snap.configEcho;
  const haveCfg = !!cfg;
  const depth = cfg?.depth ?? 0;
  const k = cfg?.k ?? 0;
  const m = cfg?.m ?? 0;
  const symSize = cfg?.symbolSize ?? 0;
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

      {/* Top strip */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <MetricCard
          label="Matrix"
          value={haveCfg ? `${depth} × ${total}` : "—"}
          tone="cyan"
          icon={<Grid3x3 size={14} />}
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
            <KV label="Depth (rows)" value={formatNumber(depth)} tone="cyan" />
            <KV label="Block width (K+M)" value={formatNumber(total)} />
            <KV label="Symbol size" value={symSize > 0 ? `${formatNumber(symSize)} B` : "—"} />
            <KV label="Symbols per matrix" value={formatNumber(cellCount)} />
            <KV label="Bytes per matrix" value={formatBytes(matrixBytes, 1)} />
            <li className="border-t border-[color:var(--color-border-hair)] my-2" />
            <KV label="Max burst recovered" value={formatNumber(recoverySpanSymbols)} tone="success" />
            <KV label="Worst holes / block" value={formatNumber(worstHoles)}
                tone={worstHoles > m ? "danger" : worstHoles > m / 2 ? "warning" : "neutral"} />
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

function computeMatrixCells(depth: number, total: number): ComputedCells {
  const rows = Math.max(1, Math.min(depth, 32)); // clamp for visual sanity
  const cols = Math.max(1, Math.min(total, 32));
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
  const cellSize = Math.max(10, Math.min(26, Math.floor(680 / Math.max(rows, cols))));
  const gap = 3;
  const totalW = cols * (cellSize + gap);
  const totalH = rows * (cellSize + gap);

  return (
    <div className="flex flex-col items-center gap-3 py-3">
      <div
        className="relative"
        style={{
          width: totalW,
          height: totalH,
          background:
            "radial-gradient(ellipse at center, rgba(0, 212, 255, 0.08), transparent 70%)",
        }}
      >
        <svg width={totalW} height={totalH} style={{ display: "block" }}>
          {cellsFlat.map((cell) => {
            const isRepair = cell.col >= k;
            const fill = isRepair ? "rgba(255, 176, 32, 0.22)" : "rgba(0, 212, 255, 0.18)";
            const stroke = isRepair ? "rgba(255, 176, 32, 0.5)" : "rgba(0, 212, 255, 0.45)";
            const x = cell.col * (cellSize + gap);
            const y = cell.row * (cellSize + gap);
            return (
              <g key={`${cell.row}-${cell.col}`}>
                <rect
                  x={x}
                  y={y}
                  width={cellSize}
                  height={cellSize}
                  fill={fill}
                  stroke={stroke}
                  strokeWidth={0.6}
                  rx={1.5}
                >
                  <animate
                    attributeName="opacity"
                    from="0.5"
                    to="1"
                    dur={`${2 + (cell.row * 0.13 + cell.col * 0.07) % 2}s`}
                    repeatCount="indefinite"
                    values="0.55; 1; 0.55"
                    keyTimes="0; 0.5; 1"
                  />
                </rect>
              </g>
            );
          })}
        </svg>
      </div>
      <div className="flex gap-6 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
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
}: {
  label: string;
  value: string;
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
