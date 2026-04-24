"use client";

import { cn } from "@/lib/utils";
import type { LinkState } from "@/types/telemetry";

interface PulseRingProps {
  state: LinkState;
  qualityPct: number;
  size?: number;
}

const stateConfig: Record<
  LinkState,
  { label: string; color: string; glow: string; ringOpacity: number }
> = {
  online: {
    label: "LINK ONLINE",
    color: "var(--color-cyan-500)",
    glow: "rgba(0, 212, 255, 0.5)",
    ringOpacity: 1,
  },
  degraded: {
    label: "LINK DEGRADED",
    color: "var(--color-warning)",
    glow: "rgba(255, 176, 32, 0.5)",
    ringOpacity: 0.85,
  },
  offline: {
    label: "LINK OFFLINE",
    color: "var(--color-danger)",
    glow: "rgba(255, 45, 92, 0.5)",
    ringOpacity: 0.75,
  },
};

export function PulseRing({ state, qualityPct, size = 280 }: PulseRingProps) {
  const cfg = stateConfig[state];
  const r = size / 2 - 12;
  const cx = size / 2;
  const cy = size / 2;
  const circumference = 2 * Math.PI * r;
  const arcLen = (qualityPct / 100) * circumference;

  return (
    <div
      className="relative flex items-center justify-center"
      style={{ width: size, height: size }}
    >
      {/* Outer pulse waves */}
      <div
        className="absolute inset-0 rounded-full pulse-ring"
        style={{ border: `2px solid ${cfg.color}`, opacity: cfg.ringOpacity }}
      />
      <div
        className="absolute inset-0 rounded-full pulse-ring"
        style={{
          border: `1px solid ${cfg.color}`,
          opacity: cfg.ringOpacity * 0.7,
          animationDelay: "0.6s",
        }}
      />
      <div
        className="absolute inset-0 rounded-full pulse-ring"
        style={{
          border: `1px solid ${cfg.color}`,
          opacity: cfg.ringOpacity * 0.4,
          animationDelay: "1.2s",
        }}
      />

      {/* Main SVG ring */}
      <svg width={size} height={size} className="absolute inset-0 -rotate-90">
        {/* Track */}
        <circle
          cx={cx}
          cy={cy}
          r={r}
          fill="none"
          stroke="rgba(255,255,255,0.06)"
          strokeWidth="3"
        />
        {/* Quality arc */}
        <circle
          cx={cx}
          cy={cy}
          r={r}
          fill="none"
          stroke={cfg.color}
          strokeWidth="3"
          strokeLinecap="round"
          strokeDasharray={`${arcLen} ${circumference}`}
          style={{
            filter: `drop-shadow(0 0 12px ${cfg.glow})`,
            transition: "stroke-dasharray 800ms cubic-bezier(0.4, 0, 0.2, 1)",
          }}
        />
        {/* Tick marks every 10% */}
        {Array.from({ length: 36 }).map((_, i) => {
          const a = (i / 36) * Math.PI * 2;
          const x1 = cx + Math.cos(a) * (r - 6);
          const y1 = cy + Math.sin(a) * (r - 6);
          const x2 = cx + Math.cos(a) * (r - 2);
          const y2 = cy + Math.sin(a) * (r - 2);
          const major = i % 9 === 0;
          return (
            <line
              key={i}
              x1={x1}
              y1={y1}
              x2={x2}
              y2={y2}
              stroke="rgba(255,255,255,0.15)"
              strokeWidth={major ? 1.5 : 1}
            />
          );
        })}
      </svg>

      {/* Inner content */}
      <div className="relative z-10 flex flex-col items-center justify-center text-center">
        <div
          className={cn("text-[10px] font-medium tracking-[0.3em] mb-2 breathe")}
          style={{ color: cfg.color }}
        >
          {cfg.label}
        </div>
        <div
          className="font-display font-bold tabular"
          style={{
            fontSize: size * 0.18,
            color: cfg.color,
            textShadow: `0 0 24px ${cfg.glow}`,
            lineHeight: 1,
          }}
        >
          {qualityPct.toFixed(1)}
          <span className="text-[0.45em] text-[color:var(--color-text-secondary)] ml-1">%</span>
        </div>
        <div className="mt-2 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Link Quality
        </div>
      </div>
    </div>
  );
}
