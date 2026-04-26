"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { AlertEvent, TelemetrySnapshot } from "@/types/telemetry";

const ACK_STORAGE_KEY = "fso-gw:ack-alerts";
const MAX_CUMULATIVE = 1000;

interface AckSet {
  has(id: string): boolean;
  add(id: string): void;
  remove(id: string): void;
  clear(): void;
  size: number;
  all(): Set<string>;
}

function loadAcks(): Set<string> {
  if (typeof window === "undefined") return new Set();
  try {
    const raw = window.localStorage.getItem(ACK_STORAGE_KEY);
    if (!raw) return new Set();
    const arr = JSON.parse(raw);
    if (!Array.isArray(arr)) return new Set();
    return new Set(arr.filter((v) => typeof v === "string"));
  } catch {
    return new Set();
  }
}

function saveAcks(set: Set<string>): void {
  if (typeof window === "undefined") return;
  try {
    window.localStorage.setItem(ACK_STORAGE_KEY, JSON.stringify(Array.from(set)));
  } catch {
    // quota exceeded / unavailable — safe to ignore
  }
}

export interface AlertsView {
  /** All alerts ever seen this session (deduped by id, newest first). */
  all: AlertEvent[];
  /** Acknowledged IDs (persisted in localStorage). */
  acked: Set<string>;
  /** Count helpers. */
  counts: { total: number; critical: number; warning: number; info: number; active: number; acked: number };
  acknowledge: (id: string) => void;
  acknowledgeAll: (ids: string[]) => void;
  unacknowledge: (id: string) => void;
  clearAcks: () => void;
  clearHistory: () => void;
}

/**
 * Accumulates alerts from the telemetry stream across snapshots (the stream
 * only carries a rolling window — we keep a session-level cumulative list).
 * Tracks acknowledgments in localStorage so refresh doesn't reset them.
 */
export function useAlerts(snap: TelemetrySnapshot | null): AlertsView {
  const [all, setAll] = useState<AlertEvent[]>([]);
  const [acked, setAcked] = useState<Set<string>>(() => loadAcks());
  // Cheap fingerprint of the alert IDs we already merged, so the effect
  // can early-exit instead of running setAll(prev=>prev) on every 10 Hz
  // tick — that path was contributing to dev-mode update-depth warnings.
  const seenFingerprint = useRef<string>("");

  // Merge incoming alerts (deduped).
  useEffect(() => {
    if (!snap || snap.alerts.length === 0) return;
    // Build a fingerprint from the incoming alert IDs (in order) — fast
    // string compare beats running the dedup loop when nothing changed.
    const fp = snap.alerts.map((a) => a.id).join("|");
    if (fp === seenFingerprint.current) return;
    seenFingerprint.current = fp;
    setAll((prev) => {
      const seen = new Set(prev.map((a) => a.id));
      const added: AlertEvent[] = [];
      for (const a of snap.alerts) {
        if (!seen.has(a.id)) added.push(a);
      }
      if (added.length === 0) return prev;
      const merged = [...added, ...prev];
      merged.sort((a, b) => b.t - a.t);
      return merged.length > MAX_CUMULATIVE ? merged.slice(0, MAX_CUMULATIVE) : merged;
    });
  }, [snap]);

  const acknowledge = useCallback((id: string) => {
    setAcked((prev) => {
      if (prev.has(id)) return prev;
      const next = new Set(prev);
      next.add(id);
      saveAcks(next);
      return next;
    });
  }, []);

  const acknowledgeAll = useCallback((ids: string[]) => {
    setAcked((prev) => {
      const next = new Set(prev);
      let changed = false;
      for (const id of ids) {
        if (!next.has(id)) {
          next.add(id);
          changed = true;
        }
      }
      if (changed) saveAcks(next);
      return changed ? next : prev;
    });
  }, []);

  const unacknowledge = useCallback((id: string) => {
    setAcked((prev) => {
      if (!prev.has(id)) return prev;
      const next = new Set(prev);
      next.delete(id);
      saveAcks(next);
      return next;
    });
  }, []);

  const clearAcks = useCallback(() => {
    setAcked(new Set());
    saveAcks(new Set());
  }, []);

  const clearHistory = useCallback(() => {
    setAll([]);
  }, []);

  const counts = useMemo(() => {
    let critical = 0, warning = 0, info = 0, ackedCount = 0;
    for (const a of all) {
      if (a.severity === "critical") critical++;
      else if (a.severity === "warning") warning++;
      else info++;
      if (acked.has(a.id)) ackedCount++;
    }
    return {
      total: all.length,
      critical,
      warning,
      info,
      active: all.length - ackedCount,
      acked: ackedCount,
    };
  }, [all, acked]);

  return {
    all,
    acked,
    counts,
    acknowledge,
    acknowledgeAll,
    unacknowledge,
    clearAcks,
    clearHistory,
  };
}
