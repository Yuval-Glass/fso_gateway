import { type ClassValue, clsx } from "clsx";
import { twMerge } from "tailwind-merge";

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

/** Format a number with unit and compact thousands separators. */
export function formatNumber(n: number, opts: { decimals?: number } = {}): string {
  const { decimals = 0 } = opts;
  if (!Number.isFinite(n)) return "—";
  return n.toLocaleString("en-US", {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals,
  });
}

/** Format bytes → human units. */
export function formatBytes(bytes: number, decimals = 2): string {
  if (bytes === 0) return "0 B";
  const k = 1024;
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  const i = Math.min(units.length - 1, Math.floor(Math.log(Math.abs(bytes)) / Math.log(k)));
  return `${(bytes / Math.pow(k, i)).toFixed(decimals)} ${units[i]}`;
}

/** Format bits/sec → Mbps/Gbps with precision. */
export function formatBitrate(bps: number): { value: string; unit: string } {
  if (bps >= 1e9) return { value: (bps / 1e9).toFixed(2), unit: "Gbps" };
  if (bps >= 1e6) return { value: (bps / 1e6).toFixed(1), unit: "Mbps" };
  if (bps >= 1e3) return { value: (bps / 1e3).toFixed(1), unit: "Kbps" };
  return { value: bps.toFixed(0), unit: "bps" };
}

/** Format a percent (0-1 → "42.3%"). */
export function formatPercent(frac: number, decimals = 2): string {
  if (!Number.isFinite(frac)) return "—";
  return `${(frac * 100).toFixed(decimals)}%`;
}

/** Format duration in seconds → "2d 14:37:12" */
export function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  const hms = `${h.toString().padStart(2, "0")}:${m.toString().padStart(2, "0")}:${s.toString().padStart(2, "0")}`;
  return d > 0 ? `${d}d ${hms}` : hms;
}
