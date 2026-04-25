"use client";

import {
  AlertOctagon,
  BellRing,
  Check,
  CheckCheck,
  Eraser,
  Info,
  Search,
  TriangleAlert,
  X,
} from "lucide-react";
import { useMemo, useState } from "react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { TactileButton } from "@/components/primitives/TactileButton";
import { FieldHint } from "@/components/primitives/FieldHint";
import { useAlerts } from "@/lib/useAlerts";
import { useTelemetry } from "@/lib/useTelemetry";
import { cn, formatNumber } from "@/lib/utils";
import type { AlertEvent } from "@/types/telemetry";

type Severity = AlertEvent["severity"];
type Tab = "active" | "acknowledged" | "all";

const SEVERITY_ORDER: Severity[] = ["critical", "warning", "info"];

const SEVERITY_META: Record<
  Severity,
  { label: string; color: string; glow: string; Icon: typeof Info }
> = {
  critical: {
    label: "Critical",
    color: "var(--color-danger)",
    glow: "rgba(255, 45, 92, 0.6)",
    Icon: AlertOctagon,
  },
  warning: {
    label: "Warning",
    color: "var(--color-warning)",
    glow: "rgba(255, 176, 32, 0.55)",
    Icon: TriangleAlert,
  },
  info: {
    label: "Info",
    color: "var(--color-cyan-500)",
    glow: "rgba(0, 212, 255, 0.5)",
    Icon: Info,
  },
};

export default function AlertsPage() {
  const { snapshot: snap } = useTelemetry();
  const alerts = useAlerts(snap);

  const [tab, setTab] = useState<Tab>("active");
  const [severityFilter, setSeverityFilter] = useState<Record<Severity, boolean>>({
    critical: true,
    warning: true,
    info: true,
  });
  const [moduleFilter, setModuleFilter] = useState<string | null>(null);
  const [search, setSearch] = useState("");

  const knownModules = useMemo(() => {
    const set = new Set<string>();
    for (const a of alerts.all) set.add(a.module);
    return Array.from(set).sort();
  }, [alerts.all]);

  const filtered = useMemo(() => {
    const needle = search.trim().toLowerCase();
    return alerts.all.filter((a) => {
      const isAcked = alerts.acked.has(a.id);
      if (tab === "active" && isAcked) return false;
      if (tab === "acknowledged" && !isAcked) return false;
      if (!severityFilter[a.severity]) return false;
      if (moduleFilter && a.module !== moduleFilter) return false;
      if (needle) {
        const hay = `${a.module} ${a.message}`.toLowerCase();
        if (!hay.includes(needle)) return false;
      }
      return true;
    });
  }, [alerts.all, alerts.acked, tab, severityFilter, moduleFilter, search]);

  return (
    <div className="flex flex-col gap-5">
      {/* Page header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Alerts
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Session-level alert history. Acknowledgments persist in your browser.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4D · Alert History
        </div>
      </div>

      {/* Stats strip (clickable severity pills) */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
        <StatTile
          label="Active"
          value={alerts.counts.active}
          icon={<BellRing size={14} />}
          tone={alerts.counts.active === 0 ? "success" : "cyan"}
          active={tab === "active"}
          onClick={() => setTab("active")}
        />
        <StatTile
          label="Critical"
          value={alerts.counts.critical}
          icon={<AlertOctagon size={14} />}
          tone={alerts.counts.critical === 0 ? "success" : "danger"}
          active={!severityFilter.warning || !severityFilter.info ? false : !severityFilter.critical}
          onClick={() => setSeverityFilter((p) => ({ ...p, critical: !p.critical }))}
          dim={!severityFilter.critical}
        />
        <StatTile
          label="Warning"
          value={alerts.counts.warning}
          icon={<TriangleAlert size={14} />}
          tone={alerts.counts.warning === 0 ? "success" : "warning"}
          onClick={() => setSeverityFilter((p) => ({ ...p, warning: !p.warning }))}
          dim={!severityFilter.warning}
        />
        <StatTile
          label="Info"
          value={alerts.counts.info}
          icon={<Info size={14} />}
          tone="cyan"
          onClick={() => setSeverityFilter((p) => ({ ...p, info: !p.info }))}
          dim={!severityFilter.info}
        />
      </div>

      {/* Toolbar */}
      <GlassPanel padded={false}>
        <div className="flex flex-wrap items-center gap-3 px-4 py-3">
          {/* Tabs */}
          <div className="flex items-center rounded-md border border-[color:var(--color-border-subtle)] overflow-hidden">
            {(["active", "acknowledged", "all"] as Tab[]).map((t) => (
              <button
                key={t}
                onClick={() => setTab(t)}
                className={cn(
                  "px-3 py-1.5 text-[10px] font-semibold tracking-[0.2em] uppercase transition-colors",
                  tab === t
                    ? "bg-[color:var(--color-cyan-900)]/60 text-[color:var(--color-cyan-300)]"
                    : "text-[color:var(--color-text-secondary)] hover:bg-white/[0.03]",
                )}
              >
                {t}
                <span className="ml-1.5 opacity-60 font-mono">
                  {t === "active" ? alerts.counts.active
                    : t === "acknowledged" ? alerts.counts.acked
                    : alerts.counts.total}
                </span>
              </button>
            ))}
          </div>

          {/* Module filter */}
          <span className="inline-flex items-center gap-1">
            <select
              value={moduleFilter ?? ""}
              onChange={(e) => setModuleFilter(e.target.value || null)}
              className="bg-white/[0.02] border border-[color:var(--color-border-subtle)] rounded-md px-2 py-1.5 text-[11px] text-[color:var(--color-text-primary)] outline-none focus:border-[color:var(--color-cyan-500)]"
            >
              <option value="">All modules</option>
              {knownModules.map((m) => (
                <option key={m} value={m}>{m}</option>
              ))}
            </select>
            <FieldHint id="alert.module" size={10} />
          </span>

          {/* Search */}
          <div className="relative">
            <Search
              size={12}
              className="absolute left-2 top-1/2 -translate-y-1/2 text-[color:var(--color-text-muted)]"
            />
            <input
              type="text"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              placeholder="Search…"
              className="pl-7 pr-7 py-1.5 w-52 rounded-md border border-[color:var(--color-border-subtle)] bg-white/[0.02] text-xs text-[color:var(--color-text-primary)] outline-none focus:border-[color:var(--color-cyan-500)] placeholder:text-[color:var(--color-text-muted)]"
            />
            {search && (
              <button
                onClick={() => setSearch("")}
                className="absolute right-1.5 top-1/2 -translate-y-1/2 text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)]"
                aria-label="Clear search"
              >
                <X size={12} />
              </button>
            )}
          </div>

          <div className="flex-1" />

          {/* Bulk actions */}
          <div className="flex items-center gap-2">
            {tab === "active" && filtered.length > 0 && (
              <TactileButton
                variant="primary"
                icon={<CheckCheck size={12} />}
                onClick={() => alerts.acknowledgeAll(filtered.map((a) => a.id))}
              >
                Ack {filtered.length}
              </TactileButton>
            )}
            {tab === "acknowledged" && alerts.counts.acked > 0 && (
              <TactileButton
                variant="secondary"
                icon={<Eraser size={12} />}
                onClick={alerts.clearAcks}
              >
                Clear ACKs
              </TactileButton>
            )}
            {tab === "all" && (
              <TactileButton
                variant="secondary"
                icon={<Eraser size={12} />}
                onClick={alerts.clearHistory}
              >
                Clear History
              </TactileButton>
            )}
          </div>
        </div>

        {/* List */}
        <div className="border-t border-[color:var(--color-border-hair)]">
          {filtered.length === 0 ? (
            <EmptyState tab={tab} />
          ) : (
            <ul>
              {filtered.map((a) => (
                <AlertRow
                  key={a.id}
                  alert={a}
                  acked={alerts.acked.has(a.id)}
                  onAck={() => alerts.acknowledge(a.id)}
                  onUnack={() => alerts.unacknowledge(a.id)}
                />
              ))}
            </ul>
          )}
        </div>
      </GlassPanel>
    </div>
  );
}

function StatTile({
  label,
  value,
  icon,
  tone = "cyan",
  active,
  onClick,
  dim,
}: {
  label: string;
  value: number;
  icon: React.ReactNode;
  tone?: "cyan" | "danger" | "warning" | "success";
  active?: boolean;
  onClick?: () => void;
  dim?: boolean;
}) {
  const toneColor =
    tone === "danger" ? "var(--color-danger)" :
    tone === "warning" ? "var(--color-warning)" :
    tone === "success" ? "var(--color-success)" : "var(--color-cyan-300)";
  return (
    <button
      onClick={onClick}
      className={cn(
        "glass-raised rounded-lg px-4 py-3 text-left transition-all",
        "hover:border-[color:var(--color-border-strong)]",
        active && "ring-1 ring-[color:var(--color-cyan-500)]/50",
        dim && "opacity-40",
      )}
      style={active ? { boxShadow: "0 0 24px rgba(0,212,255,0.2)" } : undefined}
    >
      <div className="flex items-center justify-between mb-1">
        <span className="text-[10px] font-medium tracking-[0.22em] uppercase text-[color:var(--color-text-secondary)]">
          {label}
        </span>
        <span style={{ color: toneColor }}>{icon}</span>
      </div>
      <div
        className="font-display text-2xl font-bold tabular"
        style={{ color: toneColor }}
      >
        {formatNumber(value)}
      </div>
    </button>
  );
}

function AlertRow({
  alert,
  acked,
  onAck,
  onUnack,
}: {
  alert: AlertEvent;
  acked: boolean;
  onAck: () => void;
  onUnack: () => void;
}) {
  const meta = SEVERITY_META[alert.severity];
  const Icon = meta.Icon;
  const ts = new Date(alert.t);
  const timeStr = ts.toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
  const dateStr = ts.toLocaleDateString([], { month: "short", day: "numeric" });

  return (
    <li
      className={cn(
        "grid grid-cols-[auto_auto_auto_1fr_auto] items-center gap-4 px-4 py-2.5",
        "border-b border-[color:var(--color-border-hair)] last:border-b-0",
        "hover:bg-white/[0.02] transition-colors",
        acked && "opacity-50",
      )}
    >
      <div
        className="flex items-center gap-2 min-w-[92px]"
        style={{ color: meta.color }}
      >
        <Icon
          size={14}
          strokeWidth={2}
          style={{ filter: `drop-shadow(0 0 4px ${meta.glow})` }}
        />
        <span className="text-[10px] font-semibold tracking-[0.18em] uppercase">
          {meta.label}
        </span>
      </div>

      <div className="text-[11px] font-mono tabular text-[color:var(--color-text-muted)]">
        <span className="text-[color:var(--color-text-primary)]">{timeStr}</span>
        <span className="ml-2 opacity-60">{dateStr}</span>
      </div>

      <span
        className="text-[10px] font-semibold tracking-[0.18em] uppercase min-w-[72px]"
        style={{ color: meta.color, opacity: 0.9 }}
      >
        [{alert.module}]
      </span>

      <span className="text-[12px] text-[color:var(--color-text-primary)] truncate">
        {alert.message}
      </span>

      {acked ? (
        <button
          onClick={onUnack}
          className="flex items-center gap-1 px-2 py-1 rounded text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04]"
          title="Unacknowledge"
        >
          <X size={11} /> Unack
        </button>
      ) : (
        <button
          onClick={onAck}
          className="flex items-center gap-1 px-2 py-1 rounded text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-cyan-300)] hover:bg-[color:var(--color-cyan-900)]/40 hover:text-[color:var(--color-cyan-300)] border border-[color:var(--color-border-cyan)]/60"
          title="Acknowledge"
        >
          <Check size={11} /> Ack
        </button>
      )}
    </li>
  );
}

function EmptyState({ tab }: { tab: Tab }) {
  const msg =
    tab === "active" ? "No active alerts — all clear."
    : tab === "acknowledged" ? "Nothing acknowledged yet."
    : "No alerts in this view.";
  return (
    <div className="flex flex-col items-center justify-center py-14 text-center gap-2">
      <div
        className="w-10 h-10 rounded-full glass glass-cyan flex items-center justify-center"
        style={{ boxShadow: "0 0 20px rgba(0, 212, 255, 0.25)" }}
      >
        <BellRing size={18} className="text-[color:var(--color-cyan-300)]" />
      </div>
      <div className="text-[11px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
        {msg}
      </div>
    </div>
  );
}
