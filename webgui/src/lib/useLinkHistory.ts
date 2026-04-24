"use client";

import { useEffect, useRef, useState } from "react";
import type { LinkState, TelemetrySnapshot } from "@/types/telemetry";

export interface LinkSample {
  t: number;
  state: LinkState;
  qualityPct: number;
  rssiDbm: number | null;
  snrDb: number | null;
  berEstimate: number | null;
  latencyAvgMs: number;
  latencyMaxMs: number;
}

export interface FadeEvent {
  id: string;
  tStart: number;
  tEnd: number | null; // null → ongoing
  fromState: LinkState;
  toState: LinkState;
  lowestQualityPct: number;
  durationMs: number | null;
}

export interface LinkHistoryView {
  samples: LinkSample[];
  fades: FadeEvent[];
  /** Fraction of samples where state === online (0..1). */
  uptimeRatio: number;
  /** ms since last state change. */
  streakMs: number;
  /** ms since mount (for "observed for" display). */
  observedMs: number;
}

const MAX_SAMPLES = 180; // 3 min at 1Hz

export function useLinkHistory(snap: TelemetrySnapshot | null): LinkHistoryView {
  const [samples, setSamples] = useState<LinkSample[]>([]);
  const [fades, setFades] = useState<FadeEvent[]>([]);
  const mountedAt = useRef<number>(Date.now());
  const lastState = useRef<LinkState | null>(null);
  const streakStart = useRef<number | null>(null);

  useEffect(() => {
    if (!snap) return;
    const l = snap.link;
    const now = snap.generatedAt ?? Date.now();
    const sample: LinkSample = {
      t: now,
      state: l.state,
      qualityPct: l.qualityPct,
      rssiDbm: l.rssiDbm,
      snrDb: l.snrDb,
      berEstimate: l.berEstimate,
      latencyAvgMs: l.latencyMsAvg,
      latencyMaxMs: l.latencyMsMax,
    };

    setSamples((prev) => {
      const next = prev.length >= MAX_SAMPLES
        ? prev.slice(prev.length - MAX_SAMPLES + 1)
        : prev.slice();
      next.push(sample);
      return next;
    });

    // Fade-event tracking: record every online → (degraded|offline) transition
    // and close it when we return to online.
    const prevState = lastState.current;
    if (prevState === null) {
      streakStart.current = now;
    } else if (prevState !== l.state) {
      streakStart.current = now;
      if (prevState === "online" && l.state !== "online") {
        setFades((prev) => [
          ...prev,
          {
            id: `${now}`,
            tStart: now,
            tEnd: null,
            fromState: prevState,
            toState: l.state,
            lowestQualityPct: l.qualityPct,
            durationMs: null,
          },
        ]);
      } else if (prevState !== "online" && l.state === "online") {
        setFades((prev) => {
          if (prev.length === 0) return prev;
          const last = prev[prev.length - 1];
          if (last.tEnd !== null) return prev;
          return [
            ...prev.slice(0, -1),
            {
              ...last,
              tEnd: now,
              durationMs: now - last.tStart,
            },
          ];
        });
      } else {
        // degraded → offline or offline → degraded: extend active fade
        setFades((prev) => {
          if (prev.length === 0) return prev;
          const last = prev[prev.length - 1];
          if (last.tEnd !== null) return prev;
          return [
            ...prev.slice(0, -1),
            { ...last, toState: l.state, lowestQualityPct: Math.min(last.lowestQualityPct, l.qualityPct) },
          ];
        });
      }
    } else {
      // Same state: update lowest quality on the currently-open fade
      if (l.state !== "online") {
        setFades((prev) => {
          if (prev.length === 0) return prev;
          const last = prev[prev.length - 1];
          if (last.tEnd !== null) return prev;
          if (l.qualityPct >= last.lowestQualityPct) return prev;
          return [...prev.slice(0, -1), { ...last, lowestQualityPct: l.qualityPct }];
        });
      }
    }

    lastState.current = l.state;
  }, [snap]);

  const uptimeRatio = samples.length === 0
    ? 1
    : samples.filter((s) => s.state === "online").length / samples.length;

  const streakMs = streakStart.current ? Date.now() - streakStart.current : 0;
  const observedMs = Date.now() - mountedAt.current;

  return { samples, fades, uptimeRatio, streakMs, observedMs };
}
