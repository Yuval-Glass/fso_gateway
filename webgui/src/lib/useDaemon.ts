"use client";

import { useCallback, useEffect, useRef, useState } from "react";

export type DaemonState = "stopped" | "starting" | "running" | "stopping" | "failed";

export interface DaemonConfigSummary {
  lanIface: string;
  fsoIface: string;
  k: number;
  m: number;
  depth: number;
  symbolSize: number;
}

export interface DaemonStatus {
  state: DaemonState;
  pid: number | null;
  startedAt: number | null;
  uptimeSec: number | null;
  binary: string;
  binaryFound: boolean;
  useSudo: boolean;
  logFile: string;
  command: string | null;
  config: DaemonConfigSummary | null;
  lastError: string | null;
}

export interface DaemonView {
  status: DaemonStatus | null;
  loading: boolean;
  error: string | null;
  start: () => Promise<DaemonStatus | null>;
  stop: () => Promise<DaemonStatus | null>;
  restart: () => Promise<DaemonStatus | null>;
  refresh: () => Promise<void>;
}

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

const POLL_MS = 2000;

export function useDaemon(): DaemonView {
  const [status, setStatus] = useState<DaemonStatus | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(`${bridgeBase()}/api/daemon`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = (await res.json()) as DaemonStatus;
      if (!mounted.current) return;
      setStatus(body);
      setError(null);
    } catch (e) {
      if (!mounted.current) return;
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, []);

  useEffect(() => {
    mounted.current = true;
    refresh();
    const id = setInterval(refresh, POLL_MS);
    return () => {
      mounted.current = false;
      clearInterval(id);
    };
  }, [refresh]);

  const action = useCallback(
    async (path: "start" | "stop" | "restart"): Promise<DaemonStatus | null> => {
      try {
        const res = await fetch(`${bridgeBase()}/api/daemon/${path}`, { method: "POST" });
        const body = await res.json().catch(() => ({}));
        if (!res.ok) throw new Error(body?.detail || `HTTP ${res.status}`);
        if (mounted.current) {
          setStatus(body as DaemonStatus);
          setError(null);
        }
        return body as DaemonStatus;
      } catch (e) {
        if (mounted.current) setError(e instanceof Error ? e.message : String(e));
        return null;
      }
    },
    [],
  );

  const start = useCallback(() => action("start"), [action]);
  const stop = useCallback(() => action("stop"), [action]);
  const restart = useCallback(() => action("restart"), [action]);

  return { status, loading, error, start, stop, restart, refresh };
}
