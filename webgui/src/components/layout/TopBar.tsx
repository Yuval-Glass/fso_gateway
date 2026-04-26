"use client";

import { Bell, Maximize2, Minimize2 } from "lucide-react";
import Link from "next/link";
import { useTelemetry } from "@/lib/useTelemetry";
import { useDaemon, type DaemonState } from "@/lib/useDaemon";
import { useAlerts } from "@/lib/useAlerts";
import { formatUptime } from "@/lib/utils";
import { StatusDot } from "../primitives/StatusDot";
import { ConnectionPill } from "../primitives/ConnectionPill";
import { FieldHint } from "../primitives/FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
import { useCallback, useEffect, useState } from "react";

function useClock() {
  const [now, setNow] = useState<Date | null>(null);
  useEffect(() => {
    setNow(new Date());
    const id = setInterval(() => setNow(new Date()), 1000);
    return () => clearInterval(id);
  }, []);
  return now;
}

function useFullscreen() {
  const [isFs, setIsFs] = useState(false);
  useEffect(() => {
    const onChange = () => setIsFs(!!document.fullscreenElement);
    document.addEventListener("fullscreenchange", onChange);
    return () => document.removeEventListener("fullscreenchange", onChange);
  }, []);
  const toggle = useCallback(() => {
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen().catch(() => {});
  }, []);
  return { isFs, toggle };
}

export function TopBar() {
  const { snapshot: telemetry, connection } = useTelemetry();
  const { status: daemon } = useDaemon();
  const alerts = useAlerts(telemetry);
  const { isFs, toggle: toggleFs } = useFullscreen();
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
          <span className="inline-flex items-center gap-1">
            <ConnectionPill status={connection} source={telemetry?.source} />
            <FieldHint id="topbar.connection" size={11} />
          </span>
          <Divider />
          <Stat label="System" hintId="topbar.system">
            <StatusDot status={systemStatus} />
            <span className="font-medium tracking-[0.15em] text-[color:var(--color-text-primary)]">
              {statusLabel}
            </span>
          </Stat>
          <Divider />
          <Stat label="Daemon" hintId="topbar.daemon">
            <DaemonPill state={daemon?.state ?? null} />
          </Stat>
          <Divider />
          <Stat label="Uptime" hintId="topbar.uptime">
            <span className="font-mono tabular text-[color:var(--color-text-primary)]">
              {telemetry ? formatUptime(telemetry.link.uptimeSec) : "—"}
            </span>
          </Stat>
          <Divider />
          <Stat label="Time (IL)" hintId="topbar.time">
            <span className="font-mono tabular text-[color:var(--color-text-primary)]">
              {now
                ? now.toLocaleTimeString("en-GB", {
                    timeZone: "Asia/Jerusalem",
                    hour12: false,
                    hour: "2-digit",
                    minute: "2-digit",
                    second: "2-digit",
                  })
                : "—"}
            </span>
          </Stat>
        </div>

        {/* Actions */}
        <div className="flex items-center gap-1 ml-2">
          <IconButton
            aria={isFs ? "Exit fullscreen" : "Enter fullscreen"}
            title={isFs ? "Exit fullscreen" : "Enter fullscreen"}
            onClick={toggleFs}
          >
            {isFs ? <Minimize2 size={16} strokeWidth={1.8} /> : <Maximize2 size={16} strokeWidth={1.8} />}
          </IconButton>
          <Link
            href="/alerts"
            aria-label={alerts.counts.active > 0 ? `${alerts.counts.active} active alerts` : "Alerts"}
            title={alerts.counts.active > 0 ? `${alerts.counts.active} active alert${alerts.counts.active === 1 ? "" : "s"}` : "Alerts"}
            className="relative w-9 h-9 rounded-md flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04] transition-colors"
          >
            <Bell size={16} strokeWidth={1.8} />
            {alerts.counts.active > 0 && (
              <span
                className="absolute top-1.5 right-1.5 min-w-[14px] h-[14px] px-1 rounded-full bg-[color:var(--color-danger)] text-[9px] font-bold text-white flex items-center justify-center leading-none"
                style={{ boxShadow: "0 0 6px rgba(255, 45, 92, 0.9)" }}
              >
                {alerts.counts.active > 99 ? "99+" : alerts.counts.active}
              </span>
            )}
          </Link>
        </div>
      </div>
    </header>
  );
}

function DaemonPill({ state }: { state: DaemonState | null }) {
  const { color, glow, label, breathe } =
    state === "running" ? { color: "var(--color-success)", glow: "rgba(52,211,153,0.6)", label: "RUNNING", breathe: false } :
    state === "starting" ? { color: "var(--color-warning)", glow: "rgba(255,176,32,0.65)", label: "STARTING", breathe: true } :
    state === "stopping" ? { color: "var(--color-warning)", glow: "rgba(255,176,32,0.65)", label: "STOPPING", breathe: true } :
    state === "failed" ? { color: "var(--color-danger)", glow: "rgba(255,45,92,0.7)", label: "FAILED", breathe: false } :
    state === "stopped" ? { color: "var(--color-text-muted)", glow: "rgba(85,96,114,0.4)", label: "STOPPED", breathe: false } :
    { color: "var(--color-text-muted)", glow: "rgba(85,96,114,0.3)", label: "—", breathe: false };
  return (
    <span className="inline-flex items-center gap-2">
      <span
        className={breathe ? "w-1.5 h-1.5 rounded-full breathe" : "w-1.5 h-1.5 rounded-full"}
        style={{ background: color, boxShadow: `0 0 6px ${glow}` }}
      />
      <span className="font-medium tracking-[0.15em] text-[color:var(--color-text-primary)]">
        {label}
      </span>
    </span>
  );
}

function Stat({
  label,
  children,
  hintId,
}: {
  label: string;
  children: React.ReactNode;
  hintId?: FieldHintId;
}) {
  return (
    <div className="flex flex-col leading-tight">
      <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mb-0.5 inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
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
  title,
  onClick,
}: {
  children: React.ReactNode;
  aria: string;
  title?: string;
  onClick?: () => void;
}) {
  return (
    <button
      type="button"
      aria-label={aria}
      title={title ?? aria}
      onClick={onClick}
      className="relative w-9 h-9 rounded-md flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04] transition-colors"
    >
      {children}
    </button>
  );
}
