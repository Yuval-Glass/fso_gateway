"use client";

import { AlertTriangle, Network, Radio } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { ClientBranch } from "@/components/topology/ClientBranch";
import { EndpointBadge } from "@/components/topology/EndpointBadge";
import { FsoBeam } from "@/components/topology/FsoBeam";
import { useTelemetry } from "@/lib/useTelemetry";
import { formatNumber } from "@/lib/utils";
import type { ArpEntry } from "@/types/telemetry";

/**
 * Phase 8 two-machine FSO setup (per README):
 *
 *   Win1 ──── GW-A ══════ GW-B ──── Win2
 *   192.168.50.1   FSO cable   192.168.50.2
 *
 * The FSO link is currently a direct Ethernet cable simulating optics —
 * there is no optical transceiver, no RSSI/SNR. The two gateways are the
 * machines you actually run `fso_gw_runner` on; the two Windows boxes are
 * the bench endpoints that send traffic.
 */

const WIN1 = { name: "Win-1", ip: "192.168.50.1", mac: "90:2e:16:d6:96:ba" };
const WIN2 = { name: "Win-2", ip: "192.168.50.2", mac: "c4:ef:bb:5f:cd:5c" };
const GW_A_MAC = "08:c0:eb:62:34:98";
const GW_B_MAC = "08:c0:eb:62:34:50";

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
  const cfg = snap.configEcho;
  const latest = snap.throughput[snap.throughput.length - 1];
  const txIntensity = Math.min(1, (latest?.txPps ?? 0) / 5000);
  const rxIntensity = Math.min(1, (latest?.rxPps ?? 0) / 5000);
  const offline = link.state === "offline";
  const degraded = link.state === "degraded";

  const lan = cfg.lanIface || "enp1s0f0np0";
  const fso = cfg.fsoIface || "enp1s0f1np1";

  return (
    <div className="flex flex-col gap-5">
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Topology Map
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Phase 8 deployment: two gateway boxes (GW-A / GW-B) bridging two
            Windows endpoints over a simulated FSO cable. Particle density
            scales with packet rate.
          </div>
        </div>
      </div>

      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="px-6 py-8">
          <div className="grid grid-cols-[auto_1fr_auto] items-center gap-6">
            <EndpointBadge
              side="A"
              title="GW-A"
              subtitle="fso_gw_runner"
              mac={GW_A_MAC}
              lan={lan}
              fso={fso}
              online={!offline}
              degraded={degraded}
            />

            <div className="flex flex-col gap-3 min-w-0">
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
                  <span>FSO Cable · simulating Free-Space Optics</span>
                </span>
                <span className="font-mono tabular">
                  {formatNumber(Math.round(latest?.rxPps ?? 0))} pps RX
                </span>
              </div>
            </div>

            <EndpointBadge
              side="B"
              title="GW-B"
              subtitle="fso_gw_runner"
              mac={GW_B_MAC}
              lan={lan}
              fso={fso}
              online={!offline}
              degraded={degraded}
            />
          </div>

          <div className="mt-4 grid grid-cols-[auto_1fr_auto] gap-6">
            <ClientBranch side="A" clients={[{ name: WIN1.name, ip: WIN1.ip }]} dim={offline} />
            <div />
            <ClientBranch side="B" clients={[{ name: WIN2.name, ip: WIN2.ip }]} dim={offline} />
          </div>

          <div className="mt-3 grid grid-cols-2 gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            <div className="text-center">
              <span className="font-mono normal-case tracking-normal">{WIN1.mac}</span>
            </div>
            <div className="text-center">
              <span className="font-mono normal-case tracking-normal">{WIN2.mac}</span>
            </div>
          </div>
        </div>
      </GlassPanel>

      <ArpCachePanel arp={snap.arpEntries ?? []} />

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
                a.severity === "critical" ? "var(--color-danger)"
                : a.severity === "warning" ? "var(--color-warning)"
                : "var(--color-cyan-500)";
              const t = new Date(a.t).toLocaleTimeString([], {
                hour: "2-digit", minute: "2-digit", second: "2-digit",
              });
              return (
                <li key={a.id} className="flex items-start gap-3 text-xs">
                  <AlertTriangle size={12} style={{ color, flexShrink: 0 }} className="mt-0.5" />
                  <span className="font-mono tabular text-[color:var(--color-text-muted)] shrink-0">{t}</span>
                  <span className="font-semibold tracking-[0.1em] uppercase text-[10px] shrink-0" style={{ color }}>
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

function ArpCachePanel({ arp }: { arp: ArpEntry[] }) {
  return (
    <GlassPanel
      label="ARP Cache (proxy-ARP)"
      trailing={
        <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
          {arp.length} {arp.length === 1 ? "entry" : "entries"} · learned by RX pipeline
        </span>
      }
    >
      {arp.length === 0 ? (
        <div className="flex items-center gap-3 py-4 text-[11px] text-[color:var(--color-text-muted)]">
          <Network size={14} />
          No peer MACs learned yet — the RX pipeline populates this table when it
          decodes ARP traffic from the far side.
        </div>
      ) : (
        <ul className="grid grid-cols-1 md:grid-cols-2 gap-2 py-1">
          {arp.map((e) => {
            const ageMs = Math.max(0, Date.now() - e.lastSeenMs);
            const ageStr =
              ageMs < 60_000 ? `${Math.floor(ageMs / 1000)}s ago`
              : ageMs < 3_600_000 ? `${Math.floor(ageMs / 60_000)}m ago`
              : `${Math.floor(ageMs / 3_600_000)}h ago`;
            return (
              <li
                key={e.ip + e.mac}
                className="glass rounded-md px-3 py-2 flex items-center justify-between gap-3"
              >
                <div className="min-w-0">
                  <div className="font-mono text-[12px] tabular text-[color:var(--color-text-primary)]">
                    {e.ip}
                  </div>
                  <div className="font-mono text-[10px] text-[color:var(--color-text-muted)]">
                    {e.mac}
                  </div>
                </div>
                <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
                  {ageStr}
                </span>
              </li>
            );
          })}
        </ul>
      )}
      <div className="mt-2 text-[10px] text-[color:var(--color-text-muted)] leading-snug">
        The TX pipeline answers ARP requests locally for any IP found in this
        table — see <span className="font-mono">try_proxy_arp()</span> in
        <span className="font-mono"> src/tx_pipeline.c</span>. Entries expire
        after 5 minutes of inactivity.
      </div>
    </GlassPanel>
  );
}
