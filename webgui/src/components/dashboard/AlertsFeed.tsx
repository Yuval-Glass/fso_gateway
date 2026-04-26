"use client";

import { GlassPanel } from "@/components/primitives/GlassPanel";
import type { AlertEvent } from "@/types/telemetry";
import { cn } from "@/lib/utils";

const severityStyle: Record<
  AlertEvent["severity"],
  { dot: string; text: string; glow: string }
> = {
  info: { dot: "var(--color-cyan-500)", text: "text-[color:var(--color-cyan-300)]", glow: "0 0 6px rgba(0, 212, 255, 0.7)" },
  warning: { dot: "var(--color-warning)", text: "text-[color:var(--color-warning)]", glow: "0 0 6px rgba(255, 176, 32, 0.8)" },
  critical: { dot: "var(--color-danger)", text: "text-[color:var(--color-danger)]", glow: "0 0 8px rgba(255, 45, 92, 0.9)" },
};

/** Inner scrollable list height. Fixed so the panel doesn't reflow the page
 *  every time a new event arrives at 10 Hz. */
const FEED_HEIGHT_PX = 280;

export function AlertsFeed({ alerts }: { alerts: AlertEvent[] }) {
  return (
    <GlassPanel
      label="Alerts & Events"
      labelHref="/alerts"
      trailing={
        <span className="text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
          Last {alerts.length}
        </span>
      }
    >
      <ul
        className="space-y-2 overflow-y-auto overscroll-contain pr-1"
        style={{ height: FEED_HEIGHT_PX }}
      >
        {alerts.length === 0 ? (
          <li className="text-[11px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] py-2">
            No events yet.
          </li>
        ) : (
          alerts.map((a) => {
            const s = severityStyle[a.severity];
            const t = new Date(a.t).toLocaleTimeString([], {
              hour: "2-digit",
              minute: "2-digit",
              second: "2-digit",
            });
            return (
              <li key={a.id} className="flex items-start gap-3 text-xs py-1">
                <span
                  className="mt-1.5 w-1.5 h-1.5 rounded-full shrink-0"
                  style={{ background: s.dot, boxShadow: s.glow }}
                />
                <span className="font-mono tabular text-[color:var(--color-text-muted)] shrink-0 text-[11px]">
                  {t}
                </span>
                <span className={cn("font-semibold tracking-[0.15em] uppercase shrink-0 text-[10px]", s.text)}>
                  {a.module}
                </span>
                <span className="text-[color:var(--color-text-secondary)] truncate">
                  {a.message}
                </span>
              </li>
            );
          })
        )}
      </ul>
    </GlassPanel>
  );
}
