"use client";

import { Radio, RadioTower, Satellite, WifiOff } from "lucide-react";
import type { ConnectionStatus, TelemetrySnapshot } from "@/types/telemetry";

interface PillCfg {
  label: string;
  color: string;
  glow: string;
  Icon: typeof Radio;
  breathe: boolean;
  title: string;
}

const BASE_CFG: Record<ConnectionStatus, PillCfg> = {
  live: {
    label: "LIVE · BRIDGE",
    color: "var(--color-success)",
    glow: "rgba(52, 211, 153, 0.7)",
    Icon: RadioTower,
    breathe: false,
    title: "Live telemetry streaming from the bridge.",
  },
  connecting: {
    label: "CONNECTING",
    color: "var(--color-warning)",
    glow: "rgba(255, 176, 32, 0.7)",
    Icon: Radio,
    breathe: true,
    title: "Attempting to connect to the bridge…",
  },
  demo: {
    label: "DEMO · NO BRIDGE",
    color: "var(--color-cyan-500)",
    glow: "rgba(0, 212, 255, 0.55)",
    Icon: WifiOff,
    breathe: false,
    title:
      "Bridge WebSocket unreachable — showing synthetic data. Start the bridge with: cd webgui/server && uv run uvicorn main:app --port 8000",
  },
};

function resolveCfg(status: ConnectionStatus, source?: TelemetrySnapshot["source"]): PillCfg {
  const base = BASE_CFG[status];
  if (status !== "live") return base;
  if (source === "gateway") {
    return {
      ...base,
      label: "LIVE · GATEWAY",
      Icon: Satellite,
      title: "Streaming real telemetry from the FSO Gateway daemon.",
    };
  }
  if (source === "mock") {
    return {
      ...base,
      label: "LIVE · MOCK BRIDGE",
      title:
        "Bridge is up but the gateway daemon is not connected — serving synthetic data from Python.",
    };
  }
  return base;
}

export function ConnectionPill({
  status,
  source,
}: {
  status: ConnectionStatus;
  source?: TelemetrySnapshot["source"];
}) {
  const c = resolveCfg(status, source);
  const Icon = c.Icon;
  return (
    <span
      className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-md border"
      style={{
        borderColor: `${c.color}66`,
        background: `${c.color}10`,
        boxShadow: `inset 0 0 12px ${c.glow.replace(/[0-9.]+\)$/, "0.12)")}`,
      }}
      title={c.title}
    >
      <Icon
        size={11}
        strokeWidth={2}
        className={c.breathe ? "breathe" : ""}
        style={{ color: c.color, filter: `drop-shadow(0 0 4px ${c.glow})` }}
      />
      <span
        className="text-[9px] font-semibold tracking-[0.2em]"
        style={{ color: c.color }}
      >
        {c.label}
      </span>
    </span>
  );
}
