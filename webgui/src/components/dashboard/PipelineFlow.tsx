"use client";

import { GlassPanel } from "@/components/primitives/GlassPanel";
import type { PipelineStageStats } from "@/types/telemetry";
import { cn, formatNumber } from "@/lib/utils";

export function PipelineFlow({ stages }: { stages: PipelineStageStats[] }) {
  return (
    <GlassPanel label="Data Pipeline" trailing={
      <div className="flex items-center gap-4 text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        <Legend color="var(--color-cyan-500)" label="Active" />
        <Legend color="var(--color-warning)" label="Queued" />
      </div>
    }>
      <div className="grid grid-cols-[1fr_auto_1fr] gap-3 py-3">
        <FlowStrip title="TX Pipeline  (LAN → FSO)" stages={stages.slice(0, 5)} />
        <div className="self-center text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] rotate-90 whitespace-nowrap px-1">
          FSO Air
        </div>
        <FlowStrip title="RX Pipeline  (FSO → LAN)" stages={stages.slice(5)} />
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

function FlowStrip({ title, stages }: { title: string; stages: PipelineStageStats[] }) {
  return (
    <div>
      <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-secondary)] mb-2 px-1">
        {title}
      </div>
      <div className="flex items-stretch gap-1 overflow-hidden">
        {stages.map((s, i) => (
          <Stage key={s.name} stage={s} isLast={i === stages.length - 1} />
        ))}
      </div>
    </div>
  );
}

function Stage({ stage, isLast }: { stage: PipelineStageStats; isLast: boolean }) {
  const hot = stage.queueDepth > 20;
  return (
    <div className="flex items-stretch flex-1 min-w-0 gap-1">
      <div
        className={cn(
          "flex-1 min-w-0 rounded-md px-2.5 py-2 border relative overflow-hidden",
          "bg-gradient-to-br from-white/[0.03] to-transparent",
          hot
            ? "border-[color:var(--color-warning)]/50"
            : "border-[color:var(--color-border-cyan)]/40",
        )}
        style={{
          boxShadow: hot
            ? "inset 0 0 20px rgba(255, 176, 32, 0.1)"
            : "inset 0 0 20px rgba(0, 212, 255, 0.08)",
        }}
      >
        <div className="text-[10px] tracking-[0.15em] uppercase font-medium text-[color:var(--color-text-primary)] truncate">
          {stage.name}
        </div>
        <div className="mt-1 flex items-baseline gap-1.5">
          <span className="font-mono text-[11px] tabular text-[color:var(--color-cyan-300)]">
            {formatNumber(stage.queueDepth)}
          </span>
          <span className="text-[9px] text-[color:var(--color-text-muted)]">queued</span>
        </div>
        <div className="text-[9px] font-mono text-[color:var(--color-text-muted)] mt-0.5">
          {stage.processingUs.toFixed(1)} µs
        </div>

        {/* Flow particle */}
        <div
          className="absolute inset-y-1/2 left-0 w-2 h-0.5 -translate-y-1/2 flow-particle pointer-events-none"
          style={{
            background:
              "linear-gradient(90deg, transparent, rgba(0, 212, 255, 0.9), transparent)",
            boxShadow: "0 0 8px rgba(0, 212, 255, 0.6)",
            // @ts-expect-error — css var
            "--flow-distance": "400%",
            animationDuration: `${1.2 + Math.random() * 0.8}s`,
          }}
        />
      </div>
      {!isLast && (
        <div className="flex items-center text-[color:var(--color-cyan-500)]/60 text-lg leading-none pointer-events-none select-none">
          →
        </div>
      )}
    </div>
  );
}
