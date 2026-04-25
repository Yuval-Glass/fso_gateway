"use client";

import { Activity, ArrowDownUp, Gauge, Zap } from "lucide-react";
import { MetricCard } from "@/components/primitives/MetricCard";
import { formatBitrate, formatNumber } from "@/lib/utils";
import type { TelemetrySnapshot } from "@/types/telemetry";

export function ThroughputCards({ snap }: { snap: TelemetrySnapshot }) {
  const latest = snap.throughput[snap.throughput.length - 1];
  const txRate = formatBitrate(latest.txBps);
  const rxRate = formatBitrate(latest.rxBps);

  const txSpark = snap.throughput.slice(-60).map((s) => s.txBps / 1e6);
  const rxSpark = snap.throughput.slice(-60).map((s) => s.rxBps / 1e6);
  const ppsSpark = snap.throughput.slice(-60).map((s) => s.txPps);

  const utilizationPct = (latest.txBps + latest.rxBps) / (10e9 * 2) * 100;
  const utilSpark = snap.throughput.slice(-60).map(
    (s) => ((s.txBps + s.rxBps) / (10e9 * 2)) * 100,
  );

  return (
    <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
      <MetricCard
        label="TX Throughput"
        value={txRate.value}
        unit={txRate.unit}
        tone="cyan"
        icon={<Zap size={14} />}
        hintId="traffic.txBps"
        sub={
          <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
            {formatNumber(latest.txPps)} pps
          </span>
        }
        spark={txSpark}
      />
      <MetricCard
        label="RX Throughput"
        value={rxRate.value}
        unit={rxRate.unit}
        tone="cyan"
        icon={<Zap size={14} />}
        hintId="traffic.rxBps"
        sub={
          <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
            {formatNumber(latest.rxPps)} pps
          </span>
        }
        spark={rxSpark}
      />
      <MetricCard
        label="Packets / sec"
        value={formatNumber(latest.txPps)}
        unit="pps"
        icon={<ArrowDownUp size={14} />}
        hintId="traffic.txPps"
        sub={
          <span className="font-mono tabular text-[color:var(--color-text-secondary)]">
            RX: {formatNumber(latest.rxPps)}
          </span>
        }
        spark={ppsSpark}
      />
      <MetricCard
        label="Link Utilization"
        value={utilizationPct.toFixed(1)}
        unit="%"
        tone={utilizationPct > 80 ? "warning" : "success"}
        icon={<Gauge size={14} />}
        hintId="traffic.utilization"
        sub={
          <span className="text-[color:var(--color-text-secondary)]">
            of 10 Gbps aggregate
          </span>
        }
        spark={utilSpark}
      />
    </div>
  );
}
