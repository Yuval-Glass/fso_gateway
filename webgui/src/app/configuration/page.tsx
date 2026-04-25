"use client";

import { AlertTriangle, Check, Play, RotateCcw, Save, Square, Undo2 } from "lucide-react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { Slider } from "@/components/primitives/Slider";
import { TactileButton } from "@/components/primitives/TactileButton";
import { TextField } from "@/components/primitives/TextField";
import { Toggle } from "@/components/primitives/Toggle";
import { FieldHint } from "@/components/primitives/FieldHint";
import { useConfig } from "@/lib/useConfig";
import { useDaemon, type DaemonStatus } from "@/lib/useDaemon";
import { CONFIG_BOUNDS, CONFIG_PRESETS, type ConfigPreset } from "@/types/config";
import { cn, formatBytes, formatPercent, formatUptime } from "@/lib/utils";

export default function ConfigurationPage() {
  const cfg = useConfig();
  const daemon = useDaemon();

  if (cfg.status === "loading" || !cfg.draft) {
    return (
      <div className="flex items-center justify-center h-[60vh]">
        <div className="text-xs tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)] breathe">
          {cfg.status === "error" ? "Unable to reach bridge" : "Loading configuration…"}
        </div>
      </div>
    );
  }

  const d = cfg.draft;
  const k = d.k;
  const m = d.m;
  const depth = d.depth;
  const symSize = d.symbol_size;
  const total = k + m;
  const overhead = total > 0 ? m / total : 0;
  const codeRate = total > 0 ? k / total : 0;
  const blockBytes = total * symSize;
  const approxBurstRec = m * depth;

  return (
    <div className="flex flex-col gap-5">
      {/* Header */}
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Control Center
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Tune live gateway parameters. Changes persist immediately; restart the
            daemon to apply.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 3A · Config Draft
        </div>
      </div>

      <div className="grid grid-cols-1 xl:grid-cols-[2fr_1fr] gap-4">
        {/* LEFT: editable form */}
        <div className="flex flex-col gap-4">
          <GlassPanel label="FEC Parameters">
            <div className="grid grid-cols-2 gap-6 pt-1">
              <Slider
                label="K — Source Symbols"
                value={k}
                min={CONFIG_BOUNDS.k.min}
                max={CONFIG_BOUNDS.k.max}
                onChange={(v) => cfg.update("k", v)}
                onCommit={(v) => cfg.save({ k: v })}
                unit="symbols"
                hint="data per block"
                hintId="config.k"
              />
              <Slider
                label="M — Repair Symbols"
                value={m}
                min={CONFIG_BOUNDS.m.min}
                max={CONFIG_BOUNDS.m.max}
                onChange={(v) => cfg.update("m", v)}
                onCommit={(v) => cfg.save({ m: v })}
                unit="symbols"
                hint="redundancy"
                hintId="config.m"
              />
            </div>
            <div className="mt-5 grid grid-cols-4 gap-2">
              <Derived label="Overhead" value={formatPercent(overhead, 1)} tone="cyan" hintId="config.overhead" />
              <Derived label="Code Rate" value={codeRate.toFixed(3)} hintId="config.codeRate" />
              <Derived label="Block Size" value={formatBytes(blockBytes, 1)} hintId="config.blockSize" />
              <Derived label="Burst Recovery ~" value={`${approxBurstRec} sym`} tone="success" hintId="config.burstRecovery" />
            </div>
          </GlassPanel>

          <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
            <GlassPanel label="Interleaver">
              <div className="pt-1 pb-2">
                <Slider
                  label="Depth (rows)"
                  value={depth}
                  min={CONFIG_BOUNDS.depth.min}
                  max={CONFIG_BOUNDS.depth.max}
                  onChange={(v) => cfg.update("depth", v)}
                  onCommit={(v) => cfg.save({ depth: v })}
                  unit="rows"
                  hint="spread across blocks"
                  hintId="config.depth"
                />
              </div>
            </GlassPanel>
            <GlassPanel label="Symbol">
              <div className="pt-1 pb-2">
                <Slider
                  label="Symbol Size"
                  value={symSize}
                  min={CONFIG_BOUNDS.symbol_size.min}
                  max={CONFIG_BOUNDS.symbol_size.max}
                  step={1}
                  onChange={(v) => cfg.update("symbol_size", v)}
                  onCommit={(v) => cfg.save({ symbol_size: v })}
                  unit="bytes"
                  hint="payload per symbol"
                  hintId="config.symbolSize"
                />
              </div>
            </GlassPanel>
          </div>

          <GlassPanel label="Network Interfaces">
            <div className="grid grid-cols-1 md:grid-cols-2 gap-4 pt-1">
              <TextField
                label="LAN Interface"
                value={d.lan_iface}
                onChange={(v) => cfg.update("lan_iface", v)}
                onCommit={(v) => cfg.save({ lan_iface: v })}
                placeholder="eth0"
                maxLength={31}
                mono
                hintId="config.lanIface"
              />
              <TextField
                label="FSO Interface"
                value={d.fso_iface}
                onChange={(v) => cfg.update("fso_iface", v)}
                onCommit={(v) => cfg.save({ fso_iface: v })}
                placeholder="eth1"
                maxLength={31}
                mono
                hintId="config.fsoIface"
              />
            </div>
          </GlassPanel>

          <GlassPanel label="Symbol Integrity">
            <div className="pt-1">
              <Toggle
                label="Internal Symbol CRC-32C"
                checked={d.internal_symbol_crc}
                onChange={(v) => cfg.update("internal_symbol_crc", v)}
                description="Per-symbol CRC-32C (Castagnoli) — invalid symbols are dropped on RX and treated as erasures by the FEC layer."
                hintId="config.internalSymbolCrc"
              />
            </div>
          </GlassPanel>

          <RuntimeControlsPanel daemon={daemon} />
        </div>

        {/* RIGHT: presets + action bar */}
        <div className="flex flex-col gap-4">
          <GlassPanel label="Presets" variant="cyan">
            <ul className="space-y-2 py-1">
              {CONFIG_PRESETS.map((p) => {
                const active = isActivePreset(p, d);
                return (
                  <li key={p.id}>
                    <button
                      type="button"
                      onClick={() => cfg.setDraft(p.config)}
                      className={cn(
                        "w-full text-left px-3 py-2 rounded-md border transition-all",
                        active
                          ? "border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40"
                          : "border-[color:var(--color-border-subtle)] bg-white/[0.02] hover:bg-white/[0.04] hover:border-[color:var(--color-border-strong)]",
                      )}
                    >
                      <div className="flex items-center justify-between">
                        <span className="text-xs font-semibold tracking-[0.12em] uppercase text-[color:var(--color-text-primary)]">
                          {p.name}
                        </span>
                        {active && (
                          <Check size={12} className="text-[color:var(--color-cyan-300)]" />
                        )}
                      </div>
                      <div className="text-[10px] text-[color:var(--color-text-secondary)] mt-0.5">
                        {p.description}
                      </div>
                      <div className="mt-1 text-[9px] font-mono text-[color:var(--color-text-muted)]">
                        K={p.config.k} · M={p.config.m} · depth={p.config.depth} · sym={p.config.symbol_size}
                      </div>
                    </button>
                  </li>
                );
              })}
            </ul>
          </GlassPanel>

          <GlassPanel label="Action">
            <div className="flex flex-col gap-3 pt-1">
              <div className="flex items-center gap-2">
                <StatusDotFor cfg={cfg} />
                <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)]">
                  {cfg.status === "saving"
                    ? "Saving…"
                    : cfg.error
                    ? "Error"
                    : cfg.dirty
                    ? "Unsaved changes"
                    : cfg.requiresRestart
                    ? "Saved — restart to apply"
                    : "In sync with bridge"}
                </span>
                <FieldHint id="config.statusDot" size={11} />
              </div>
              {cfg.error && (
                <div className="flex items-start gap-2 px-3 py-2 rounded-md border border-[color:var(--color-border-danger)] bg-[color:var(--color-danger)]/10 text-[11px] text-[color:var(--color-danger)]">
                  <AlertTriangle size={13} className="mt-0.5 shrink-0" />
                  <span>{cfg.error}</span>
                </div>
              )}
              <div className="flex gap-2 items-center">
                <TactileButton
                  variant="primary"
                  icon={<Save size={13} />}
                  onClick={() => cfg.save()}
                  loading={cfg.status === "saving"}
                  disabled={!cfg.dirty}
                  className="flex-1"
                >
                  Apply Changes
                </TactileButton>
                <FieldHint id="config.applyBtn" size={11} />
                <TactileButton
                  variant="secondary"
                  icon={<Undo2 size={13} />}
                  onClick={() => cfg.revert()}
                  disabled={!cfg.dirty}
                >
                  Revert
                </TactileButton>
                <FieldHint id="config.revertBtn" size={11} />
              </div>
              <div className="text-[10px] text-[color:var(--color-text-muted)] leading-snug">
                Saving persists to <span className="font-mono">webgui/server/config.yaml</span>.
                The running daemon picks it up on the next launch (manual for now).
              </div>
            </div>
          </GlassPanel>

          <GlassPanel label="Live Derived">
            <ul className="text-[11px] space-y-1.5 py-1">
              <KV label="FEC Overhead"           value={formatPercent(overhead, 2)} tone="cyan" hintId="config.overhead" />
              <KV label="Code Rate (k/(k+m))"    value={codeRate.toFixed(4)} hintId="config.codeRate" />
              <KV label="Symbols per Block"      value={`${total}`} hintId="interleaver.blockWidth" />
              <KV label="Block Payload"          value={formatBytes(k * symSize, 1)} />
              <KV label="Block Wire Size"        value={formatBytes(blockBytes, 1)} hintId="config.blockSize" />
              <KV label="Interleaver Window"     value={`${depth} × ${total} = ${depth * total} sym`} hintId="interleaver.matrix" />
              <KV label="Approx. Burst Recovery" value={`${approxBurstRec} symbols`} tone="success" hintId="config.burstRecovery" />
            </ul>
          </GlassPanel>
        </div>
      </div>
    </div>
  );
}

function isActivePreset(p: ConfigPreset, d: ReturnType<typeof useConfig>["draft"]) {
  if (!d) return false;
  const c = p.config;
  if (c.k !== undefined && c.k !== d.k) return false;
  if (c.m !== undefined && c.m !== d.m) return false;
  if (c.depth !== undefined && c.depth !== d.depth) return false;
  if (c.symbol_size !== undefined && c.symbol_size !== d.symbol_size) return false;
  return true;
}

function Derived({
  label,
  value,
  tone = "neutral",
  hintId,
}: {
  label: string;
  value: string;
  tone?: "neutral" | "cyan" | "success";
  hintId?: import("@/lib/fieldHints").FieldHintId;
}) {
  const color =
    tone === "cyan"
      ? "var(--color-cyan-300)"
      : tone === "success"
      ? "var(--color-success)"
      : "var(--color-text-primary)";
  return (
    <div className="glass rounded-md px-3 py-2 border-[color:var(--color-border-hair)]">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </div>
      <div className="font-display text-base font-semibold tabular mt-0.5" style={{ color }}>
        {value}
      </div>
    </div>
  );
}

function KV({
  label,
  value,
  tone = "neutral",
  hintId,
}: {
  label: string;
  value: string;
  tone?: "neutral" | "cyan" | "success";
  hintId?: import("@/lib/fieldHints").FieldHintId;
}) {
  const color =
    tone === "cyan"
      ? "var(--color-cyan-300)"
      : tone === "success"
      ? "var(--color-success)"
      : "var(--color-text-primary)";
  return (
    <li className="flex items-baseline justify-between gap-3">
      <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] inline-flex items-center gap-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={10} />}
      </span>
      <span className="font-mono tabular" style={{ color }}>
        {value}
      </span>
    </li>
  );
}

function RuntimeControlsPanel({ daemon }: { daemon: ReturnType<typeof useDaemon> }) {
  const s = daemon.status;
  const state: DaemonStatus["state"] = s?.state ?? "stopped";
  const isStarting = state === "starting";
  const isStopping = state === "stopping";
  const isRunning = state === "running";
  const isStopped = state === "stopped";
  const isFailed = state === "failed";
  const busy = isStarting || isStopping;
  const canStart = !busy && (isStopped || isFailed);
  const canStop = !busy && (isRunning || isStarting);
  const canRestart = !busy && isRunning;

  return (
    <GlassPanel
      label="Runtime Controls"
      trailing={<DaemonStateBadge status={s} />}
    >
      <div className="flex flex-col gap-3 pt-1">
        <div className="flex flex-wrap items-center gap-2">
          <span className="inline-flex items-center gap-1">
            <TactileButton
              variant="primary"
              icon={<Play size={13} />}
              onClick={() => daemon.start()}
              loading={isStarting}
              disabled={!canStart}
            >
              Start Gateway
            </TactileButton>
            <FieldHint id="daemon.startBtn" size={11} />
          </span>
          <span className="inline-flex items-center gap-1">
            <TactileButton
              variant="secondary"
              icon={<RotateCcw size={13} />}
              onClick={() => daemon.restart()}
              disabled={!canRestart}
            >
              Restart
            </TactileButton>
            <FieldHint id="daemon.restartBtn" size={11} />
          </span>
          <span className="inline-flex items-center gap-1">
            <TactileButton
              variant="danger"
              icon={<Square size={13} />}
              onClick={() => daemon.stop()}
              loading={isStopping}
              disabled={!canStop}
            >
              Stop
            </TactileButton>
            <FieldHint id="daemon.stopBtn" size={11} />
          </span>
        </div>

        {daemon.error && (
          <div className="flex items-start gap-2 px-3 py-2 rounded-md border border-[color:var(--color-border-danger)] bg-[color:var(--color-danger)]/10 text-[11px] text-[color:var(--color-danger)]">
            <AlertTriangle size={13} className="mt-0.5 shrink-0" />
            <span>{daemon.error}</span>
          </div>
        )}

        {s?.lastError && isFailed && (
          <div className="flex items-start gap-2 px-3 py-2 rounded-md border border-[color:var(--color-border-danger)] bg-[color:var(--color-danger)]/10 text-[11px] text-[color:var(--color-danger)]">
            <AlertTriangle size={13} className="mt-0.5 shrink-0" />
            <span>{s.lastError}</span>
          </div>
        )}

        {s && (
          <div className="grid grid-cols-2 md:grid-cols-4 gap-2 text-[11px]">
            <KV label="PID" value={s.pid != null ? String(s.pid) : "—"} hintId="daemon.pid" />
            <KV
              label="Uptime"
              value={s.uptimeSec != null ? formatUptime(s.uptimeSec) : "—"}
              hintId="daemon.uptime"
            />
            <KV label="Sudo" value={s.useSudo ? "yes" : "no"} hintId="daemon.sudo" />
            <KV
              label="Binary"
              value={s.binaryFound ? "found" : "missing"}
              tone={s.binaryFound ? "success" : "neutral"}
              hintId="daemon.binary"
            />
          </div>
        )}

        {s?.binary && (
          <div className="text-[10px] text-[color:var(--color-text-muted)] leading-snug font-mono break-all">
            <span className="text-[color:var(--color-text-secondary)]">binary:</span> {s.binary}
            {s.logFile && (
              <>
                <br />
                <span className="text-[color:var(--color-text-secondary)]">log:</span> {s.logFile}
              </>
            )}
          </div>
        )}
      </div>
    </GlassPanel>
  );
}

function DaemonStateBadge({ status }: { status: DaemonStatus | null }) {
  const state = status?.state ?? "stopped";
  const tone =
    state === "running" ? { bg: "var(--color-success)", glow: "rgba(52, 211, 153, 0.6)", label: "RUNNING" } :
    state === "starting" ? { bg: "var(--color-warning)", glow: "rgba(255, 176, 32, 0.6)", label: "STARTING" } :
    state === "stopping" ? { bg: "var(--color-warning)", glow: "rgba(255, 176, 32, 0.6)", label: "STOPPING" } :
    state === "failed" ? { bg: "var(--color-danger)", glow: "rgba(255, 45, 92, 0.7)", label: "FAILED" } :
    { bg: "var(--color-text-muted)", glow: "rgba(85, 96, 114, 0.4)", label: "STOPPED" };
  const breathe = state === "starting" || state === "stopping";
  return (
    <span className="inline-flex items-center gap-2">
      <span
        className={cn("w-1.5 h-1.5 rounded-full", breathe && "breathe")}
        style={{ background: tone.bg, boxShadow: `0 0 6px ${tone.glow}` }}
      />
      <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-secondary)]">
        {tone.label}
      </span>
    </span>
  );
}

function StatusDotFor({ cfg }: { cfg: ReturnType<typeof useConfig> }) {
  let color = "var(--color-text-muted)";
  let glow = "rgba(85, 96, 114, 0.3)";
  let breathe = false;
  if (cfg.error) {
    color = "var(--color-danger)";
    glow = "rgba(255, 45, 92, 0.7)";
  } else if (cfg.status === "saving") {
    color = "var(--color-warning)";
    glow = "rgba(255, 176, 32, 0.7)";
    breathe = true;
  } else if (cfg.dirty) {
    color = "var(--color-cyan-500)";
    glow = "rgba(0, 212, 255, 0.55)";
  } else if (cfg.requiresRestart) {
    color = "var(--color-warning)";
    glow = "rgba(255, 176, 32, 0.55)";
  } else {
    color = "var(--color-success)";
    glow = "rgba(52, 211, 153, 0.55)";
  }
  return (
    <span
      className={cn("w-2 h-2 rounded-full", breathe && "breathe")}
      style={{ background: color, boxShadow: `0 0 8px ${glow}` }}
    />
  );
}

