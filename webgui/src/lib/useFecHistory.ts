"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import type { TelemetrySnapshot } from "@/types/telemetry";

export interface FecSample {
  t: number;          // epoch ms
  attempted: number;  // blocks per second in this tick
  recovered: number;
  failed: number;
}

const MAX_SAMPLES = 120; // 2 min at 1Hz

/**
 * Returns per-tick block-outcome rates over time. The bridge now embeds
 * `blocksAttempted/Recovered/Failed` per second in the throughput history
 * array, so the chart has data on first paint instead of building it up
 * client-side. We fall back to deriving deltas client-side only when the
 * server didn't supply them (older bridge or unusual fallback path).
 */
export function useFecHistory(snap: TelemetrySnapshot | null): FecSample[] {
  // Prefer server-supplied series. Map throughput entries that carry the
  // per-second block rates straight into FecSamples.
  const fromServer = useMemo<FecSample[] | null>(() => {
    if (!snap) return null;
    const samples = snap.throughput.filter(
      (s) =>
        s.blocksAttempted !== undefined ||
        s.blocksRecovered !== undefined ||
        s.blocksFailed !== undefined,
    );
    if (samples.length === 0) return null;
    return samples.map((s) => ({
      t: s.t,
      attempted: s.blocksAttempted ?? 0,
      recovered: s.blocksRecovered ?? 0,
      failed: s.blocksFailed ?? 0,
    }));
  }, [snap?.throughput]);

  // ---- Client-side fallback (only used when server didn't supply rates) ----
  const prev = useRef<{ t: number; attempted: number; recovered: number; failed: number } | null>(null);
  const [series, setSeries] = useState<FecSample[]>([]);

  useEffect(() => {
    if (!snap || fromServer) return;
    const now = snap.generatedAt ?? Date.now();
    const cur = {
      t: now,
      attempted: snap.errors.blocksAttempted,
      recovered: snap.errors.blocksRecovered,
      failed: snap.errors.blocksFailed,
    };

    const last = prev.current;
    prev.current = cur;
    if (!last) return;

    const dt = (cur.t - last.t) / 1000;
    if (dt <= 0.01) return;
    if (cur.attempted < last.attempted) return;

    const sample: FecSample = {
      t: cur.t,
      attempted: Math.max(0, (cur.attempted - last.attempted) / dt),
      recovered: Math.max(0, (cur.recovered - last.recovered) / dt),
      failed: Math.max(0, (cur.failed - last.failed) / dt),
    };

    setSeries((prevSeries) => {
      const next = prevSeries.length >= MAX_SAMPLES
        ? prevSeries.slice(prevSeries.length - MAX_SAMPLES + 1)
        : prevSeries.slice();
      next.push(sample);
      return next;
    });
  }, [snap, fromServer]);

  return fromServer ?? series;
}
