"use client";

import { AlertOctagon, CheckCircle2, ShieldX, TriangleAlert, XCircle } from "lucide-react";
import { MetricCard } from "@/components/primitives/MetricCard";
import { formatNumber } from "@/lib/utils";
import type { ErrorMetrics as Errors } from "@/types/telemetry";

export function ErrorMetrics({ errors }: { errors: Errors }) {
  const ber = errors.ber != null ? errors.ber.toExponential(1) : "—";
  const flr = (errors.flrPct * 100).toFixed(4);
  const recoveryRate =
    errors.blocksAttempted > 0
      ? (errors.blocksRecovered / errors.blocksAttempted) * 100
      : 100;

  return (
    <div className="grid grid-cols-2 lg:grid-cols-5 gap-4">
      <MetricCard
        label="BER (est.)"
        value={ber}
        tone={errors.ber != null && errors.ber > 1e-6 ? "warning" : "success"}
        icon={<ShieldX size={14} />}
        sub={<span className="text-[color:var(--color-text-secondary)]">Very Good</span>}
      />
      <MetricCard
        label="FLR"
        value={flr}
        unit="%"
        tone={errors.flrPct > 0.01 ? "danger" : errors.flrPct > 0.001 ? "warning" : "success"}
        icon={<TriangleAlert size={14} />}
        sub={<span className="text-[color:var(--color-text-secondary)]">Packet loss rate</span>}
      />
      <MetricCard
        label="CRC Drops"
        value={formatNumber(errors.crcDrops)}
        tone={errors.crcDrops > 100 ? "warning" : "neutral"}
        icon={<XCircle size={14} />}
        sub={<span className="text-[color:var(--color-text-secondary)]">Since start</span>}
      />
      <MetricCard
        label="Recovered"
        value={formatNumber(errors.recoveredPackets)}
        tone="success"
        icon={<CheckCircle2 size={14} />}
        sub={<span className="text-[color:var(--color-text-secondary)]">FEC-rescued packets</span>}
      />
      <MetricCard
        label="FEC Success Rate"
        value={recoveryRate.toFixed(3)}
        unit="%"
        tone={recoveryRate > 99.9 ? "success" : recoveryRate > 99 ? "cyan" : "warning"}
        icon={<AlertOctagon size={14} />}
        sub={
          <span className="text-[color:var(--color-text-secondary)]">
            {formatNumber(errors.blocksRecovered)} / {formatNumber(errors.blocksAttempted)} blocks
          </span>
        }
      />
    </div>
  );
}
