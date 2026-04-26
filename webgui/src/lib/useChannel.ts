"use client";

import { useCallback, useEffect, useRef, useState } from "react";

export interface NetemState {
  iface: string;
  available: boolean;
  active: boolean;
  raw: string;
  model: "gemodel" | "uniform" | null;
  lossPct: number | null;
  enterPct: number | null;
  exitPct: number | null;
  error?: string;
}

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

export interface ChannelView {
  state: NetemState | null;
  loading: boolean;
  error: string | null;
  iface: string;
  setIface: (s: string) => void;
  refresh: () => Promise<void>;
  apply: (enterPct: number, exitPct: number, lossPct: number) => Promise<boolean>;
  clear: () => Promise<boolean>;
}

const POLL_MS = 5000;

export function useChannel(initialIface = "enp1s0f1np1"): ChannelView {
  const [state, setState] = useState<NetemState | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [iface, setIface] = useState(initialIface);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(`${bridgeBase()}/api/channel/netem?iface=${encodeURIComponent(iface)}`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = (await res.json()) as NetemState;
      if (!mounted.current) return;
      setState(body);
      setError(null);
    } catch (e) {
      if (!mounted.current) return;
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, [iface]);

  useEffect(() => {
    mounted.current = true;
    refresh();
    const id = setInterval(refresh, POLL_MS);
    return () => {
      mounted.current = false;
      clearInterval(id);
    };
  }, [refresh]);

  const apply = useCallback(
    async (enterPct: number, exitPct: number, lossPct: number) => {
      try {
        const res = await fetch(`${bridgeBase()}/api/channel/netem`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ iface, enterPct, exitPct, lossPct }),
        });
        const body = await res.json().catch(() => ({}));
        if (!res.ok) throw new Error(body?.detail || `HTTP ${res.status}`);
        await refresh();
        return body?.ok !== false;
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        return false;
      }
    },
    [iface, refresh],
  );

  const clear = useCallback(async () => {
    try {
      const res = await fetch(`${bridgeBase()}/api/channel/netem`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ iface, clear: true }),
      });
      if (!res.ok) {
        const body = await res.json().catch(() => ({}));
        throw new Error(body?.detail || `HTTP ${res.status}`);
      }
      await refresh();
      return true;
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
      return false;
    }
  }, [iface, refresh]);

  return { state, loading, error, iface, setIface, refresh, apply, clear };
}
