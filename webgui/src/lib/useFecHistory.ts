"use client";

import { useEffect, useRef, useState } from "react";
import type { TelemetrySnapshot } from "@/types/telemetry";

export interface FecSample {
  t: number;          // epoch ms
  attempted: number;  // blocks per second in this tick
  recovered: number;
  failed: number;
}

const MAX_SAMPLES = 120; // 2 min at 1Hz

/**
 * Derives per-tick block-outcome rates from cumulative counters on each
 * snapshot. The bridge only streams counters; this hook keeps a local ring
 * buffer of deltas so charts can show "blocks/sec over time" without the
 * bridge having to do that work.
 */
export function useFecHistory(snap: TelemetrySnapshot | null): FecSample[] {
  const prev = useRef<{ t: number; attempted: number; recovered: number; failed: number } | null>(null);
  const [series, setSeries] = useState<FecSample[]>([]);

  useEffect(() => {
    if (!snap) return;
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

    // Handle counter resets (daemon restart → counters drop): skip a sample.
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
  }, [snap]);

  return series;
}
