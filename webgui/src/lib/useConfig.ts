"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { GatewayConfig } from "@/types/config";

function bridgeBase(): string {
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `http://${host}:8000`;
}

export interface ConfigState {
  /** Persisted config as returned by the last successful GET or POST. */
  persisted: GatewayConfig | null;
  /** Current editable draft. */
  draft: GatewayConfig | null;
  /** True when draft differs from persisted. */
  dirty: boolean;
  /** Network / load state. */
  status: "idle" | "loading" | "saving" | "error";
  /** Non-null when the last operation failed. */
  error: string | null;
  /** True when the most recent save reported requires_restart. */
  requiresRestart: boolean;
  /** Replace the draft wholesale (used by presets). */
  setDraft: (cfg: Partial<GatewayConfig>) => void;
  /** Patch a single field in the draft. */
  update: <K extends keyof GatewayConfig>(key: K, value: GatewayConfig[K]) => void;
  /** POST draft to the bridge. Optional `override` is merged on top of the
   *  current draft before sending — useful when a caller updates a field
   *  and immediately wants to save (avoids the React state-batching race). */
  save: (override?: Partial<GatewayConfig>) => Promise<boolean>;
  /** Revert the draft back to persisted. */
  revert: () => void;
  /** Refetch persisted config from the bridge. */
  refresh: () => Promise<void>;
}

function eq(a: GatewayConfig | null, b: GatewayConfig | null): boolean {
  if (!a || !b) return a === b;
  return (
    a.lan_iface === b.lan_iface &&
    a.fso_iface === b.fso_iface &&
    a.k === b.k &&
    a.m === b.m &&
    a.depth === b.depth &&
    a.symbol_size === b.symbol_size &&
    a.internal_symbol_crc === b.internal_symbol_crc
  );
}

export function useConfig(): ConfigState {
  const [persisted, setPersisted] = useState<GatewayConfig | null>(null);
  const [draft, setDraftInternal] = useState<GatewayConfig | null>(null);
  const [status, setStatus] = useState<ConfigState["status"]>("loading");
  const [error, setError] = useState<string | null>(null);
  const [requiresRestart, setRequiresRestart] = useState(false);

  const refresh = useCallback(async () => {
    setStatus("loading");
    setError(null);
    try {
      const res = await fetch(`${bridgeBase()}/api/config`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = await res.json();
      const cfg: GatewayConfig = body.config;
      setPersisted(cfg);
      setDraftInternal(cfg);
      setStatus("idle");
    } catch (e) {
      setStatus("error");
      setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  useEffect(() => {
    refresh();
  }, [refresh]);

  const setDraft = useCallback(
    (patch: Partial<GatewayConfig>) => {
      setDraftInternal((prev) => (prev ? { ...prev, ...patch } : prev));
    },
    [],
  );

  const update = useCallback(
    <K extends keyof GatewayConfig>(key: K, value: GatewayConfig[K]) => {
      setDraftInternal((prev) => (prev ? { ...prev, [key]: value } : prev));
    },
    [],
  );

  // Ref-backed draft so callbacks always read the current state without
  // having to be rebuilt on every change.
  const draftRef = useRef<GatewayConfig | null>(null);
  useEffect(() => {
    draftRef.current = draft;
  }, [draft]);

  const save = useCallback(async (override?: Partial<GatewayConfig>) => {
    const base = draftRef.current;
    if (!base) return false;
    // Merge any override the caller passes — covers the case where the
    // caller just called update() and wants the new value sent immediately
    // (setState is async, so the ref/state may still be the previous value).
    const payload: GatewayConfig = override ? { ...base, ...override } : base;
    // Optimistically reflect the override in local state so the UI doesn't
    // briefly snap back to the prior value while we wait for the server.
    if (override) setDraftInternal(payload);
    setStatus("saving");
    setError(null);
    try {
      const res = await fetch(`${bridgeBase()}/api/config`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ config: payload }),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok) {
        const msg = (body && body.detail) ? String(body.detail) : `HTTP ${res.status}`;
        throw new Error(msg);
      }
      setPersisted(body.config as GatewayConfig);
      setDraftInternal(body.config as GatewayConfig);
      setRequiresRestart(Boolean(body.requires_restart));
      setStatus("idle");
      return true;
    } catch (e) {
      setStatus("error");
      setError(e instanceof Error ? e.message : String(e));
      return false;
    }
  }, []);

  const revert = useCallback(() => {
    setDraftInternal(persisted);
    setError(null);
  }, [persisted]);

  return {
    persisted,
    draft,
    dirty: !eq(persisted, draft),
    status,
    error,
    requiresRestart,
    setDraft,
    update,
    save,
    revert,
    refresh,
  };
}
