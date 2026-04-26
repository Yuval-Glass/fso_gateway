"use client";

import Link from "next/link";
import { PulseRing } from "@/components/primitives/PulseRing";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { FieldHint } from "@/components/primitives/FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";
import type { ConfigEcho, LinkStatus } from "@/types/telemetry";
import { formatNumber, formatPercent } from "@/lib/utils";

/**
 * Hardware layout is the Phase 8 two-machine FSO demo:
 *   Win1 ──── GW-A ══════ GW-B ──── Win2
 *
 * The labels below reflect the README-documented deployment. GW-A/GW-B
 * interface names come from the live config echo; the Windows endpoints
 * are the bench PCs behind each gateway (their IPs/MACs are fixed in the
 * Phase 8 setup but only mentioned as context — the daemon has no way
 * to observe the remote endpoint directly).
 */
interface LinkStatusHeroProps {
  link: LinkStatus;
  cfg: ConfigEcho;
}

export function LinkStatusHero({ link, cfg }: LinkStatusHeroProps) {
  return (
    <GlassPanel variant="raised" padded={false} className="overflow-hidden">
      <div className="relative">
        <div className="scanlines absolute inset-0 pointer-events-none" />

        <div className="grid grid-cols-[1fr_auto_1fr] items-center gap-8 px-8 py-7">
          <GatewaySide
            side="A"
            peerLabel="Win-1 · 192.168.50.1"
            lan={cfg.lanIface || "enp1s0f0np0"}
            fso={cfg.fsoIface || "enp1s0f1np1"}
            online={link.state !== "offline"}
          />

          <div className="flex flex-col items-center gap-4">
            <Link
              href="/link-status"
              className="text-[10px] tracking-[0.32em] uppercase text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] transition-colors"
            >
              FSO Link Integrity →
            </Link>
            <PulseRing state={link.state} qualityPct={link.qualityPct} size={240} />
            <div className="grid grid-cols-3 gap-4 w-full max-w-[440px]">
              <Stat label="State" value={link.state.toUpperCase()} hintId="link.state" />
              <Stat label="Quality" value={formatPercent(link.qualityPct / 100, 2)} hintId="link.qualityPct" />
              <Stat label="K/M/D" value={`${cfg.k}/${cfg.m}/${cfg.depth}`} hintId="config.k" />
            </div>
          </div>

          <GatewaySide
            side="B"
            peerLabel="Win-2 · 192.168.50.2"
            lan={cfg.lanIface || "enp1s0f0np0"}
            fso={cfg.fsoIface || "enp1s0f1np1"}
            online={link.state !== "offline"}
          />
        </div>

        <div
          className="absolute top-1/2 left-[22%] right-[22%] h-px pointer-events-none"
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

function GatewaySide({
  side,
  peerLabel,
  lan,
  fso,
  online,
}: {
  side: "A" | "B";
  peerLabel: string;
  lan: string;
  fso: string;
  online: boolean;
}) {
  return (
    <div className={side === "B" ? "text-right" : ""}>
      <div className="text-xs tracking-[0.28em] uppercase font-semibold text-[color:var(--color-cyan-300)] mb-1">
        Gateway {side}
      </div>
      <div className="font-display text-lg font-semibold text-[color:var(--color-text-primary)]">
        GW-{side}
      </div>
      <div className="text-[11px] text-[color:var(--color-text-muted)] mt-1">
        {peerLabel}
      </div>
      <div className={`mt-3 inline-flex items-center gap-2 px-2.5 py-1 rounded-md border ${online ? "border-[color:var(--color-success)]/40 bg-[color:var(--color-success)]/10" : "border-[color:var(--color-danger)]/40 bg-[color:var(--color-danger)]/10"}`}>
        <span
          className="w-1.5 h-1.5 rounded-full breathe"
          style={{
            background: online ? "var(--color-success)" : "var(--color-danger)",
            color: online ? "var(--color-success)" : "var(--color-danger)",
            boxShadow: online ? "0 0 8px var(--color-success)" : "0 0 8px var(--color-danger)",
          }}
        />
        <span className={`text-[10px] font-semibold tracking-[0.18em] uppercase ${online ? "text-[color:var(--color-success)]" : "text-[color:var(--color-danger)]"}`}>
          {online ? "Online" : "Offline"}
        </span>
      </div>
      <div className={`mt-4 grid grid-cols-2 gap-3 ${side === "B" ? "justify-items-end" : ""}`}>
        <IfaceInfo label="LAN IFACE" value={lan} align={side === "B" ? "right" : "left"} hintId="config.lanIface" />
        <IfaceInfo label="FSO IFACE" value={fso} align={side === "B" ? "right" : "left"} hintId="config.fsoIface" />
      </div>
    </div>
  );
}

function IfaceInfo({
  label,
  value,
  align,
  hintId,
}: {
  label: string;
  value: string;
  align: "left" | "right";
  hintId?: FieldHintId;
}) {
  return (
    <div className={align === "right" ? "text-right" : ""}>
      <div className={`text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1 ${align === "right" ? "flex-row-reverse" : ""}`}>
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </div>
      <div className="font-mono text-xs text-[color:var(--color-text-primary)] mt-0.5">{value}</div>
    </div>
  );
}

function Stat({ label, value, hintId }: { label: string; value: string; hintId?: FieldHintId }) {
  return (
    <div className="text-center glass rounded-md px-3 py-2 border-[color:var(--color-border-cyan)]/40">
      <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </div>
      <div className="font-mono text-sm font-semibold tabular text-[color:var(--color-cyan-300)] mt-0.5">
        {value}
      </div>
    </div>
  );
}
