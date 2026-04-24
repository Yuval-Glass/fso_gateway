"use client";

import { Bell, Search, Maximize2 } from "lucide-react";
import { useTelemetry } from "@/lib/useTelemetry";
import { formatUptime } from "@/lib/utils";
import { StatusDot } from "../primitives/StatusDot";
import { ConnectionPill } from "../primitives/ConnectionPill";
import { useEffect, useState } from "react";

function useClock() {
  const [now, setNow] = useState<Date | null>(null);
  useEffect(() => {
    setNow(new Date());
    const id = setInterval(() => setNow(new Date()), 1000);
    return () => clearInterval(id);
  }, []);
  return now;
}

export function TopBar() {
  const { snapshot: telemetry, connection } = useTelemetry();
  const now = useClock();

  const systemStatus =
    telemetry?.link.state === "online" ? "active" :
    telemetry?.link.state === "degraded" ? "warning" : "error";
  const statusLabel =
    telemetry?.link.state === "online" ? "OPERATIONAL" :
    telemetry?.link.state === "degraded" ? "DEGRADED" : "OFFLINE";

  return (
    <header className="sticky top-0 z-10 glass-raised rounded-none border-t-0 border-x-0 border-[color:var(--color-border-subtle)]">
      <div className="flex items-center h-16 px-6 gap-6">
        {/* Title block */}
        <div className="flex items-baseline gap-3">
          <h1 className="font-display text-lg font-bold tracking-[0.15em] uppercase text-[color:var(--color-text-primary)]">
            FSO Gateway
          </h1>
          <span className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Control Center
          </span>
          <span className="text-[10px] tracking-[0.25em] text-[color:var(--color-text-muted)]">
            v3.1 · BUILD a202b70
          </span>
        </div>

        <div className="flex-1" />

        {/* Status panel */}
        <div className="flex items-center gap-5 text-xs">
          <ConnectionPill status={connection} source={telemetry?.source} />
          <Divider />
          <Stat label="System">
            <StatusDot status={systemStatus} />
            <span className="font-medium tracking-[0.15em] text-[color:var(--color-text-primary)]">
              {statusLabel}
            </span>
          </Stat>
          <Divider />
          <Stat label="Uptime">
            <span className="font-mono tabular text-[color:var(--color-text-primary)]">
              {telemetry ? formatUptime(telemetry.link.uptimeSec) : "—"}
            </span>
          </Stat>
          <Divider />
          <Stat label="CPU">
            <span className="font-mono tabular text-[color:var(--color-text-primary)]">
              {telemetry ? telemetry.system.cpuPct.toFixed(0) : "—"}%
            </span>
          </Stat>
          <Divider />
          <Stat label="Time (UTC)">
            <span className="font-mono tabular text-[color:var(--color-text-primary)]">
              {now ? now.toISOString().slice(11, 19) : "—"}
            </span>
          </Stat>
        </div>

        {/* Actions */}
        <div className="flex items-center gap-1 ml-2">
          <IconButton aria="Search"><Search size={16} strokeWidth={1.8} /></IconButton>
          <IconButton aria="Fullscreen"><Maximize2 size={16} strokeWidth={1.8} /></IconButton>
          <IconButton aria="Notifications" hasBadge>
            <Bell size={16} strokeWidth={1.8} />
          </IconButton>
          <div className="w-9 h-9 ml-2 rounded-md border border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40 flex items-center justify-center text-[11px] font-semibold tracking-[0.1em] text-[color:var(--color-cyan-300)]">
            OP
          </div>
        </div>
      </div>
    </header>
  );
}

function Stat({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col leading-tight">
      <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mb-0.5">
        {label}
      </span>
      <div className="flex items-center gap-2">{children}</div>
    </div>
  );
}

function Divider() {
  return <span className="w-px h-7 bg-[color:var(--color-border-hair)]" />;
}

function IconButton({
  children,
  aria,
  hasBadge,
}: {
  children: React.ReactNode;
  aria: string;
  hasBadge?: boolean;
}) {
  return (
    <button
      aria-label={aria}
      className="relative w-9 h-9 rounded-md flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04] transition-colors"
    >
      {children}
      {hasBadge && (
        <span
          className="absolute top-2 right-2 w-1.5 h-1.5 rounded-full bg-[color:var(--color-danger)]"
          style={{ boxShadow: "0 0 6px rgba(255, 45, 92, 0.9)" }}
        />
      )}
    </button>
  );
}
