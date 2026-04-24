"use client";

import { Download, Eraser, Pause, Play, Search, X } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { TactileButton } from "@/components/primitives/TactileButton";
import { cn, formatNumber } from "@/lib/utils";
import { useLogs } from "@/lib/useLogs";
import type { LogEvent, LogLevel } from "@/types/logs";

const LEVEL_ORDER: LogLevel[] = ["DEBUG", "INFO", "WARN", "ERROR"];

const LEVEL_COLORS: Record<LogLevel, { color: string; label: string }> = {
  DEBUG: { color: "#8da2c2", label: "DEBUG" },
  INFO:  { color: "#4de3ff", label: "INFO"  },
  WARN:  { color: "#ffb020", label: "WARN"  },
  ERROR: { color: "#ff2d5c", label: "ERROR" },
};

const MODULE_PALETTE = [
  "#4de3ff", "#5aa0ff", "#a78bfa", "#34d399", "#ffb020",
  "#f472b6", "#60a5fa", "#22d3ee", "#fb923c", "#94a3b8",
];

function moduleColor(mod: string): string {
  // Stable hash → palette index
  let h = 0;
  for (let i = 0; i < mod.length; i++) h = (h * 31 + mod.charCodeAt(i)) >>> 0;
  return MODULE_PALETTE[h % MODULE_PALETTE.length];
}

export function LogConsole({ bufferSize = 1000 }: { bufferSize?: number }) {
  const logs = useLogs(bufferSize);
  const [autoScroll, setAutoScroll] = useState(true);
  const scrollRef = useRef<HTMLDivElement | null>(null);
  const endRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!autoScroll || logs.paused) return;
    const el = endRef.current;
    if (el) el.scrollIntoView({ behavior: "instant" as ScrollBehavior, block: "end" });
  }, [logs.events, autoScroll, logs.paused]);

  const handleScroll = () => {
    const el = scrollRef.current;
    if (!el) return;
    const nearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
    if (autoScroll && !nearBottom) setAutoScroll(false);
    else if (!autoScroll && nearBottom) setAutoScroll(true);
  };

  const download = () => {
    const text = logs.exportText();
    const blob = new Blob([text], { type: "text/plain" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `fso_gateway_${new Date().toISOString().replace(/[:.]/g, "-")}.log`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const sourceLabel =
    logs.mode === "tail" ? "TAILING FILE" :
    logs.mode === "mock" ? "MOCK SOURCE" : "IDLE";
  const sourceColor =
    logs.mode === "tail" ? "var(--color-success)" :
    logs.mode === "mock" ? "var(--color-cyan-500)" : "var(--color-text-muted)";

  return (
    <GlassPanel
      label="System Log Console"
      padded={false}
      trailing={
        <div className="flex items-center gap-3 text-[10px] tracking-[0.18em] uppercase">
          <span className="flex items-center gap-1.5">
            <span
              className="w-1.5 h-1.5 rounded-full breathe"
              style={{ background: sourceColor, boxShadow: `0 0 6px ${sourceColor}` }}
            />
            <span style={{ color: sourceColor }}>{sourceLabel}</span>
          </span>
          <span className="text-[color:var(--color-text-muted)]">
            {logs.connected ? "CONNECTED" : "DISCONNECTED"}
          </span>
          <span className="text-[color:var(--color-text-muted)]">
            {formatNumber(logs.events.length)} / {formatNumber(logs.allCount)}
          </span>
        </div>
      }
    >
      {/* Toolbar */}
      <div className="px-4 py-3 border-b border-[color:var(--color-border-hair)] flex flex-wrap gap-3 items-center">
        <LevelFilter logs={logs} />
        <ModuleFilter logs={logs} />
        <SearchBox logs={logs} />
        <div className="ml-auto flex items-center gap-1.5">
          <TactileButton
            variant={logs.paused ? "primary" : "secondary"}
            icon={logs.paused ? <Play size={12} /> : <Pause size={12} />}
            onClick={() => logs.pause(!logs.paused)}
          >
            {logs.paused ? "Resume" : "Pause"}
          </TactileButton>
          <TactileButton variant="secondary" icon={<Eraser size={12} />} onClick={logs.clear}>
            Clear
          </TactileButton>
          <TactileButton variant="secondary" icon={<Download size={12} />} onClick={download}>
            Export
          </TactileButton>
        </div>
      </div>

      {/* Console body */}
      <div
        ref={scrollRef}
        onScroll={handleScroll}
        className="relative max-h-[600px] min-h-[400px] overflow-y-auto font-mono text-[11.5px] leading-[1.55] px-4 py-3 bg-black/25"
        style={{
          backgroundImage:
            "repeating-linear-gradient(to bottom, transparent 0, transparent 19px, rgba(255,255,255,0.015) 19px, rgba(255,255,255,0.015) 20px)",
        }}
      >
        {logs.events.length === 0 ? (
          <div className="flex items-center justify-center h-[360px] text-[color:var(--color-text-muted)] text-xs tracking-[0.2em] uppercase">
            {logs.connected ? "Waiting for events…" : "Connecting to bridge…"}
          </div>
        ) : (
          logs.events.map((e, i) => <LogRow key={i} e={e} />)
        )}
        <div ref={endRef} />

        {logs.paused && (
          <div
            className="sticky bottom-2 mx-auto w-fit px-3 py-1 rounded-md border border-[color:var(--color-warning)]/50 bg-[color:var(--color-warning)]/10 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-warning)]"
          >
            Paused — new events buffering
          </div>
        )}
        {!autoScroll && !logs.paused && (
          <button
            onClick={() => { setAutoScroll(true); endRef.current?.scrollIntoView(); }}
            className="sticky bottom-2 ml-auto block px-3 py-1 rounded-md border border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40 text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-cyan-300)] hover:bg-[color:var(--color-cyan-900)]/70"
          >
            Jump to Latest
          </button>
        )}
      </div>
    </GlassPanel>
  );
}

function LogRow({ e }: { e: LogEvent }) {
  const lvl = LEVEL_COLORS[e.level];
  const modColor = moduleColor(e.module);
  const t = new Date(e.ts_ms).toISOString().slice(11, 23);
  const isError = e.level === "ERROR";
  const isWarn = e.level === "WARN";
  return (
    <div
      className={cn(
        "grid grid-cols-[auto_auto_auto_1fr] gap-x-3 px-1 -mx-1 rounded hover:bg-white/[0.03] transition-colors",
        isError && "bg-[color:var(--color-danger)]/5",
        isWarn && "bg-[color:var(--color-warning)]/5",
      )}
    >
      <span className="text-[color:var(--color-text-muted)] tabular">{t}</span>
      <span
        className="font-semibold tracking-[0.1em]"
        style={{ color: lvl.color }}
      >
        {lvl.label.padEnd(5, " ")}
      </span>
      <span
        className="font-semibold tracking-[0.1em]"
        style={{ color: modColor }}
      >
        [{e.module}]
      </span>
      <span className="text-[color:var(--color-text-primary)] break-words">
        {e.message}
      </span>
    </div>
  );
}

function LevelFilter({ logs }: { logs: ReturnType<typeof useLogs> }) {
  return (
    <div className="flex items-center gap-1">
      <span className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mr-1">Level</span>
      {LEVEL_ORDER.map((l) => {
        const active = logs.filter.levels[l];
        const c = LEVEL_COLORS[l];
        return (
          <button
            key={l}
            onClick={() => logs.toggleLevel(l)}
            className={cn(
              "px-2 py-0.5 rounded border text-[10px] font-semibold tracking-[0.15em] uppercase transition-colors",
              active
                ? "border-transparent"
                : "border-[color:var(--color-border-hair)] opacity-40 hover:opacity-70",
            )}
            style={
              active
                ? {
                    borderColor: `${c.color}66`,
                    background: `${c.color}12`,
                    color: c.color,
                  }
                : undefined
            }
            title={active ? `Hide ${l}` : `Show ${l}`}
          >
            {c.label}
          </button>
        );
      })}
    </div>
  );
}

function ModuleFilter({ logs }: { logs: ReturnType<typeof useLogs> }) {
  const [open, setOpen] = useState(false);
  const activeCount = Object.values(logs.filter.modules).filter(Boolean).length;
  return (
    <div className="relative">
      <button
        onClick={() => setOpen((o) => !o)}
        className="px-2 py-1 rounded border border-[color:var(--color-border-subtle)] text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.04]"
      >
        Modules{activeCount > 0 ? ` · ${activeCount}` : ""}
      </button>
      {open && (
        <div className="absolute z-20 top-full mt-1 left-0 w-60 glass-raised rounded-md p-2 space-y-0.5 max-h-72 overflow-y-auto">
          {logs.knownModules.length === 0 ? (
            <div className="text-[10px] text-[color:var(--color-text-muted)] px-2 py-1">
              no modules seen yet
            </div>
          ) : (
            logs.knownModules.map((m) => {
              const c = moduleColor(m);
              const checked = !!logs.filter.modules[m];
              return (
                <label
                  key={m}
                  className="flex items-center gap-2 px-2 py-1 text-xs rounded hover:bg-white/[0.04] cursor-pointer"
                >
                  <input
                    type="checkbox"
                    checked={checked}
                    onChange={() => logs.toggleModule(m)}
                    className="accent-[color:var(--color-cyan-500)]"
                  />
                  <span
                    className="font-semibold tracking-[0.1em]"
                    style={{ color: c }}
                  >
                    {m}
                  </span>
                </label>
              );
            })
          )}
          {activeCount > 0 && (
            <button
              onClick={() => logs.setFilter({ modules: Object.fromEntries(Object.keys(logs.filter.modules).map((k) => [k, false])) })}
              className="w-full mt-1 text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)] py-1"
            >
              Clear selection
            </button>
          )}
        </div>
      )}
    </div>
  );
}

function SearchBox({ logs }: { logs: ReturnType<typeof useLogs> }) {
  return (
    <div className="relative">
      <Search
        size={12}
        className="absolute left-2 top-1/2 -translate-y-1/2 text-[color:var(--color-text-muted)]"
      />
      <input
        type="text"
        value={logs.filter.search}
        onChange={(e) => logs.setSearch(e.target.value)}
        placeholder="Search messages…"
        className="pl-7 pr-7 py-1 w-56 rounded-md border border-[color:var(--color-border-subtle)] bg-white/[0.02] text-xs text-[color:var(--color-text-primary)] outline-none focus:border-[color:var(--color-cyan-500)] focus:ring-1 focus:ring-[color:var(--color-cyan-500)]/40 placeholder:text-[color:var(--color-text-muted)]"
      />
      {logs.filter.search && (
        <button
          onClick={() => logs.setSearch("")}
          className="absolute right-1.5 top-1/2 -translate-y-1/2 text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)]"
          aria-label="Clear search"
        >
          <X size={12} />
        </button>
      )}
    </div>
  );
}
