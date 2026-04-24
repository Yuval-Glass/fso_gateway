"use client";

import { useMemo } from "react";
import {
  Activity,
  CheckCircle2,
  Hash,
  PackageSearch,
  Ruler,
  Shield,
  XCircle,
  Zap,
} from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { MetricCard } from "@/components/primitives/MetricCard";
import { useLogs } from "@/lib/useLogs";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatBytes, formatNumber } from "@/lib/utils";

export default function PacketInspectorPage() {
  const { snapshot: snap } = useTelemetry();
  // Re-use the live log feed to surface symbol/packet events without a
  // dedicated capture endpoint (Phase 5 will add real per-packet tap).
  const logs = useLogs(400);

  const packetEvents = useMemo(() => {
    if (logs.events.length === 0) return [];
    return logs.events.filter((e) => {
      const msg = e.message.toLowerCase();
      return (
        msg.includes("pkt_id") ||
        msg.includes("block_id") ||
        msg.includes("symbols") ||
        msg.includes("fragment") ||
        msg.includes("reassemble")
      );
    }).slice(-40).reverse();
  }, [logs.events]);

  if (!snap) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          Loading packet telemetry…
        </div>
      </div>
    );
  }

  const cfg = snap.configEcho;
  const latest = snap.throughput[snap.throughput.length - 1];
  const avgTxPkt = latest.txPps > 0 ? latest.txBps / 8 / latest.txPps : 0;
  const symbolsPerPacket = cfg && avgTxPkt > 0
    ? Math.max(1, Math.ceil(avgTxPkt / cfg.symbolSize))
    : 0;

  const e = snap.errors;
  const totalPackets = e.blocksAttempted > 0
    ? Math.round(e.blocksAttempted * (cfg?.k ?? 1) / Math.max(1, symbolsPerPacket))
    : 0;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Packet Inspector
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Wire-level protocol reference and live symbol-event feed. Deep capture
            (pcap export, per-packet correlation) lands in Phase 5.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4H · Inspector
        </div>
      </div>

      {/* Activity strip */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <MetricCard
          label="Packets / sec"
          value={formatNumber(Math.round(latest.txPps))}
          unit="tx"
          tone="cyan"
          icon={<Activity size={14} />}
          sub={
            <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
              RX: {formatNumber(Math.round(latest.rxPps))}
            </span>
          }
        />
        <MetricCard
          label="Avg Packet Size"
          value={avgTxPkt > 0 ? avgTxPkt.toFixed(0) : "—"}
          unit="bytes"
          icon={<Ruler size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              ≈ {symbolsPerPacket} symbols each
            </span>
          }
        />
        <MetricCard
          label="Symbols Processed"
          value={formatNumber(e.blocksAttempted * (cfg?.k ?? 0))}
          tone="cyan"
          icon={<Hash size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              Source symbols since start
            </span>
          }
        />
        <MetricCard
          label="CRC Drops"
          value={formatNumber(e.crcDrops)}
          tone={e.crcDrops === 0 ? "success" : e.crcDrops > 100 ? "warning" : "cyan"}
          icon={<Shield size={14} />}
          sub={
            <span className="text-[color:var(--color-text-secondary)]">
              {cfg?.internalSymbolCrc ? "Per-symbol CRC-32C enabled" : "CRC disabled"}
            </span>
          }
        />
      </div>

      {/* Main row: wire format + live events */}
      <div className="grid grid-cols-1 xl:grid-cols-[3fr_2fr] gap-4">
        <GlassPanel
          label="Symbol Wire Format"
          trailing={
            <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
              18-byte header + payload
            </span>
          }
        >
          <WireFormatDiagram symbolSize={cfg?.symbolSize ?? 0} />
        </GlassPanel>

        <GlassPanel
          label="Recent Symbol Events"
          trailing={
            <span className="flex items-center gap-2 text-[10px] tracking-[0.18em] uppercase">
              <span
                className="w-1.5 h-1.5 rounded-full breathe"
                style={{
                  background: logs.connected ? "var(--color-success)" : "var(--color-text-muted)",
                  boxShadow: logs.connected ? "0 0 6px var(--color-success)" : "none",
                }}
              />
              <span style={{ color: logs.connected ? "var(--color-success)" : "var(--color-text-muted)" }}>
                {logs.mode.toUpperCase()}
              </span>
            </span>
          }
        >
          {packetEvents.length === 0 ? (
            <div className="py-10 text-center text-[11px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
              Waiting for packet events…
            </div>
          ) : (
            <ul className="space-y-1 max-h-[360px] overflow-y-auto font-mono text-[11px]">
              {packetEvents.map((ev, i) => {
                const time = new Date(ev.ts_ms).toISOString().slice(11, 19);
                const sev =
                  ev.level === "ERROR" ? "var(--color-danger)"
                  : ev.level === "WARN" ? "var(--color-warning)" : "var(--color-cyan-300)";
                return (
                  <li
                    key={i}
                    className="grid grid-cols-[auto_auto_1fr] gap-3 rounded px-1 -mx-1 hover:bg-white/[0.03]"
                  >
                    <span className="tabular text-[color:var(--color-text-muted)]">{time}</span>
                    <span className="font-semibold tracking-[0.1em]" style={{ color: sev }}>
                      [{ev.module}]
                    </span>
                    <span className="text-[color:var(--color-text-primary)] truncate">
                      {ev.message}
                    </span>
                  </li>
                );
              })}
            </ul>
          )}
        </GlassPanel>
      </div>

      {/* Planned features */}
      <GlassPanel label="Planned — Phase 5 Capture">
        <ul className="grid grid-cols-1 md:grid-cols-2 gap-x-8 gap-y-2 py-1">
          {[
            "Live capture with BPF filters",
            "Per-packet pipeline trace (fragment → FEC → interleave → wire → …)",
            "Error frame highlighting (CRC drop, missing symbol, decode fail)",
            "Pcap export (standard libpcap format)",
            "Cross-endpoint packet correlation by packet_id",
            "Histograms over size, block_id, symbol_index",
          ].map((f) => (
            <li key={f} className="flex items-start gap-2.5 text-sm text-[color:var(--color-text-secondary)]">
              <span
                className="mt-1.5 w-1.5 h-1.5 rounded-full shrink-0"
                style={{ background: "var(--color-cyan-500)", boxShadow: "0 0 6px rgba(0, 212, 255, 0.7)" }}
              />
              <span>{f}</span>
            </li>
          ))}
        </ul>
      </GlassPanel>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Wire format diagram                                                         */
/* -------------------------------------------------------------------------- */

interface HeaderField {
  name: string;
  size: number; // bytes
  description: string;
  color: string;
}

const HEADER_FIELDS: HeaderField[] = [
  { name: "packet_id",     size: 4, description: "Monotonic per-packet identifier (fragment-level)", color: "#00d4ff" },
  { name: "fec_id",        size: 2, description: "FEC block identifier",                              color: "#5aa0ff" },
  { name: "symbol_index",  size: 2, description: "Symbol position within the block (0..K+M-1)",       color: "#a78bfa" },
  { name: "total_symbols", size: 2, description: "Symbols in this block (K+M)",                       color: "#34d399" },
  { name: "payload_len",   size: 2, description: "Valid payload bytes in this symbol",                color: "#ffb020" },
  { name: "reserved",      size: 2, description: "Reserved / alignment",                              color: "#566377" },
  { name: "crc32c",        size: 4, description: "CRC-32C over this symbol (when enabled)",           color: "#ff2d5c" },
];

function WireFormatDiagram({ symbolSize }: { symbolSize: number }) {
  const totalHeader = HEADER_FIELDS.reduce((a, b) => a + b.size, 0);
  const payloadSize = Math.max(0, symbolSize - totalHeader);
  const totalForScale = Math.max(symbolSize, totalHeader + 32);

  return (
    <div className="flex flex-col gap-5 py-3">
      {/* Byte layout bar */}
      <div>
        <div className="flex h-10 rounded-md overflow-hidden ring-1 ring-[color:var(--color-border-hair)]">
          {HEADER_FIELDS.map((f) => {
            const pct = (f.size / totalForScale) * 100;
            return (
              <div
                key={f.name}
                className="relative flex items-center justify-center text-[9px] font-mono tabular border-r border-[rgba(255,255,255,0.1)]"
                style={{
                  flexBasis: `${pct}%`,
                  background: `linear-gradient(180deg, ${f.color}40, ${f.color}20)`,
                  color: f.color,
                  textShadow: "0 0 4px rgba(0,0,0,0.6)",
                }}
                title={`${f.name} · ${f.size}B`}
              >
                {pct > 1.5 ? `${f.size}B` : ""}
              </div>
            );
          })}
          <div
            className="relative flex items-center justify-center text-[10px] font-mono tabular text-[color:var(--color-text-muted)]"
            style={{
              flex: 1,
              background: "repeating-linear-gradient(45deg, rgba(255,255,255,0.03), rgba(255,255,255,0.03) 6px, transparent 6px, transparent 12px)",
            }}
            title={`payload · ${payloadSize}B`}
          >
            payload · {formatBytes(payloadSize, 0)}
          </div>
        </div>
        <div className="mt-2 flex items-center justify-between text-[10px] font-mono tabular text-[color:var(--color-text-muted)]">
          <span>0</span>
          <span>header {totalHeader}B</span>
          <span>symbol total {formatBytes(symbolSize, 0)}</span>
        </div>
      </div>

      {/* Field table */}
      <div className="grid grid-cols-1 md:grid-cols-2 gap-x-6 gap-y-2">
        {HEADER_FIELDS.map((f) => (
          <div key={f.name} className="flex items-start gap-3">
            <span
              className="mt-1 w-2 h-2 rounded-sm shrink-0"
              style={{ background: f.color, boxShadow: `0 0 6px ${f.color}80` }}
            />
            <div className="min-w-0 flex-1">
              <div className="flex items-baseline justify-between gap-2">
                <span className="font-mono text-[11px] font-semibold text-[color:var(--color-text-primary)]">
                  {f.name}
                </span>
                <span className="font-mono text-[10px] text-[color:var(--color-text-muted)]">{f.size}B</span>
              </div>
              <div className="text-[10px] text-[color:var(--color-text-secondary)] leading-snug mt-0.5">
                {f.description}
              </div>
            </div>
          </div>
        ))}
      </div>

      <div className="mt-1 text-[10px] leading-relaxed text-[color:var(--color-text-muted)]">
        Source: <span className="font-mono">src/fso_protocol.c</span>. Header is prepended to every symbol
        emitted by the TX pipeline after fragmentation & FEC encode. On RX, symbols
        are deserialized, CRC-verified (when enabled), deinterleaved, and fed to the
        FEC decoder in block_id order.
      </div>
    </div>
  );
}
