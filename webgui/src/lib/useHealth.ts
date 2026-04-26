"use client";

import { useEffect, useState } from "react";

export interface BridgeHealth {
  status: string;
  source: "gateway" | "mock" | string;
  gateway_socket: string;
  logs_mode: "tail" | "mock" | "idle" | string;
  active_run_id: number | null;
  uptime_sec: number;
  tick_hz: number;
}

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

export function useHealth(): BridgeHealth | null {
  const [data, setData] = useState<BridgeHealth | null>(null);

  useEffect(() => {
    let cancelled = false;
    const fetchOnce = async () => {
      try {
        const res = await fetch(`${bridgeBase()}/health`, { cache: "no-store" });
        if (!res.ok) return;
        const body = (await res.json()) as BridgeHealth;
        if (!cancelled) setData(body);
      } catch {
        // ignore — bridge may be down; UI will show fallback values
      }
    };
    fetchOnce();
    const id = setInterval(fetchOnce, 5000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  return data;
}
