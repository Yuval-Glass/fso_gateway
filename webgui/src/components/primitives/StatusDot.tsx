import { cn } from "@/lib/utils";

type Status = "active" | "idle" | "warning" | "error";

const cfg: Record<Status, { color: string; glow: string }> = {
  active: { color: "var(--color-success)", glow: "rgba(52, 211, 153, 0.7)" },
  idle: { color: "var(--color-text-muted)", glow: "rgba(85, 96, 114, 0.3)" },
  warning: { color: "var(--color-warning)", glow: "rgba(255, 176, 32, 0.6)" },
  error: { color: "var(--color-danger)", glow: "rgba(255, 45, 92, 0.7)" },
};

export function StatusDot({
  status,
  label,
  size = 8,
  className,
}: {
  status: Status;
  label?: string;
  size?: number;
  className?: string;
}) {
  const c = cfg[status];
  return (
    <span className={cn("inline-flex items-center gap-2", className)}>
      <span
        className="rounded-full breathe"
        style={{
          width: size,
          height: size,
          background: c.color,
          color: c.color,
          boxShadow: `0 0 8px ${c.glow}`,
        }}
      />
      {label && (
        <span className="text-[11px] font-medium tracking-wider uppercase text-[color:var(--color-text-secondary)]">
          {label}
        </span>
      )}
    </span>
  );
}
