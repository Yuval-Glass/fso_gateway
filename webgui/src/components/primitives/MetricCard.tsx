import { cn } from "@/lib/utils";
import { GlassPanel } from "./GlassPanel";
import { Sparkline } from "./Sparkline";
import type { ReactNode } from "react";

type Tone = "neutral" | "cyan" | "success" | "warning" | "danger";

interface MetricCardProps {
  label: string;
  value: string | number;
  unit?: string;
  sub?: ReactNode;
  spark?: number[];
  tone?: Tone;
  icon?: ReactNode;
}

const toneMap: Record<Tone, { text: string; color: string; border: string }> = {
  neutral: { text: "text-[color:var(--color-text-primary)]", color: "var(--color-cyan-500)", border: "" },
  cyan: { text: "text-[color:var(--color-cyan-300)]", color: "var(--color-cyan-500)", border: "" },
  success: { text: "text-[color:var(--color-success)]", color: "var(--color-success)", border: "" },
  warning: { text: "text-[color:var(--color-warning)]", color: "var(--color-warning)", border: "" },
  danger: { text: "text-[color:var(--color-danger)]", color: "var(--color-danger)", border: "" },
};

export function MetricCard({ label, value, unit, sub, spark, tone = "neutral", icon }: MetricCardProps) {
  const t = toneMap[tone];
  return (
    <GlassPanel padded={false} className="group">
      <div className="px-4 pt-3 pb-3 flex flex-col h-full">
        <div className="flex items-center justify-between mb-2">
          <span className="text-[10px] font-medium tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)]">
            {label}
          </span>
          {icon && <span className="text-[color:var(--color-text-muted)] opacity-80">{icon}</span>}
        </div>
        <div className="flex items-baseline gap-1.5">
          <span
            className={cn(
              "font-display text-3xl font-semibold tabular tracking-tight",
              t.text,
            )}
            style={tone === "cyan" ? { textShadow: "0 0 24px rgba(0, 212, 255, 0.35)" } : undefined}
          >
            {value}
          </span>
          {unit && (
            <span className="text-xs font-medium text-[color:var(--color-text-muted)]">{unit}</span>
          )}
        </div>
        {sub && <div className="mt-1 text-[11px] text-[color:var(--color-text-secondary)]">{sub}</div>}
        {spark && spark.length > 1 && (
          <div className="mt-3 -mx-1 opacity-90 group-hover:opacity-100 transition-opacity">
            <Sparkline values={spark} color={t.color} width={220} height={34} />
          </div>
        )}
      </div>
    </GlassPanel>
  );
}
