import Link from "next/link";
import { cn } from "@/lib/utils";
import { GlassPanel } from "./GlassPanel";
import { Sparkline } from "./Sparkline";
import { FieldHint } from "./FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
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
  hintId?: FieldHintId;
  /** If set, the whole card becomes a navigation link. */
  href?: string;
}

const toneMap: Record<Tone, { text: string; color: string; border: string }> = {
  neutral: { text: "text-[color:var(--color-text-primary)]", color: "var(--color-cyan-500)", border: "" },
  cyan: { text: "text-[color:var(--color-cyan-300)]", color: "var(--color-cyan-500)", border: "" },
  success: { text: "text-[color:var(--color-success)]", color: "var(--color-success)", border: "" },
  warning: { text: "text-[color:var(--color-warning)]", color: "var(--color-warning)", border: "" },
  danger: { text: "text-[color:var(--color-danger)]", color: "var(--color-danger)", border: "" },
};

export function MetricCard({ label, value, unit, sub, spark, tone = "neutral", icon, hintId, href }: MetricCardProps) {
  const t = toneMap[tone];
  const inner = (
    <GlassPanel
      padded={false}
      className={cn(
        "group h-full transition-[transform,box-shadow,border-color] duration-200 ease-out",
        // Subtle hover on every card — gives the dashboard a 'live' feel.
        "hover:border-[color:var(--color-border-cyan)] hover:shadow-[0_0_18px_rgba(0,212,255,0.10)]",
        // Stronger lift only when the card is actually clickable.
        href && [
          "cursor-pointer",
          "hover:-translate-y-0.5 hover:scale-[1.015]",
          "hover:shadow-[0_0_24px_rgba(0,212,255,0.20)]",
        ],
      )}
    >
      <div className="px-4 pt-3 pb-3 flex flex-col h-full">
        <div className="flex items-center justify-between mb-2">
          <span className="text-[10px] font-medium tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] inline-flex items-center gap-1">
            <span>{label}</span>
            {hintId && <FieldHint id={hintId} size={11} />}
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
        {/* Sub text reserves a 2-line slot so the card height stays stable
            even when long values like "3,497,288 / 3,500,772 blocks" wrap.
            Without this the whole grid row reflows once per second. */}
        {sub && (
          <div
            className="mt-1 text-[11px] leading-snug text-[color:var(--color-text-secondary)] line-clamp-2"
            style={{ minHeight: "calc(11px * 1.375 * 2)" }}
          >
            {sub}
          </div>
        )}
        {spark && spark.length > 1 && (
          <div className="mt-3 -mx-1 opacity-90 group-hover:opacity-100 transition-opacity">
            <Sparkline values={spark} color={t.color} width={220} height={34} />
          </div>
        )}
      </div>
    </GlassPanel>
  );
  return href ? (
    <Link href={href} className="block focus:outline-none focus-visible:ring-1 focus-visible:ring-[color:var(--color-cyan-500)] rounded-lg">
      {inner}
    </Link>
  ) : (
    inner
  );
}
