"use client";

import { memo } from "react";
import { cn } from "@/lib/utils";
import type { LinkState } from "@/types/telemetry";

interface PulseRingProps {
  state: LinkState;
  qualityPct: number;
  size?: number;
}

const stateConfig: Record<
  LinkState,
  {
    label: string;
    color: string;
    glow: string;
    glowSoft: string;
    arcGrad: [string, string];
    ringOpacity: number;
  }
> = {
  online: {
    label: "LINK ONLINE",
    color: "var(--color-cyan-300)",
    glow: "rgba(0, 212, 255, 0.55)",
    glowSoft: "rgba(0, 212, 255, 0.18)",
    arcGrad: ["#00d4ff", "#5aa0ff"],
    ringOpacity: 1,
  },
  degraded: {
    label: "LINK DEGRADED",
    color: "var(--color-warning)",
    glow: "rgba(255, 176, 32, 0.55)",
    glowSoft: "rgba(255, 176, 32, 0.16)",
    arcGrad: ["#ffb020", "#ff8a00"],
    ringOpacity: 0.92,
  },
  offline: {
    label: "LINK OFFLINE",
    color: "var(--color-danger)",
    glow: "rgba(255, 45, 92, 0.55)",
    glowSoft: "rgba(255, 45, 92, 0.18)",
    arcGrad: ["#ff2d5c", "#c81b48"],
    ringOpacity: 0.85,
  },
};

/**
 * Hero "link integrity" gauge for the dashboard.
 *
 * It is a layered composition designed to feel alive without being jittery:
 *
 *   1. Outer chevron ring — slow rotation (40s), aerospace bezel feel.
 *   2. Pulse waves — three staggered rings emanating outward.
 *   3. Soft halo disk — depth glow behind the gauge.
 *   4. Tick marks on the gauge ring.
 *   5. Quality arc with a gradient stroke + drop-shadow glow.
 *   6. Radar sweep — conic gradient rotating once per ~6s.
 *   7. Inner counter-rotating chevron ring (25s, opposite direction).
 *   8. Orbital particles at three radii with different periods.
 *   9. Compass crosshair (N/E/S/W).
 *  10. Center: breathing label + percentage with halo, subtle drop-cap "%".
 */
function PulseRingImpl({ state, qualityPct, size = 280 }: PulseRingProps) {
  const cfg = stateConfig[state];

  // Geometry of the SVG layer
  const cx = size / 2;
  const cy = size / 2;
  const rOuter = size / 2 - 12;            // gauge ring
  const circumference = 2 * Math.PI * rOuter;
  const arcLen = (qualityPct / 100) * circumference;

  // Outer chevron-bezel radius (sits *outside* the gauge)
  const bezelSize = size + 32;

  // Particles fly at 3 different radii so the ring feels populated
  const particleRadii = [rOuter + 18, rOuter - 8, rOuter - 28];

  return (
    <div
      // Container is the size of the gauge but we let children expand
      // outwards visually. `overflow: visible` is implicit; do NOT clip
      // here so pulse rings, bezel, particles can spill outside.
      className="relative flex items-center justify-center isolate"
      style={{ width: size, height: size }}
      aria-label={cfg.label}
    >
      {/* ---------- 0. Outer chevron bezel (rotating, behind everything) ---------- */}
      <div
        className="absolute rounded-full ring-spin-slow"
        style={{
          width: bezelSize,
          height: bezelSize,
          left: (size - bezelSize) / 2,
          top: (size - bezelSize) / 2,
          opacity: 0.5,
        }}
      >
        <svg width={bezelSize} height={bezelSize} className="block">
          {Array.from({ length: 12 }).map((_, i) => {
            const a0 = (i / 12) * Math.PI * 2;
            const a1 = a0 + Math.PI * 2 * (i % 3 === 0 ? 0.06 : 0.025);
            const r = bezelSize / 2 - 1;
            const c = bezelSize / 2;
            const x0 = c + Math.cos(a0) * r;
            const y0 = c + Math.sin(a0) * r;
            const x1 = c + Math.cos(a1) * r;
            const y1 = c + Math.sin(a1) * r;
            const large = a1 - a0 > Math.PI ? 1 : 0;
            return (
              <path
                key={i}
                d={`M ${x0} ${y0} A ${r} ${r} 0 ${large} 1 ${x1} ${y1}`}
                fill="none"
                stroke={cfg.color}
                strokeWidth={i % 3 === 0 ? 2 : 1}
                strokeLinecap="round"
                opacity={i % 3 === 0 ? 0.9 : 0.5}
              />
            );
          })}
        </svg>
      </div>

      {/* ---------- 1. Soft halo disk (depth) ---------- */}
      <div
        className="absolute rounded-full"
        style={{
          width: size + 60,
          height: size + 60,
          left: -30,
          top: -30,
          background: `radial-gradient(circle, ${cfg.glowSoft} 0%, transparent 65%)`,
          pointerEvents: "none",
        }}
      />

      {/* ---------- 2. Pulse waves emanating outward ---------- */}
      {[0, 0.7, 1.4].map((delay, i) => (
        <div
          key={i}
          className="absolute inset-0 rounded-full pulse-ring"
          style={{
            border: `${i === 0 ? 2 : 1}px solid ${cfg.color}`,
            opacity: cfg.ringOpacity * (i === 0 ? 1 : i === 1 ? 0.6 : 0.35),
            boxShadow: i === 0 ? `0 0 12px ${cfg.glow}` : undefined,
            animationDelay: `${delay}s`,
          }}
        />
      ))}

      {/* ---------- 3. Radar sweep (conic gradient rotating inside the ring) ---------- */}
      <div
        className="absolute inset-0 m-auto rounded-full ring-sweep"
        style={{
          width: size - 8,
          height: size - 8,
          background: `conic-gradient(from 0deg, transparent 0deg, ${cfg.glow} 30deg, transparent 60deg)`,
          maskImage: "radial-gradient(circle, transparent 35%, black 36%, black 95%, transparent 96%)",
          WebkitMaskImage: "radial-gradient(circle, transparent 35%, black 36%, black 95%, transparent 96%)",
          opacity: 0.55,
          pointerEvents: "none",
        }}
      />

      {/* ---------- 4-5-9. SVG layer: ticks, quality arc, crosshair ---------- */}
      <svg width={size} height={size} className="absolute inset-0 -rotate-90">
        <defs>
          <linearGradient id={`arcGrad-${state}`} x1="0%" y1="0%" x2="100%" y2="100%">
            <stop offset="0%" stopColor={cfg.arcGrad[0]} />
            <stop offset="100%" stopColor={cfg.arcGrad[1]} />
          </linearGradient>
        </defs>

        {/* Faint track */}
        <circle
          cx={cx}
          cy={cy}
          r={rOuter}
          fill="none"
          stroke="rgba(255,255,255,0.06)"
          strokeWidth="3"
        />

        {/* Tick marks every 10°, accent every 90° */}
        {Array.from({ length: 36 }).map((_, i) => {
          const a = (i / 36) * Math.PI * 2;
          const major = i % 9 === 0;
          const inner = rOuter - (major ? 10 : 5);
          const outer = rOuter - 1;
          const x1 = cx + Math.cos(a) * inner;
          const y1 = cy + Math.sin(a) * inner;
          const x2 = cx + Math.cos(a) * outer;
          const y2 = cy + Math.sin(a) * outer;
          return (
            <line
              key={i}
              x1={x1}
              y1={y1}
              x2={x2}
              y2={y2}
              stroke={major ? cfg.color : "rgba(255,255,255,0.18)"}
              strokeOpacity={major ? 0.7 : 1}
              strokeWidth={major ? 1.6 : 0.8}
            />
          );
        })}

        {/* Quality arc with gradient + drop-shadow glow */}
        <circle
          cx={cx}
          cy={cy}
          r={rOuter}
          fill="none"
          stroke={`url(#arcGrad-${state})`}
          strokeWidth="3.5"
          strokeLinecap="round"
          strokeDasharray={`${arcLen} ${circumference}`}
          style={{
            filter: `drop-shadow(0 0 12px ${cfg.glow})`,
            transition: "stroke-dasharray 800ms cubic-bezier(0.4, 0, 0.2, 1)",
          }}
        />

        {/* Compass crosshair markers (N/E/S/W) */}
        {[0, 0.25, 0.5, 0.75].map((p) => {
          const a = p * Math.PI * 2;
          const x = cx + Math.cos(a) * (rOuter + 16);
          const y = cy + Math.sin(a) * (rOuter + 16);
          return (
            <g key={p} opacity={0.55}>
              <circle cx={x} cy={y} r={1.6} fill={cfg.color} />
            </g>
          );
        })}

        {/* Live arc-head — small glowing puck at the tip of the quality arc.
            Animates smoothly as the value changes. */}
        {(() => {
          const a = (qualityPct / 100) * Math.PI * 2;
          const hx = cx + Math.cos(a) * rOuter;
          const hy = cy + Math.sin(a) * rOuter;
          return (
            <g
              style={{
                transition: "transform 800ms cubic-bezier(0.4, 0, 0.2, 1)",
              }}
            >
              <circle
                cx={hx}
                cy={hy}
                r={6}
                fill={cfg.color}
                style={{ filter: `drop-shadow(0 0 10px ${cfg.glow})` }}
              />
              <circle cx={hx} cy={hy} r={2.5} fill="white" opacity={0.9} />
            </g>
          );
        })()}
      </svg>

      {/* ---------- 6. Inner counter-rotating chevron ring ---------- */}
      <div
        className="absolute inset-0 m-auto rounded-full ring-spin-reverse"
        style={{
          width: size - 80,
          height: size - 80,
          opacity: 0.55,
        }}
      >
        <svg width={size - 80} height={size - 80} className="block">
          {Array.from({ length: 24 }).map((_, i) => {
            const a = (i / 24) * Math.PI * 2;
            const r = (size - 80) / 2 - 1;
            const c = (size - 80) / 2;
            const inner = r - 4;
            const outer = r;
            const x1 = c + Math.cos(a) * inner;
            const y1 = c + Math.sin(a) * inner;
            const x2 = c + Math.cos(a) * outer;
            const y2 = c + Math.sin(a) * outer;
            return (
              <line
                key={i}
                x1={x1}
                y1={y1}
                x2={x2}
                y2={y2}
                stroke={cfg.color}
                strokeWidth={i % 6 === 0 ? 1.4 : 0.7}
                opacity={i % 6 === 0 ? 0.85 : 0.4}
              />
            );
          })}
        </svg>
      </div>

      {/* ---------- 7. Orbital particles (three radii, different periods) ---------- */}
      {particleRadii.map((r, i) => (
        <div
          key={i}
          className={cn(
            "absolute inset-0 m-auto",
            i === 0 ? "ring-orbit-a" : i === 1 ? "ring-orbit-b" : "ring-orbit-c",
          )}
          style={{
            width: r * 2,
            height: r * 2,
            pointerEvents: "none",
          }}
        >
          <div
            className="absolute rounded-full"
            style={{
              width: i === 0 ? 6 : 4,
              height: i === 0 ? 6 : 4,
              top: -3,
              left: r - (i === 0 ? 3 : 2),
              background: cfg.color,
              boxShadow: `0 0 10px ${cfg.glow}, 0 0 4px ${cfg.color}`,
              opacity: i === 2 ? 0.7 : 1,
            }}
          />
        </div>
      ))}

      {/* ---------- 8. Center content ---------- */}
      <div className="relative z-10 flex flex-col items-center justify-center text-center">
        {/* Backing glow disk */}
        <div
          className="absolute rounded-full"
          style={{
            width: size * 0.55,
            height: size * 0.55,
            background: `radial-gradient(circle, ${cfg.glowSoft} 0%, transparent 70%)`,
            pointerEvents: "none",
          }}
        />
        <div
          className={cn("text-[10px] font-medium tracking-[0.32em] mb-2 breathe relative z-10")}
          style={{ color: cfg.color, textShadow: `0 0 8px ${cfg.glow}` }}
        >
          {cfg.label}
        </div>
        <div
          className="font-display font-bold tabular relative z-10"
          style={{
            fontSize: size * 0.18,
            color: cfg.color,
            textShadow: `0 0 28px ${cfg.glow}, 0 0 8px ${cfg.glow}`,
            lineHeight: 1,
          }}
        >
          {qualityPct.toFixed(1)}
          <span className="text-[0.45em] text-[color:var(--color-text-secondary)] ml-1">%</span>
        </div>
        <div className="mt-2 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] relative z-10">
          Link Quality
        </div>
      </div>
    </div>
  );
}

/**
 * Memoize the gauge so the Hero panel doesn't rebuild ~100 SVG elements
 * on every 10 Hz telemetry frame. We compare on:
 *   - state changes (online/degraded/offline → palette)
 *   - quality rounded to 1 decimal (the displayed precision)
 *   - size
 * Without this the dashboard kept the React reconciler busy enough to
 * make route navigation in dev mode visibly stall.
 */
export const PulseRing = memo(PulseRingImpl, (a, b) => {
  return (
    a.state === b.state &&
    a.size === b.size &&
    Math.round(a.qualityPct * 10) === Math.round(b.qualityPct * 10)
  );
});
