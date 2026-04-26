"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { LogEvent, LogLevel, LogsMode, LogWireFrame } from "@/types/logs";

const BRIDGE_LOGS_URL = (() => {
  const override = process.env.NEXT_PUBLIC_BRIDGE_LOGS_WS;
  if (override) return override;
  return null; // resolved per-client using window.location
})();

function wsUrl(): string {
  if (BRIDGE_LOGS_URL) return BRIDGE_LOGS_URL;
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `ws://${host}:8000/ws/logs`;
}

const RECONNECT_DELAY_MS = 3000;
const DEFAULT_BUFFER = 1000;

export interface LogsFilter {
  levels: Record<LogLevel, boolean>;
  modules: Record<string, boolean>; // empty ⇒ all allowed
  search: string;
}

export interface LogsState {
  events: LogEvent[];          // filtered, chronological (oldest first)
  allCount: number;            // total unfiltered count in the rolling buffer
  mode: LogsMode;              // server-reported source
  connected: boolean;
  paused: boolean;
  filter: LogsFilter;
  knownModules: string[];
  setFilter: (f: Partial<LogsFilter>) => void;
  toggleLevel: (l: LogLevel) => void;
  toggleModule: (m: string) => void;
  setSearch: (s: string) => void;
  clear: () => void;
  pause: (p: boolean) => void;
  exportText: () => string;
}

const DEFAULT_FILTER: LogsFilter = {
  levels: { DEBUG: false, INFO: true, WARN: true, ERROR: true },
  modules: {},
  search: "",
};

export function useLogs(bufferSize: number = DEFAULT_BUFFER): LogsState {
  const [events, setEvents] = useState<LogEvent[]>([]);
  const [mode, setMode] = useState<LogsMode>("idle");
  const [connected, setConnected] = useState(false);
  const [paused, setPaused] = useState(false);
  const [filter, setFilterState] = useState<LogsFilter>(DEFAULT_FILTER);

  const pausedRef = useRef(paused);
  pausedRef.current = paused;
  const pausedBuffer = useRef<LogEvent[]>([]);

  useEffect(() => {
    let ws: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let cancelled = false;

    const connect = () => {
      if (cancelled) return;
      let sock: WebSocket;
      try {
        sock = new WebSocket(wsUrl());
      } catch {
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
        return;
      }
      ws = sock;

      sock.onopen = () => {
        if (cancelled) return;
        setConnected(true);
      };

      sock.onmessage = (ev) => {
        let parsed: LogWireFrame;
        try {
          parsed = JSON.parse(ev.data) as LogWireFrame;
        } catch {
          return;
        }
        if (parsed.type === "meta") {
          if (parsed.mode) setMode(parsed.mode);
          return;
        }
        if (parsed.type !== "event" || parsed.ts_ms === undefined) return;

        const e: LogEvent = {
          ts_ms: parsed.ts_ms,
          level: (parsed.level ?? "INFO") as LogLevel,
          module: parsed.module ?? "SYSTEM",
          message: parsed.message ?? "",
          raw: parsed.raw ?? "",
        };

        if (pausedRef.current) {
          pausedBuffer.current.push(e);
          // Keep paused buffer bounded too; drop oldest if over-large.
          if (pausedBuffer.current.length > bufferSize) {
            pausedBuffer.current.shift();
          }
          return;
        }

        setEvents((prev) => {
          const next = prev.length >= bufferSize
            ? prev.slice(prev.length - bufferSize + 1)
            : prev.slice();
          next.push(e);
          return next;
        });
      };

      const handleLost = () => {
        ws = null;
        if (cancelled) return;
        setConnected(false);
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
      };

      sock.onclose = handleLost;
      sock.onerror = () => { /* onclose will follow */ };
    };

    connect();

    return () => {
      cancelled = true;
      if (ws) {
        ws.onclose = null;
        ws.onerror = null;
        ws.onmessage = null;
        ws.onopen = null;
        try { ws.close(); } catch {}
      }
      if (reconnectTimer) clearTimeout(reconnectTimer);
    };
  }, [bufferSize]);

  // When unpausing, flush the paused buffer into events.
  useEffect(() => {
    if (!paused && pausedBuffer.current.length > 0) {
      const pending = pausedBuffer.current;
      pausedBuffer.current = [];
      setEvents((prev) => {
        const merged = prev.concat(pending);
        return merged.length > bufferSize
          ? merged.slice(merged.length - bufferSize)
          : merged;
      });
    }
  }, [paused, bufferSize]);

  const knownModules = useMemo(() => {
    const set = new Set<string>();
    for (const e of events) set.add(e.module);
    return Array.from(set).sort();
  }, [events]);

  const filtered = useMemo(() => {
    const needle = filter.search.trim().toLowerCase();
    const modulesSelected = Object.keys(filter.modules).filter((k) => filter.modules[k]);
    const modFilterOn = modulesSelected.length > 0;
    return events.filter((e) => {
      if (!filter.levels[e.level]) return false;
      if (modFilterOn && !filter.modules[e.module]) return false;
      if (needle) {
        const hay = `${e.module} ${e.message} ${e.raw}`.toLowerCase();
        if (!hay.includes(needle)) return false;
      }
      return true;
    });
  }, [events, filter]);

  const setFilter = useCallback((f: Partial<LogsFilter>) => {
    setFilterState((prev) => ({ ...prev, ...f, levels: { ...prev.levels, ...(f.levels ?? {}) }, modules: { ...prev.modules, ...(f.modules ?? {}) } }));
  }, []);

  const toggleLevel = useCallback((l: LogLevel) => {
    setFilterState((prev) => ({ ...prev, levels: { ...prev.levels, [l]: !prev.levels[l] } }));
  }, []);

  const toggleModule = useCallback((m: string) => {
    setFilterState((prev) => ({ ...prev, modules: { ...prev.modules, [m]: !prev.modules[m] } }));
  }, []);

  const setSearch = useCallback((s: string) => {
    setFilterState((prev) => ({ ...prev, search: s }));
  }, []);

  const clear = useCallback(() => {
    setEvents([]);
    pausedBuffer.current = [];
  }, []);

  const exportText = useCallback(() => {
    return filtered.map((e) => {
      const t = new Date(e.ts_ms).toISOString();
      return `${t} [${e.level}] [${e.module}] ${e.message}`;
    }).join("\n");
  }, [filtered]);

  return {
    events: filtered,
    allCount: events.length,
    mode,
    connected,
    paused,
    filter,
    knownModules,
    setFilter,
    toggleLevel,
    toggleModule,
    setSearch,
    clear,
    pause: setPaused,
    exportText,
  };
}
