"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type {
  RunDetailResponse,
  RunListResponse,
  RunSample,
  RunSummary,
} from "@/types/runs";

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

const POLL_MS = 5000;

/* -------------------------------------------------------------------------- */
/* Runs list                                                                   */
/* -------------------------------------------------------------------------- */

export interface RunsListView {
  runs: RunSummary[];
  activeRunId: number | null;
  loading: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  startNewRun: (name?: string, notes?: string) => Promise<RunSummary | null>;
  endRun: (id: number) => Promise<boolean>;
  deleteRun: (id: number) => Promise<boolean>;
}

export function useRuns(): RunsListView {
  const [runs, setRuns] = useState<RunSummary[]>([]);
  const [activeRunId, setActiveRunId] = useState<number | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const res = await fetch(`${bridgeBase()}/api/runs`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = (await res.json()) as RunListResponse;
      if (!mounted.current) return;
      setRuns(body.runs);
      setActiveRunId(body.active_run_id);
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

  const startNewRun = useCallback(
    async (name?: string, notes?: string): Promise<RunSummary | null> => {
      try {
        const res = await fetch(`${bridgeBase()}/api/runs/new`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name: name ?? null, notes: notes ?? null }),
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const body = await res.json();
        await refresh();
        return body.run as RunSummary | null;
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        return null;
      }
    },
    [refresh],
  );

  const endRun = useCallback(
    async (id: number): Promise<boolean> => {
      try {
        const res = await fetch(`${bridgeBase()}/api/runs/${id}/end`, { method: "POST" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        await refresh();
        return true;
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        return false;
      }
    },
    [refresh],
  );

  const deleteRun = useCallback(
    async (id: number): Promise<boolean> => {
      try {
        const res = await fetch(`${bridgeBase()}/api/runs/${id}`, { method: "DELETE" });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        await refresh();
        return true;
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        return false;
      }
    },
    [refresh],
  );

  return { runs, activeRunId, loading, error, refresh, startNewRun, endRun, deleteRun };
}

/* -------------------------------------------------------------------------- */
/* Single run detail + samples                                                 */
/* -------------------------------------------------------------------------- */

export interface RunDetailView {
  detail: RunDetailResponse | null;
  samples: RunSample[];
  loading: boolean;
  error: string | null;
  refresh: () => Promise<void>;
}

export function useRunDetail(
  runId: number | null,
  opts: { pollWhileActive?: boolean; maxPoints?: number } = {},
): RunDetailView {
  const { pollWhileActive = true, maxPoints = 600 } = opts;
  const [detail, setDetail] = useState<RunDetailResponse | null>(null);
  const [samples, setSamples] = useState<RunSample[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    if (runId === null) {
      setDetail(null);
      setSamples([]);
      return;
    }
    setLoading(true);
    try {
      const [detailRes, samplesRes] = await Promise.all([
        fetch(`${bridgeBase()}/api/runs/${runId}`, { cache: "no-store" }),
        fetch(`${bridgeBase()}/api/runs/${runId}/samples?max_points=${maxPoints}`, { cache: "no-store" }),
      ]);
      if (!detailRes.ok) throw new Error(`detail HTTP ${detailRes.status}`);
      if (!samplesRes.ok) throw new Error(`samples HTTP ${samplesRes.status}`);
      const d = (await detailRes.json()) as RunDetailResponse;
      const s = (await samplesRes.json()) as { runId: number; samples: RunSample[] };
      if (!mounted.current) return;
      setDetail(d);
      setSamples(s.samples);
      setError(null);
    } catch (e) {
      if (!mounted.current) return;
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, [runId, maxPoints]);

  useEffect(() => {
    mounted.current = true;
    refresh();
    let id: ReturnType<typeof setInterval> | null = null;
    if (pollWhileActive && runId !== null) {
      id = setInterval(() => {
        if (detail?.run.active !== false) refresh();
      }, POLL_MS);
    }
    return () => {
      mounted.current = false;
      if (id) clearInterval(id);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runId, pollWhileActive]);

  return { detail, samples, loading, error, refresh };
}
