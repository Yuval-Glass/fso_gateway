"use client";

import { PulseRing } from "@/components/primitives/PulseRing";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import type { LinkStatus } from "@/types/telemetry";
import { formatNumber, formatPercent } from "@/lib/utils";

export function LinkStatusHero({ link }: { link: LinkStatus }) {
  const rssi = link.rssiDbm != null ? `${link.rssiDbm.toFixed(1)} dBm` : "N/A";
  const snr = link.snrDb != null ? `${link.snrDb.toFixed(1)} dB` : "N/A";
  const ber = link.berEstimate != null ? link.berEstimate.toExponential(1) : "N/A";

  return (
    <GlassPanel variant="raised" padded={false} className="overflow-hidden">
      <div className="relative">
        {/* Scanline overlay */}
        <div className="scanlines absolute inset-0 pointer-events-none" />

        <div className="grid grid-cols-[1fr_auto_1fr] items-center gap-8 px-8 py-7">
          {/* Left: Endpoint A */}
          <EndpointCard
            side="A"
            title="Machine A — T1-Forwarder"
            mac="90:2e:16:d6:96:ba"
            lan="enp1s0f0np0"
            fso="enp1s0f1np1"
            online
          />

          {/* Center: Pulse ring + link signals */}
          <div className="flex flex-col items-center gap-4">
            <div className="text-[10px] tracking-[0.32em] uppercase text-[color:var(--color-text-secondary)]">
              FSO Link Integrity
            </div>
            <PulseRing state={link.state} qualityPct={link.qualityPct} size={260} />
            <div className="grid grid-cols-3 gap-5 w-full max-w-[360px]">
              <LinkStat label="RSSI" value={rssi} />
              <LinkStat label="SNR" value={snr} />
              <LinkStat label="BER" value={ber} />
            </div>
          </div>

          {/* Right: Endpoint B */}
          <EndpointCard
            side="B"
            title="Machine B — C4Net-10G5TH"
            mac="08:c0:eb:62:34:50"
            lan="enp1s0f0np0"
            fso="enp1s0f1np1"
            online
          />
        </div>

        {/* Beam visual */}
        <div
          className="absolute top-1/2 left-[20%] right-[20%] h-px pointer-events-none"
          style={{
            background:
              "linear-gradient(90deg, transparent, rgba(0,212,255,0.7), transparent)",
            boxShadow: "0 0 14px rgba(0, 212, 255, 0.5)",
          }}
        />
      </div>
    </GlassPanel>
  );
}

function EndpointCard({
  side,
  title,
  mac,
  lan,
  fso,
  online,
}: {
  side: "A" | "B";
  title: string;
  mac: string;
  lan: string;
  fso: string;
  online: boolean;
}) {
  return (
    <div className={side === "B" ? "text-right" : ""}>
      <div className="text-xs tracking-[0.28em] uppercase font-semibold text-[color:var(--color-cyan-300)] mb-1">
        Endpoint {side}
      </div>
      <div className="font-display text-lg font-semibold text-[color:var(--color-text-primary)]">
        {title}
      </div>
      <div className="font-mono text-[11px] text-[color:var(--color-text-muted)] mt-1">{mac}</div>
      <div className={`mt-3 inline-flex items-center gap-2 px-2.5 py-1 rounded-md border ${online ? "border-[color:var(--color-success)]/40 bg-[color:var(--color-success)]/10" : "border-[color:var(--color-danger)]/40 bg-[color:var(--color-danger)]/10"}`}>
        <span
          className="w-1.5 h-1.5 rounded-full breathe"
          style={{
            background: online ? "var(--color-success)" : "var(--color-danger)",
            color: online ? "var(--color-success)" : "var(--color-danger)",
            boxShadow: online ? "0 0 8px var(--color-success)" : "0 0 8px var(--color-danger)",
          }}
        />
        <span className="text-[10px] font-semibold tracking-[0.18em] uppercase text-[color:var(--color-success)]">
          {online ? "Online" : "Offline"}
        </span>
      </div>
      <div className={`mt-4 grid grid-cols-2 gap-3 ${side === "B" ? "justify-items-end" : ""}`}>
        <InterfaceInfo label="LAN IFACE" value={lan} align={side === "B" ? "right" : "left"} />
        <InterfaceInfo label="FSO IFACE" value={fso} align={side === "B" ? "right" : "left"} />
      </div>
    </div>
  );
}

function InterfaceInfo({
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
      <div className="font-mono text-xs text-[color:var(--color-text-primary)] mt-0.5">{value}</div>
    </div>
  );
}

function LinkStat({ label, value }: { label: string; value: string }) {
  return (
    <div className="text-center glass rounded-md px-3 py-2 border-[color:var(--color-border-cyan)]/40">
      <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div className="font-mono text-sm font-semibold tabular text-[color:var(--color-cyan-300)] mt-0.5">
        {value}
      </div>
    </div>
  );
}
