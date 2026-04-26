"use client";

import { useEffect, useRef, useState } from "react";
import { snapshot as localSnapshot, SAMPLE_RATE_MS } from "./mockTelemetry";
import type { ConnectionStatus, TelemetryFeed, TelemetrySnapshot } from "@/types/telemetry";

/**
 * Bridge endpoint. Same host as the app, port 8000 by default.
 * Override with NEXT_PUBLIC_BRIDGE_WS (e.g. "ws://192.168.1.50:8000/ws/live").
 */
function wsUrl(): string {
  const override = process.env.NEXT_PUBLIC_BRIDGE_WS;
  if (override) return override;
  if (typeof window === "undefined") return "";
  const host = window.location.hostname || "127.0.0.1";
  return `ws://${host}:8000/ws/live`;
}

const RECONNECT_DELAY_MS = 3000;

/**
 * Subscribes to the telemetry stream.
 *
 * Primary path: WebSocket to FastAPI bridge at /ws/live.
 * Fallback: local mock at the same 1 Hz cadence while the WS is disconnected.
 * Reconnects automatically with a fixed backoff.
 */
export function useTelemetry(): TelemetryFeed {
  const [snap, setSnap] = useState<TelemetrySnapshot | null>(null);
  const [connection, setConnection] = useState<ConnectionStatus>("connecting");
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    let ws: WebSocket | null = null;
    let fallbackTimer: ReturnType<typeof setInterval> | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

    const startFallback = () => {
      if (fallbackTimer) return;
      setConnection("demo");
      // Immediate tick so UI updates without waiting.
      setSnap(localSnapshot());
      fallbackTimer = setInterval(() => {
        if (!mounted.current) return;
        setSnap(localSnapshot());
      }, SAMPLE_RATE_MS);
    };

    const stopFallback = () => {
      if (fallbackTimer) {
        clearInterval(fallbackTimer);
        fallbackTimer = null;
      }
    };

    const connect = () => {
      if (!mounted.current) return;
      // Don't flip to "connecting" if we're already in "demo" — that would
      // visually flap. Only show "connecting" on first attempt / after live.
      setConnection((prev) => (prev === "live" ? "connecting" : prev));

      let socket: WebSocket;
      try {
        socket = new WebSocket(wsUrl());
      } catch {
        startFallback();
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
        return;
      }
      ws = socket;

      socket.onopen = () => {
        if (!mounted.current) return;
        stopFallback();
        setConnection("live");
      };

      socket.onmessage = (ev) => {
        if (!mounted.current) return;
        try {
          const parsed = JSON.parse(ev.data) as TelemetrySnapshot;
          // Functional update with identity bail-out: when the bridge
          // re-sends the same frame (same generatedAt) React 18+ skips
          // the re-render entirely. Prevents update-depth thrash under
          // 10 Hz streaming + dev-mode strict checks.
          setSnap((prev) => {
            if (prev && prev.generatedAt === parsed.generatedAt) return prev;
            return parsed;
          });
        } catch {
          // malformed frame — ignore, next one will arrive
        }
      };

      const handleLost = () => {
        ws = null;
        if (!mounted.current) return;
        startFallback();
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connect, RECONNECT_DELAY_MS);
      };

      socket.onclose = handleLost;
      socket.onerror = () => {
        // onclose will always follow; avoid double-handling
      };
    };

    connect();

    return () => {
      mounted.current = false;
      if (ws) {
        ws.onclose = null;
        ws.onerror = null;
        ws.onmessage = null;
        ws.onopen = null;
        try { ws.close(); } catch {}
      }
      if (fallbackTimer) clearInterval(fallbackTimer);
      if (reconnectTimer) clearTimeout(reconnectTimer);
    };
  }, []);

  return { snapshot: snap, connection };
}
