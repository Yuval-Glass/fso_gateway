"use client";

import { useMemo } from "react";
import type { LinkState } from "@/types/telemetry";

/**
 * Animated optical beam between the two endpoints.
 *
 * Approach:
 *  - SVG for the static beam geometry (strong core line + halo + end flares).
 *  - CSS-animated divs for the moving packet particles — two tracks (TX left→right,
 *    RX right→left). Particle density and speed are derived from telemetry pps.
 *  - Particles are pre-generated (fixed count with staggered delays) rather than
 *    dynamically spawned — cheap, smooth, and respects `prefers-reduced-motion`.
 */
interface FsoBeamProps {
  state: LinkState;
  qualityPct: number;
  /** 0–1, scales particle cadence for both directions. */
  txIntensity: number;
  rxIntensity: number;
}

const TRACK_COUNT = 14; // particles per direction

export function FsoBeam({ state, qualityPct, txIntensity, rxIntensity }: FsoBeamProps) {
  const accent =
    state === "online"
      ? "var(--color-cyan-500)"
      : state === "degraded"
      ? "var(--color-warning)"
      : "var(--color-danger)";
  const accentGlow =
    state === "online"
      ? "rgba(0, 212, 255, 0.55)"
      : state === "degraded"
      ? "rgba(255, 176, 32, 0.55)"
      : "rgba(255, 45, 92, 0.55)";

  // Base traversal time in seconds: faster when traffic is higher.
  const txDur = 1.1 + (1.0 - clamp01(txIntensity)) * 2.4;
  const rxDur = 1.1 + (1.0 - clamp01(rxIntensity)) * 2.4;

  const txParticles = useMemo(() => buildParticles(TRACK_COUNT, txDur, "tx"), [txDur]);
  const rxParticles = useMemo(() => buildParticles(TRACK_COUNT, rxDur, "rx"), [rxDur]);

  return (
    <div className="fso-beam-container relative h-24 flex items-center px-4 select-none overflow-hidden">
      {/* Halo */}
      <div
        className="absolute inset-x-0 top-1/2 -translate-y-1/2 h-12 rounded-full pointer-events-none"
        style={{
          background: `radial-gradient(ellipse at center, ${accentGlow}, transparent 70%)`,
          filter: "blur(14px)",
          opacity: state === "offline" ? 0.25 : 0.8,
        }}
      />

      {/* Core line */}
      <svg
        width="100%"
        height="24"
        viewBox="0 0 1000 24"
        preserveAspectRatio="none"
        className="relative z-[1]"
      >
        <defs>
          <linearGradient id="fso-beam-grad" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor={accent} stopOpacity="0.2" />
            <stop offset="50%" stopColor={accent} stopOpacity="1" />
            <stop offset="100%" stopColor={accent} stopOpacity="0.2" />
          </linearGradient>
          <linearGradient id="fso-beam-halo" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor={accent} stopOpacity="0" />
            <stop offset="50%" stopColor={accent} stopOpacity="0.5" />
            <stop offset="100%" stopColor={accent} stopOpacity="0" />
          </linearGradient>
        </defs>
        <line
          x1="0" y1="12" x2="1000" y2="12"
          stroke="url(#fso-beam-halo)" strokeWidth="6" opacity={state === "offline" ? 0.3 : 1}
        />
        <line
          x1="0" y1="12" x2="1000" y2="12"
          stroke="url(#fso-beam-grad)" strokeWidth="1.5"
          style={{ filter: `drop-shadow(0 0 4px ${accentGlow})` }}
          opacity={state === "offline" ? 0.4 : 1}
        />
        {/* Tick marks every 10% */}
        {Array.from({ length: 11 }).map((_, i) => (
          <line
            key={i}
            x1={i * 100}
            y1={8}
            x2={i * 100}
            y2={16}
            stroke={accent}
            strokeWidth="0.6"
            opacity="0.4"
          />
        ))}
      </svg>

      {/* TX particles (left → right) */}
      {state !== "offline" && txParticles.map((p, i) => (
        <Particle key={`tx-${i}`} direction="ltr" color={accent} glow={accentGlow} {...p} />
      ))}
      {/* RX particles (right → left) */}
      {state !== "offline" && rxParticles.map((p, i) => (
        <Particle key={`rx-${i}`} direction="rtl" color={accent} glow={accentGlow} {...p} />
      ))}

      {/* Direction labels */}
      <DirLabel side="left"  label="TX →" intensity={txIntensity} color={accent} />
      <DirLabel side="right" label="← RX" intensity={rxIntensity} color={accent} />

      {state === "offline" && (
        <div className="absolute inset-0 flex items-center justify-center pointer-events-none">
          <div
            className="px-3 py-1 rounded-md border text-[10px] tracking-[0.28em] uppercase font-semibold"
            style={{
              borderColor: "var(--color-danger)",
              background: "rgba(255, 45, 92, 0.1)",
              color: "var(--color-danger)",
              boxShadow: "0 0 18px rgba(255, 45, 92, 0.4)",
            }}
          >
            Link Lost
          </div>
        </div>
      )}

      {state === "degraded" && (
        <div className="absolute top-0 left-1/2 -translate-x-1/2 -translate-y-1/2 pointer-events-none">
          <span
            className="text-[9px] tracking-[0.28em] uppercase font-semibold px-2 py-0.5 rounded-sm"
            style={{
              color: "var(--color-warning)",
              background: "rgba(255, 176, 32, 0.1)",
              border: "1px solid rgba(255, 176, 32, 0.4)",
            }}
          >
            {qualityPct.toFixed(1)}% · Degraded
          </span>
        </div>
      )}
    </div>
  );
}

interface ParticleCfg {
  offsetSec: number;
  durSec: number;
  opacity: number;
  size: number;
}

function buildParticles(count: number, dur: number, salt: string): ParticleCfg[] {
  return Array.from({ length: count }).map((_, i) => {
    // Deterministic variation for steady look (no random each render).
    const phase = ((i + hash(salt) % count) % count) / count;
    return {
      offsetSec: -phase * dur,
      durSec: dur,
      opacity: 0.55 + (i % 3) * 0.15,
      size: 3 + (i % 3),
    };
  });
}

function hash(s: string): number {
  let h = 0;
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) >>> 0;
  return h;
}

function Particle({
  direction,
  color,
  glow,
  offsetSec,
  durSec,
  opacity,
  size,
}: ParticleCfg & {
  direction: "ltr" | "rtl";
  color: string;
  glow: string;
}) {
  return (
    <div
      className="absolute top-1/2 -translate-y-1/2 rounded-full pointer-events-none"
      style={{
        width: size,
        height: size,
        background: color,
        boxShadow: `0 0 8px ${glow}, 0 0 14px ${glow}`,
        opacity,
        left: 0,
        animationName: direction === "ltr" ? "fso-flow-ltr" : "fso-flow-rtl",
        animationDuration: `${durSec}s`,
        animationTimingFunction: "linear",
        animationIterationCount: "infinite",
        animationDelay: `${offsetSec}s`,
        willChange: "transform",
      }}
    />
  );
}

function DirLabel({
  side,
  label,
  intensity,
  color,
}: {
  side: "left" | "right";
  label: string;
  intensity: number;
  color: string;
}) {
  const dim = clamp01(intensity);
  return (
    <div
      className={`absolute ${side === "left" ? "left-2" : "right-2"} top-1`}
      style={{ color, opacity: 0.35 + dim * 0.55 }}
    >
      <span className="text-[9px] tracking-[0.25em] uppercase font-semibold">{label}</span>
    </div>
  );
}

function clamp01(v: number): number {
  if (!Number.isFinite(v)) return 0;
  if (v < 0) return 0;
  if (v > 1) return 1;
  return v;
}
