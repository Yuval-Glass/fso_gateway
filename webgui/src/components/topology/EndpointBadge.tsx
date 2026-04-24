"use client";

import { Cpu, Server, Wifi, WifiOff } from "lucide-react";
import { cn } from "@/lib/utils";

interface EndpointBadgeProps {
  side: "A" | "B";
  title: string;
  subtitle?: string;
  mac: string;
  lan: string;
  fso: string;
  online: boolean;
  degraded?: boolean;
}

export function EndpointBadge({
  side,
  title,
  subtitle,
  mac,
  lan,
  fso,
  online,
  degraded,
}: EndpointBadgeProps) {
  const state = !online ? "offline" : degraded ? "degraded" : "online";
  const accent =
    state === "online"
      ? "var(--color-cyan-500)"
      : state === "degraded"
      ? "var(--color-warning)"
      : "var(--color-danger)";
  const accentGlow =
    state === "online"
      ? "rgba(0, 212, 255, 0.45)"
      : state === "degraded"
      ? "rgba(255, 176, 32, 0.45)"
      : "rgba(255, 45, 92, 0.5)";

  return (
    <div
      className={cn(
        "relative glass-raised rounded-lg p-5 min-w-[260px]",
        side === "B" ? "text-right" : "",
      )}
      style={{
        boxShadow: `0 0 40px ${accentGlow}, inset 0 0 20px ${accentGlow.replace(/0\.\d+\)$/, "0.08)")}`,
        borderColor: accent + "55",
      }}
    >
      {/* Side tag */}
      <div
        className={cn(
          "absolute top-3 px-2 py-0.5 rounded-sm text-[9px] font-bold tracking-[0.3em]",
          side === "A" ? "left-3" : "right-3",
        )}
        style={{
          background: accent + "22",
          color: accent,
          border: `1px solid ${accent}55`,
        }}
      >
        NODE {side}
      </div>

      {/* Device icon */}
      <div className={cn("flex items-center gap-3 mt-5 mb-4", side === "B" && "flex-row-reverse")}>
        <div
          className="w-14 h-14 rounded-lg border flex items-center justify-center shrink-0"
          style={{
            borderColor: accent + "55",
            background: `linear-gradient(135deg, ${accent}18, transparent)`,
            boxShadow: `inset 0 0 16px ${accentGlow}`,
          }}
        >
          <Server
            size={28}
            strokeWidth={1.4}
            style={{ color: accent, filter: `drop-shadow(0 0 6px ${accentGlow})` }}
          />
        </div>
        <div className={cn("min-w-0 flex-1", side === "B" && "text-right")}>
          <div className="font-display font-semibold text-base text-[color:var(--color-text-primary)] truncate">
            {title}
          </div>
          {subtitle && (
            <div className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] mt-0.5 truncate">
              {subtitle}
            </div>
          )}
          <div className="font-mono text-[10px] text-[color:var(--color-text-secondary)] mt-1 truncate">
            {mac}
          </div>
        </div>
      </div>

      {/* Status pill */}
      <div
        className={cn(
          "inline-flex items-center gap-2 px-2.5 py-1 rounded-md border text-[10px] font-semibold tracking-[0.18em] uppercase",
        )}
        style={{
          borderColor: accent + "55",
          background: accent + "10",
          color: accent,
        }}
      >
        {online ? (
          <Wifi size={10} strokeWidth={2} />
        ) : (
          <WifiOff size={10} strokeWidth={2} />
        )}
        <span>
          {state === "online" ? "Online" : state === "degraded" ? "Degraded" : "Offline"}
        </span>
      </div>

      {/* Interfaces */}
      <div className={cn("mt-4 grid grid-cols-2 gap-3")}>
        <InterfaceRow label="LAN IFACE" value={lan} align={side === "B" ? "right" : "left"} />
        <InterfaceRow label="FSO IFACE" value={fso} align={side === "B" ? "right" : "left"} />
      </div>

      {/* Compute indicator */}
      <div className={cn("mt-3 flex items-center gap-1.5", side === "B" && "justify-end")}>
        <Cpu size={10} className="text-[color:var(--color-text-muted)]" />
        <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          FPGA Accelerated
        </span>
      </div>
    </div>
  );
}

function InterfaceRow({
  label,
  value,
  align,
}: {
  label: string;
  value: string;
  align: "left" | "right";
}) {
  return (
    <div className={align === "right" ? "text-right" : ""}>
      <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div className="font-mono text-xs text-[color:var(--color-text-primary)] mt-0.5 truncate">
        {value}
      </div>
    </div>
  );
}
