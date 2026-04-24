"use client";

import type { LinkState } from "@/types/telemetry";

/**
 * Radial quality gauge that sits between the two endpoints, above the beam.
 * Shows % quality, RSSI/SNR/BER, and latency. Pure SVG + text, no animation
 * loops — the numbers come from useTelemetry and React updates do the rest.
 */
interface LinkQualityOverlayProps {
  state: LinkState;
  qualityPct: number;
  rssiDbm: number | null;
  snrDb: number | null;
  berEstimate: number | null;
  latencyMsAvg: number;
}

export function LinkQualityOverlay({
  state,
  qualityPct,
  rssiDbm,
  snrDb,
  berEstimate,
  latencyMsAvg,
}: LinkQualityOverlayProps) {
  const accent =
    state === "online"
      ? "#00d4ff"
      : state === "degraded"
      ? "#ffb020"
      : "#ff2d5c";
  const glow =
    state === "online"
      ? "rgba(0, 212, 255, 0.55)"
      : state === "degraded"
      ? "rgba(255, 176, 32, 0.55)"
      : "rgba(255, 45, 92, 0.55)";

  const size = 128;
  const r = size / 2 - 8;
  const cx = size / 2;
  const cy = size / 2;
  const circ = 2 * Math.PI * r;
  const arcLen = Math.min(100, Math.max(0, qualityPct)) / 100 * circ;

  return (
    <div className="flex flex-col items-center gap-2">
      <div className="relative" style={{ width: size, height: size }}>
        <svg width={size} height={size} className="-rotate-90">
          <circle cx={cx} cy={cy} r={r} fill="none" stroke="rgba(255,255,255,0.05)" strokeWidth="3" />
          <circle
            cx={cx}
            cy={cy}
            r={r}
            fill="none"
            stroke={accent}
            strokeWidth="3"
            strokeLinecap="round"
            strokeDasharray={`${arcLen} ${circ}`}
            style={{
              filter: `drop-shadow(0 0 10px ${glow})`,
              transition: "stroke-dasharray 600ms cubic-bezier(0.4,0,0.2,1)",
            }}
          />
        </svg>
        <div className="absolute inset-0 flex flex-col items-center justify-center">
          <div
            className="font-display text-2xl font-bold tabular"
            style={{ color: accent, textShadow: `0 0 14px ${glow}` }}
          >
            {qualityPct.toFixed(1)}
            <span className="text-[0.55em] opacity-80 ml-0.5">%</span>
          </div>
          <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mt-0.5">
            Link Quality
          </div>
        </div>
      </div>

      <div className="grid grid-cols-4 gap-2 w-full max-w-[420px]">
        <StatTile label="RSSI" value={rssiDbm != null ? `${rssiDbm.toFixed(1)} dBm` : "N/A"} accent={accent} />
        <StatTile label="SNR" value={snrDb != null ? `${snrDb.toFixed(1)} dB` : "N/A"} accent={accent} />
        <StatTile label="BER" value={berEstimate != null ? berEstimate.toExponential(1) : "N/A"} accent={accent} />
        <StatTile label="Latency" value={`${latencyMsAvg.toFixed(2)} ms`} accent={accent} />
      </div>
    </div>
  );
}

function StatTile({ label, value, accent }: { label: string; value: string; accent: string }) {
  return (
    <div className="glass rounded-md px-2 py-1.5 text-center">
      <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div
        className="font-mono text-[11px] tabular mt-0.5 truncate"
        style={{ color: accent }}
      >
        {value}
      </div>
    </div>
  );
}
