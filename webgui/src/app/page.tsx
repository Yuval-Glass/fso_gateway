"use client";

import { useTelemetry } from "@/lib/useTelemetry";
import { LinkStatusHero } from "@/components/dashboard/LinkStatusHero";
import { ThroughputCards } from "@/components/dashboard/ThroughputCards";
import { ErrorMetrics } from "@/components/dashboard/ErrorMetrics";
import { LiveCharts } from "@/components/dashboard/LiveCharts";
import { PipelineFlow } from "@/components/dashboard/PipelineFlow";
import { AlertsFeed } from "@/components/dashboard/AlertsFeed";
import { SystemTelemetryLog } from "@/components/dashboard/SystemTelemetryLog";

export default function DashboardPage() {
  const { snapshot: snap, connection } = useTelemetry();

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Initializing telemetry stream…
        </div>
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-5">
      {/* Page header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Live Telemetry
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Dashboard
          </h2>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          {connection === "connecting"
            ? "Phase 2B · Connecting"
            : connection === "demo"
            ? "Phase 2B · Demo Mode"
            : snap.source === "gateway"
            ? "Phase 2B · Live from Gateway"
            : "Phase 2B · Live via Bridge (mock)"}
        </div>
      </div>

      {/* Hero — link status */}
      <LinkStatusHero link={snap.link} />

      {/* Throughput cards */}
      <ThroughputCards snap={snap} />

      {/* Error metrics */}
      <ErrorMetrics errors={snap.errors} />

      {/* Pipeline flow */}
      <PipelineFlow stages={snap.pipeline} />

      {/* Charts grid */}
      <LiveCharts snap={snap} />

      {/* Bottom: Log + Alerts */}
      <div className="grid grid-cols-1 lg:grid-cols-[2fr_1fr] gap-4">
        <SystemTelemetryLog />
        <AlertsFeed alerts={snap.alerts} />
      </div>
    </div>
  );
}
