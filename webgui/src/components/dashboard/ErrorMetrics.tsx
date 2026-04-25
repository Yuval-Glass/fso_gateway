"use client";

import { AlertOctagon, CheckCircle2, ShieldX, TriangleAlert, XCircle } from "lucide-react";
import { MetricCard } from "@/components/primitives/MetricCard";
import { formatCompact, formatNumber, formatPercent } from "@/lib/utils";
import type { ErrorMetrics as Errors } from "@/types/telemetry";

export function ErrorMetrics({ errors }: { errors: Errors }) {
  const lossRatio = errors.symbolLossRatio;
  const lossStr =
    lossRatio == null ? "—"
    : lossRatio === 0 ? "0.00%"
    : lossRatio < 1e-4 ? lossRatio.toExponential(1)
    : formatPercent(lossRatio, 3);

  const recoveryRate =
    errors.blocksAttempted > 0
      ? (errors.blocksRecovered / errors.blocksAttempted) * 100
      : 100;

  return (
    <div className="grid grid-cols-2 lg:grid-cols-5 gap-4">
      <MetricCard
        label="Symbol Loss"
        value={lossStr}
        tone={
          lossRatio == null ? "neutral"
          : lossRatio > 1e-3 ? "danger"
          : lossRatio > 1e-4 ? "warning"
          : "success"
        }
        icon={<ShieldX size={14} />}
        hintId="errors.symbolLossRatio"
        href="/link-status"
        sub={<span className="text-[color:var(--color-text-secondary)]">lost/total on FSO</span>}
      />
      <MetricCard
        label="Block Fail Rate"
        value={(errors.blockFailRatio * 100).toFixed(4)}
        unit="%"
        tone={errors.blockFailRatio > 0.01 ? "danger" : errors.blockFailRatio > 0.001 ? "warning" : "success"}
        icon={<TriangleAlert size={14} />}
        hintId="errors.blockFailRatio"
        href="/fec-analytics"
        sub={<span className="text-[color:var(--color-text-secondary)]">blocks_failed / attempted</span>}
      />
      <MetricCard
        label="CRC Drops"
        value={formatCompact(errors.crcDrops)}
        tone={errors.crcDrops > 100 ? "warning" : "neutral"}
        icon={<XCircle size={14} />}
        hintId="errors.crcDrops"
        href="/packet-inspector"
        sub={<span className="text-[color:var(--color-text-secondary)]">Since run start</span>}
      />
      <MetricCard
        label="Recovered"
        value={formatCompact(errors.recoveredPackets)}
        tone="success"
        icon={<CheckCircle2 size={14} />}
        hintId="errors.recoveredPackets"
        href="/fec-analytics"
        sub={<span className="text-[color:var(--color-text-secondary)]">LAN frames re-emitted</span>}
      />
      <MetricCard
        label="FEC Success"
        value={recoveryRate.toFixed(3)}
        unit="%"
        tone={recoveryRate > 99.9 ? "success" : recoveryRate > 99 ? "cyan" : "warning"}
        icon={<AlertOctagon size={14} />}
        hintId="errors.fecSuccessRate"
        href="/fec-analytics"
        sub={
          <span className="text-[color:var(--color-text-secondary)]">
            {formatCompact(errors.blocksRecovered)} / {formatCompact(errors.blocksAttempted)} blocks
          </span>
        }
      />
    </div>
  );
}
