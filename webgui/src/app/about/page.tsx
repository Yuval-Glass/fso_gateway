"use client";

import {
  ArrowRight,
  Boxes,
  Cpu,
  Database,
  ExternalLink,
  Layers,
  Network,
  Radio,
  Server,
  Sparkles,
  Wifi,
} from "lucide-react";
import { BrandMark } from "@/components/layout/BrandMark";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { useHealth } from "@/lib/useHealth";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatUptime } from "@/lib/utils";

const APP_NAME = "FSO Gateway Control Center";
const APP_VERSION = "v1.0.0-phase5";

export default function AboutPage() {
  const health = useHealth();
  const { snapshot: snap } = useTelemetry();

  const bridgeUp = !!health;
  const gatewayUp = health?.source === "gateway";
  const logsMode = health?.logs_mode ?? "idle";
  const activeRun = health?.active_run_id ?? null;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            About System
          </h2>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 5 · Final
        </div>
      </div>

      {/* Brand hero */}
      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="relative scanlines">
          <div className="flex flex-col md:flex-row items-center gap-8 px-10 py-10">
            <div className="shrink-0">
              <BrandMark size={140} />
            </div>
            <div className="text-center md:text-left flex-1 min-w-0">
              <div className="text-[10px] tracking-[0.32em] uppercase text-[color:var(--color-cyan-300)]">
                BER Killerz · Engineering
              </div>
              <h1 className="font-display text-3xl md:text-4xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-1">
                {APP_NAME}
              </h1>
              <div className="mt-2 text-sm text-[color:var(--color-text-secondary)] max-w-2xl">
                Real-time telemetry, control, and analysis for the FSO Gateway —
                an Ethernet-over-Free-Space-Optical link with fountain-code FEC,
                matrix interleaving, and burst-loss mitigation.
              </div>
              <div className="mt-3 inline-flex items-center gap-2 font-mono text-[11px] text-[color:var(--color-text-muted)]">
                <span>{APP_VERSION}</span>
                <span className="opacity-50">·</span>
                <span>build {snap?.system.build ?? "—"}</span>
              </div>
            </div>
          </div>
          {/* beam decoration */}
          <div
            className="absolute top-1/2 left-[15%] right-[15%] h-px pointer-events-none"
            style={{
              background: "linear-gradient(90deg, transparent, rgba(0,212,255,0.5), transparent)",
              boxShadow: "0 0 14px rgba(0, 212, 255, 0.4)",
            }}
          />
        </div>
      </GlassPanel>

      {/* System status grid */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
        <StatusCard
          icon={<Wifi size={16} />}
          label="Bridge"
          status={bridgeUp ? "online" : "offline"}
          primary={bridgeUp ? "Connected" : "Unreachable"}
          detail={bridgeUp ? `Tick ${health?.tick_hz}Hz` : "Start it with `uv run uvicorn …`"}
        />
        <StatusCard
          icon={<Server size={16} />}
          label="Gateway Daemon"
          status={gatewayUp ? "online" : "warning"}
          primary={gatewayUp ? "Live (control_server)" : "Mock data"}
          detail={gatewayUp ? "Streaming via UNIX socket" : "control_server_demo or fso_gw_runner not running"}
        />
        <StatusCard
          icon={<Database size={16} />}
          label="Recording"
          status={activeRun !== null ? "online" : "offline"}
          primary={activeRun !== null ? `Run #${activeRun}` : "Not recording"}
          detail={activeRun !== null ? `Logs mode: ${logsMode}` : "Open Analytics to start a run"}
        />
      </div>

      {/* Architecture diagram */}
      <GlassPanel label="Architecture">
        <div className="py-4">
          <ArchitectureDiagram bridgeUp={bridgeUp} gatewayUp={gatewayUp} />
        </div>
        <div className="mt-2 text-[11px] text-[color:var(--color-text-secondary)] leading-relaxed">
          The dashboard talks to a thin Python <span className="font-mono">FastAPI</span>{" "}
          bridge (<span className="font-mono">webgui/server/</span>) over WebSocket and
          REST. The bridge enriches raw counters from the C{" "}
          <span className="font-mono">control_server</span> thread (running inside{" "}
          <span className="font-mono">fso_gw_runner</span>) into the full
          telemetry shape the UI consumes, and persists a downsampled stream
          into SQLite for the Analytics page.
        </div>
      </GlassPanel>

      {/* Tech stack + Phase roadmap */}
      <div className="grid grid-cols-1 xl:grid-cols-2 gap-4">
        <TechStackPanel />
        <PhaseRoadmapPanel />
      </div>

      {/* Module map + Acknowledgments */}
      <div className="grid grid-cols-1 xl:grid-cols-[1fr_1fr] gap-4">
        <ModuleMapPanel />
        <AcknowledgmentsPanel />
      </div>
    </div>
  );
}

/* -------------------------------------------------------------------------- */
/* Sub-components                                                              */
/* -------------------------------------------------------------------------- */

function StatusCard({
  icon,
  label,
  status,
  primary,
  detail,
}: {
  icon: React.ReactNode;
  label: string;
  status: "online" | "warning" | "offline";
  primary: string;
  detail: string;
}) {
  const color =
    status === "online" ? "var(--color-success)"
    : status === "warning" ? "var(--color-warning)" : "var(--color-danger)";
  const glow =
    status === "online" ? "rgba(52, 211, 153, 0.45)"
    : status === "warning" ? "rgba(255, 176, 32, 0.45)" : "rgba(255, 45, 92, 0.45)";

  return (
    <GlassPanel variant="raised" padded={false}>
      <div className="px-4 py-3 flex items-start gap-3">
        <div
          className="w-10 h-10 rounded-md flex items-center justify-center shrink-0 border"
          style={{
            color,
            borderColor: `${color}55`,
            background: `${color}10`,
            boxShadow: `inset 0 0 12px ${glow}`,
          }}
        >
          {icon}
        </div>
        <div className="min-w-0 flex-1">
          <div className="text-[9px] font-medium tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
            {label}
          </div>
          <div
            className="font-display text-base font-semibold mt-0.5 truncate"
            style={{ color }}
          >
            {primary}
          </div>
          <div className="text-[10px] text-[color:var(--color-text-secondary)] mt-0.5 leading-snug">
            {detail}
          </div>
        </div>
        <span
          className="w-2 h-2 mt-1.5 rounded-full breathe shrink-0"
          style={{ background: color, boxShadow: `0 0 6px ${glow}` }}
        />
      </div>
    </GlassPanel>
  );
}

function ArchitectureDiagram({ bridgeUp, gatewayUp }: { bridgeUp: boolean; gatewayUp: boolean }) {
  const liveColor = "var(--color-success)";
  const dimColor = "var(--color-text-muted)";
  return (
    <div className="grid grid-cols-1 md:grid-cols-[1fr_auto_1fr_auto_1fr] items-center gap-3">
      <ArchNode icon={<Sparkles size={18} />} title="Next.js GUI" detail="React 19 · Tailwind 4 · ECharts" port=":3100" online />
      <ArchArrow color={bridgeUp ? liveColor : dimColor} label="WS / REST" />
      <ArchNode icon={<Server size={18} />} title="FastAPI Bridge" detail="Python 3.12 · uvicorn · SQLite" port=":8000" online={bridgeUp} />
      <ArchArrow color={gatewayUp ? liveColor : dimColor} label="UNIX socket" />
      <ArchNode icon={<Cpu size={18} />} title="C Daemon" detail="fso_gw_runner · libpcap · pthreads" port="/tmp/fso_gw.sock" online={gatewayUp} />
    </div>
  );
}

function ArchNode({
  icon,
  title,
  detail,
  port,
  online,
}: {
  icon: React.ReactNode;
  title: string;
  detail: string;
  port: string;
  online: boolean;
}) {
  const c = online ? "var(--color-cyan-300)" : "var(--color-text-muted)";
  const border = online ? "var(--color-border-cyan)" : "var(--color-border-subtle)";
  const glow = online ? "0 0 24px rgba(0, 212, 255, 0.18)" : "none";
  return (
    <div
      className="glass rounded-lg px-4 py-3 text-center border"
      style={{ borderColor: border, boxShadow: glow }}
    >
      <div
        className="w-9 h-9 mx-auto rounded-md flex items-center justify-center"
        style={{ color: c, background: online ? "rgba(0, 212, 255, 0.08)" : "transparent" }}
      >
        {icon}
      </div>
      <div className="mt-1.5 font-display text-sm font-semibold" style={{ color: c }}>
        {title}
      </div>
      <div className="text-[10px] text-[color:var(--color-text-secondary)] mt-0.5">
        {detail}
      </div>
      <div className="font-mono text-[10px] text-[color:var(--color-text-muted)] mt-1">
        {port}
      </div>
    </div>
  );
}

function ArchArrow({ color, label }: { color: string; label: string }) {
  return (
    <div className="flex flex-col items-center gap-1 hidden md:flex">
      <div className="text-[9px] tracking-[0.22em] uppercase font-mono" style={{ color }}>
        {label}
      </div>
      <ArrowRight size={20} style={{ color }} />
    </div>
  );
}

function TechStackPanel() {
  const items = [
    { name: "Next.js 16", role: "App Router · Turbopack" },
    { name: "React 19", role: "Server / Client components" },
    { name: "TypeScript 5", role: "End-to-end typing" },
    { name: "Tailwind CSS 4", role: "@theme inline tokens" },
    { name: "ECharts", role: "Time-series + histograms" },
    { name: "Framer Motion", role: "Micro-interactions" },
    { name: "lucide-react", role: "Icon system" },
    { name: "FastAPI", role: "Bridge: WS + REST" },
    { name: "uvicorn", role: "ASGI server" },
    { name: "SQLite", role: "Run / sample persistence" },
    { name: "pthreads", role: "C-side control_server" },
    { name: "libpcap", role: "Raw Ethernet I/O" },
  ];
  return (
    <GlassPanel label="Tech Stack" trailing={<Layers size={12} className="text-[color:var(--color-cyan-300)]" />}>
      <ul className="grid grid-cols-2 gap-x-4 gap-y-2 py-1">
        {items.map((it) => (
          <li key={it.name} className="flex items-baseline justify-between gap-2 text-[11px]">
            <span className="font-mono text-[color:var(--color-text-primary)]">{it.name}</span>
            <span className="text-[10px] tracking-[0.1em] text-[color:var(--color-text-muted)] truncate">
              {it.role}
            </span>
          </li>
        ))}
      </ul>
    </GlassPanel>
  );
}

function PhaseRoadmapPanel() {
  const phases: Array<{ name: string; what: string; status: "done" | "office" | "polish" }> = [
    { name: "Phase 1",  what: "Shell · design system · dashboard skeleton",          status: "done" },
    { name: "Phase 2A", what: "FastAPI bridge + WebSocket + mock fallback",          status: "done" },
    { name: "Phase 2B", what: "C control_server pthread → bridge enrichment",        status: "done" },
    { name: "Phase 3A", what: "Control Center config form + YAML persistence",       status: "done" },
    { name: "Phase 3B", what: "Daemon supervision (start/stop/restart)",             status: "office" },
    { name: "Phase 4A", what: "Live log streaming with file-tail + mock",            status: "done" },
    { name: "Phase 4B", what: "Topology page (animated beam + endpoints)",           status: "done" },
    { name: "Phase 4C", what: "FEC Analytics page",                                  status: "done" },
    { name: "Phase 4D", what: "Alerts page (history + ack)",                         status: "done" },
    { name: "Phase 4E", what: "Link Status page (RSSI/SNR/BER + fade events)",       status: "done" },
    { name: "Phase 4F", what: "Traffic Monitor page",                                status: "done" },
    { name: "Phase 4G", what: "Interleaver page (matrix + burst coverage)",          status: "done" },
    { name: "Phase 4H", what: "Packet Inspector page (wire format)",                 status: "done" },
    { name: "Phase 5",  what: "Analytics & history (SQLite + run compare prep)",    status: "done" },
    { name: "Polish",   what: "Cosmetic pass — deferred bugs, layout tuning",        status: "polish" },
  ];
  return (
    <GlassPanel label="Roadmap" trailing={<Boxes size={12} className="text-[color:var(--color-cyan-300)]" />}>
      <ul className="space-y-1.5 py-1">
        {phases.map((p) => {
          const cfg = STATUS_CFG[p.status];
          return (
            <li key={p.name} className="flex items-center gap-3 text-[11px]">
              <span
                className={cn("w-2 h-2 rounded-full shrink-0", p.status === "done" ? "" : "breathe")}
                style={{ background: cfg.color, boxShadow: `0 0 6px ${cfg.glow}` }}
              />
              <span
                className="font-semibold tracking-[0.12em] uppercase text-[10px] shrink-0 w-20"
                style={{ color: cfg.color }}
              >
                {p.name}
              </span>
              <span className="text-[color:var(--color-text-secondary)] flex-1 min-w-0 truncate">
                {p.what}
              </span>
              <span
                className="text-[9px] tracking-[0.18em] uppercase shrink-0"
                style={{ color: cfg.color }}
              >
                {cfg.label}
              </span>
            </li>
          );
        })}
      </ul>
    </GlassPanel>
  );
}

const STATUS_CFG = {
  done:   { color: "var(--color-success)", glow: "rgba(52, 211, 153, 0.6)", label: "Done" },
  office: { color: "var(--color-warning)", glow: "rgba(255, 176, 32, 0.6)", label: "Office" },
  polish: { color: "var(--color-cyan-500)", glow: "rgba(0, 212, 255, 0.6)", label: "Pending" },
};

function ModuleMapPanel() {
  const modules = [
    { module: "tx_pipeline",    role: "LAN RX → fragment → FEC encode → interleave → FSO TX" },
    { module: "rx_pipeline",    role: "FSO RX → deinterleave → FEC decode → reassemble → LAN TX" },
    { module: "fec_wrapper",    role: "Wirehair fountain code encode/decode" },
    { module: "interleaver",    role: "Matrix interleaver (depth × K+M)" },
    { module: "deinterleaver",  role: "Inverse, sparse-safe" },
    { module: "block_builder",  role: "Accumulate symbols into FEC blocks; flush on timeout" },
    { module: "packet_io",      role: "libpcap-based raw frame I/O (also packet_io_dpdk)" },
    { module: "stats",          role: "Lock-free atomic counters + percentile sampling" },
    { module: "control_server", role: "AF_UNIX server emitting JSON snapshots @ 10 Hz" },
    { module: "logging",        role: "Thread-safe levelled output" },
    { module: "config",         role: "CLI argument parser (getopt_long)" },
    { module: "arp_cache",      role: "MAC↔IP map for proxy ARP" },
    { module: "fso_protocol",   role: "18-byte symbol header serialization" },
  ];
  return (
    <GlassPanel
      label="C Module Map"
      trailing={<Network size={12} className="text-[color:var(--color-cyan-300)]" />}
    >
      <ul className="grid grid-cols-1 md:grid-cols-2 gap-x-4 gap-y-1.5 py-1">
        {modules.map((m) => (
          <li key={m.module} className="text-[11px]">
            <div className="font-mono font-semibold text-[color:var(--color-cyan-300)]">
              {m.module}
            </div>
            <div className="text-[10px] text-[color:var(--color-text-secondary)] leading-snug">
              {m.role}
            </div>
          </li>
        ))}
      </ul>
    </GlassPanel>
  );
}

function AcknowledgmentsPanel() {
  return (
    <GlassPanel label="Acknowledgments">
      <div className="text-[11px] text-[color:var(--color-text-secondary)] space-y-3 leading-relaxed py-1">
        <p>
          Built on the shoulders of open-source giants. Major direct dependencies:
        </p>
        <ul className="space-y-1 ml-4 list-disc marker:text-[color:var(--color-cyan-500)]">
          <li>
            <a
              className="text-[color:var(--color-cyan-300)] hover:underline inline-flex items-center gap-1"
              href="https://github.com/catid/wirehair"
              target="_blank" rel="noopener noreferrer"
            >
              Wirehair fountain code <ExternalLink size={9} />
            </a>{" "}
            — FEC encode/decode (vendored under <span className="font-mono">third_party/wirehair/</span>)
          </li>
          <li>
            <a
              className="text-[color:var(--color-cyan-300)] hover:underline inline-flex items-center gap-1"
              href="https://www.tcpdump.org/" target="_blank" rel="noopener noreferrer"
            >
              libpcap <ExternalLink size={9} />
            </a>{" "}
            — raw Ethernet capture / inject
          </li>
          <li>
            <a
              className="text-[color:var(--color-cyan-300)] hover:underline inline-flex items-center gap-1"
              href="https://nextjs.org/" target="_blank" rel="noopener noreferrer"
            >
              Next.js <ExternalLink size={9} />
            </a>
            ,{" "}
            <a
              className="text-[color:var(--color-cyan-300)] hover:underline inline-flex items-center gap-1"
              href="https://fastapi.tiangolo.com/" target="_blank" rel="noopener noreferrer"
            >
              FastAPI <ExternalLink size={9} />
            </a>
            ,{" "}
            <a
              className="text-[color:var(--color-cyan-300)] hover:underline inline-flex items-center gap-1"
              href="https://echarts.apache.org/" target="_blank" rel="noopener noreferrer"
            >
              ECharts <ExternalLink size={9} />
            </a>
          </li>
        </ul>
        <p className="text-[10px] text-[color:var(--color-text-muted)]">
          Designed and built for the BER Killerz team. Brand identity by the team.
          For source contributions and issues, open a PR on the project repository.
        </p>
        <div className="pt-2 mt-2 border-t border-[color:var(--color-border-hair)] flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
          <Radio size={11} className="text-[color:var(--color-cyan-300)]" />
          <span>{APP_NAME}</span>
          <span className="opacity-50">·</span>
          <span>{APP_VERSION}</span>
        </div>
      </div>
    </GlassPanel>
  );
}
