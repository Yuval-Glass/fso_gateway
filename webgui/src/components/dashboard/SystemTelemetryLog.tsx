"use client";

import { GlassPanel } from "@/components/primitives/GlassPanel";
import { Pause, Save } from "lucide-react";

const LOG_LINES = [
  { t: "23:01:45.32", mod: "RX", msg: "reassemble_packet success pkt_id=2303 len=74" },
  { t: "23:01:45.33", mod: "TX", msg: "block_builder block 121 complete (K=2)" },
  { t: "23:01:45.34", mod: "ARP", msg: "proxy_arp target 192.168.50.2 handled locally" },
  { t: "23:01:45.36", mod: "FEC", msg: "encode_block block_id=121 symbols_generated=14" },
  { t: "23:01:45.37", mod: "LINK", msg: "fso_tx burst transmitted size=14" },
  { t: "23:01:45.39", mod: "RX", msg: "deinterleaver flush depth=8 blocks_flushed=1" },
  { t: "23:01:45.41", mod: "FEC", msg: "decode_block block_id=120 recovered K=2 M=1" },
  { t: "23:01:45.44", mod: "TX", msg: "fragment pkt_id=2304 len=1514 → symbols=8" },
];

const modColor: Record<string, string> = {
  RX: "var(--color-success)",
  TX: "var(--color-cyan-300)",
  ARP: "var(--color-warning)",
  FEC: "var(--color-blue-400)",
  LINK: "var(--color-cyan-500)",
};

export function SystemTelemetryLog() {
  return (
    <GlassPanel
      label="System Telemetry Log"
      labelHref="/logs"
      trailing={
        <div className="flex items-center gap-1">
          <button className="flex items-center gap-1 px-2 py-1 rounded text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04]">
            <Pause size={11} /> Pause
          </button>
          <button className="flex items-center gap-1 px-2 py-1 rounded text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04]">
            <Save size={11} /> Save
          </button>
        </div>
      }
    >
      <div className="font-mono text-[11px] leading-relaxed">
        <div className="grid grid-cols-[auto_auto_1fr] gap-x-4 text-[color:var(--color-text-muted)] text-[9px] tracking-[0.2em] uppercase pb-1.5 border-b border-[color:var(--color-border-hair)] mb-1.5">
          <span>Time (UTC)</span>
          <span>Module</span>
          <span>Message</span>
        </div>
        <div className="space-y-0.5">
          {LOG_LINES.map((line, i) => (
            <div key={i} className="grid grid-cols-[auto_auto_1fr] gap-x-4 hover:bg-white/[0.02] rounded px-0.5 py-0.5">
              <span className="text-[color:var(--color-text-muted)] tabular">{line.t}</span>
              <span
                className="font-semibold tracking-[0.1em] uppercase"
                style={{ color: modColor[line.mod] ?? "var(--color-text-secondary)" }}
              >
                [{line.mod}]
              </span>
              <span className="text-[color:var(--color-text-primary)]">{line.msg}</span>
            </div>
          ))}
        </div>
      </div>
    </GlassPanel>
  );
}
