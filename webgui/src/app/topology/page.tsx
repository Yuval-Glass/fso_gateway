"use client";

import { AlertTriangle, Radio } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { ClientBranch } from "@/components/topology/ClientBranch";
import { EndpointBadge } from "@/components/topology/EndpointBadge";
import { FsoBeam } from "@/components/topology/FsoBeam";
import { LinkQualityOverlay } from "@/components/topology/LinkQualityOverlay";
import { useTelemetry } from "@/lib/useTelemetry";
import { formatNumber } from "@/lib/utils";

const ENDPOINT_A = {
  title: "Machine A",
  subtitle: "T1-Forwarder",
  mac: "90:2e:16:d6:96:ba",
  lan: "enp1s0f0np0",
  fso: "enp1s0f1np1",
};

const ENDPOINT_B = {
  title: "Machine B",
  subtitle: "C4Net-10G5TH",
  mac: "08:c0:eb:62:34:50",
  lan: "enp1s0f0np0",
  fso: "enp1s0f1np1",
};

const CLIENTS_A = [
  { name: "Win-1", ip: "192.168.50.11" },
  { name: "Win-2", ip: "192.168.50.12" },
];

const CLIENTS_B = [
  { name: "Win-3", ip: "192.168.50.21" },
  { name: "Win-4", ip: "192.168.50.22" },
];

export default function TopologyPage() {
  const { snapshot: snap, connection } = useTelemetry();

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          {connection === "demo" ? "Bridge offline — mock topology" : "Connecting…"}
        </div>
      </div>
    );
  }

  const link = snap.link;
  const latest = snap.throughput[snap.throughput.length - 1];
  // Normalize pps (0..58000 in demo) into 0..1 "intensity" for particle speed.
  const txIntensity = Math.min(1, (latest?.txPps ?? 0) / 60_000);
  const rxIntensity = Math.min(1, (latest?.rxPps ?? 0) / 60_000);
  const degraded = link.state === "degraded";
  const offline = link.state === "offline";

  return (
    <div className="flex flex-col gap-5">
      {/* Page header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Topology Map
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Live view of the FSO link and the LAN clients behind each endpoint.
            Particle density and speed scale with observed packet rate.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4B · Animated Topology
        </div>
      </div>

      {/* Main topology panel */}
      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="px-6 py-8">
          {/* Row 1: endpoints + beam */}
          <div className="grid grid-cols-[auto_1fr_auto] items-center gap-6">
            <EndpointBadge
              side="A"
              title={ENDPOINT_A.title}
              subtitle={ENDPOINT_A.subtitle}
              mac={ENDPOINT_A.mac}
              lan={ENDPOINT_A.lan}
              fso={ENDPOINT_A.fso}
              online={!offline}
              degraded={degraded}
            />

            <div className="flex flex-col gap-3 min-w-0">
              <LinkQualityOverlay
                state={link.state}
                qualityPct={link.qualityPct}
                rssiDbm={link.rssiDbm}
                snrDb={link.snrDb}
                berEstimate={link.berEstimate}
                latencyMsAvg={link.latencyMsAvg}
              />
              <FsoBeam
                state={link.state}
                qualityPct={link.qualityPct}
                txIntensity={txIntensity}
                rxIntensity={rxIntensity}
              />
              <div className="flex items-center justify-between text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] px-4">
                <span className="font-mono tabular">
                  TX {formatNumber(Math.round(latest?.txPps ?? 0))} pps
                </span>
                <span className="flex items-center gap-1.5">
                  <Radio size={10} className="text-[color:var(--color-cyan-300)]" />
                  <span>FSO Optical Link</span>
                </span>
                <span className="font-mono tabular">
                  {formatNumber(Math.round(latest?.rxPps ?? 0))} pps RX
                </span>
              </div>
            </div>

            <EndpointBadge
              side="B"
              title={ENDPOINT_B.title}
              subtitle={ENDPOINT_B.subtitle}
              mac={ENDPOINT_B.mac}
              lan={ENDPOINT_B.lan}
              fso={ENDPOINT_B.fso}
              online={!offline}
              degraded={degraded}
            />
          </div>

          {/* Row 2: client branches */}
          <div className="mt-4 grid grid-cols-[auto_1fr_auto] gap-6">
            <ClientBranch side="A" clients={CLIENTS_A} dim={offline} />
            <div /> {/* spacer under the beam */}
            <ClientBranch side="B" clients={CLIENTS_B} dim={offline} />
          </div>
        </div>
      </GlassPanel>

      {/* Event summary strip — critical alerts from the feed */}
      <GlassPanel label="Recent Link Events" trailing={
        <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
          From live stream
        </span>
      }>
        {snap.alerts.length === 0 ? (
          <div className="text-[11px] text-[color:var(--color-text-muted)] py-2">
            No link events reported.
          </div>
        ) : (
          <ul className="space-y-1.5">
            {snap.alerts.slice(0, 6).map((a) => {
              const color =
                a.severity === "critical"
                  ? "var(--color-danger)"
                  : a.severity === "warning"
                  ? "var(--color-warning)"
                  : "var(--color-cyan-500)";
              const t = new Date(a.t).toLocaleTimeString([], {
                hour: "2-digit",
                minute: "2-digit",
                second: "2-digit",
              });
              return (
                <li key={a.id} className="flex items-start gap-3 text-xs">
                  <AlertTriangle
                    size={12}
                    style={{ color, flexShrink: 0 }}
                    className="mt-0.5"
                  />
                  <span className="font-mono tabular text-[color:var(--color-text-muted)] shrink-0">
                    {t}
                  </span>
                  <span
                    className="font-semibold tracking-[0.1em] uppercase text-[10px] shrink-0"
                    style={{ color }}
                  >
                    {a.module}
                  </span>
                  <span className="text-[color:var(--color-text-secondary)]">{a.message}</span>
                </li>
              );
            })}
          </ul>
        )}
      </GlassPanel>
    </div>
  );
}
