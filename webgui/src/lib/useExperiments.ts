"use client";

import { useCallback, useEffect, useRef, useState } from "react";

export interface ExperimentSummary {
  name: string;
  ts: string;            // YYYYMMDD_HHMMSS
  size: number;
  mtime: number;         // epoch ms
}

export interface ExperimentDetail extends ExperimentSummary {
  text: string;
  summary: {
    params: Record<string, string>;
    mode?: string;
    protocol?: string;
    throughput?: { value: number; unit: string };
    totalData?: { value: number; unit: string };
    lossPct?: number;
  };
}

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

const POLL_MS = 10_000;

export function useExperiments(): {
  list: ExperimentSummary[];
  loading: boolean;
  error: string | null;
  refresh: () => Promise<void>;
} {
  const [list, setList] = useState<ExperimentSummary[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(`${bridgeBase()}/api/experiments`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = await res.json();
      if (!mounted.current) return;
      setList(body.experiments ?? []);
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

  return { list, loading, error, refresh };
}

export function useExperimentDetail(name: string | null): {
  detail: ExperimentDetail | null;
  loading: boolean;
  error: string | null;
} {
  const [detail, setDetail] = useState<ExperimentDetail | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    if (!name) {
      setDetail(null);
      return;
    }
    setLoading(true);
    fetch(`${bridgeBase()}/api/experiments/${encodeURIComponent(name)}`, { cache: "no-store" })
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return res.json();
      })
      .then((body) => {
        if (!mounted.current) return;
        setDetail(body as ExperimentDetail);
        setError(null);
      })
      .catch((e) => {
        if (!mounted.current) return;
        setError(e instanceof Error ? e.message : String(e));
      })
      .finally(() => {
        if (mounted.current) setLoading(false);
      });
    return () => {
      mounted.current = false;
    };
  }, [name]);

  return { detail, loading, error };
}
