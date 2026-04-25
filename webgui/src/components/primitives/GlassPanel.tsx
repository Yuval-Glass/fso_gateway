import { cn } from "@/lib/utils";
import type { HTMLAttributes, ReactNode } from "react";
import { FieldHint } from "./FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";

interface GlassPanelProps extends HTMLAttributes<HTMLDivElement> {
  variant?: "default" | "raised" | "cyan";
  label?: string;
  trailing?: ReactNode;
  padded?: boolean;
  hintId?: FieldHintId;
}

export function GlassPanel({
  variant = "default",
  label,
  trailing,
  padded = true,
  hintId,
  className,
  children,
  ...rest
}: GlassPanelProps) {
  const base =
    variant === "raised" ? "glass-raised" : variant === "cyan" ? "glass glass-cyan" : "glass";
  return (
    <div
      className={cn(
        base,
        "rounded-lg relative transition-all duration-300",
        "hover:border-[color:var(--color-border-strong)]",
        className,
      )}
      {...rest}
    >
      {label !== undefined && (
        <div className="flex items-center justify-between px-4 pt-3 pb-2 border-b border-[color:var(--color-border-hair)]">
          <h3 className="text-[10px] font-medium tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] inline-flex items-center gap-1">
            <span>{label}</span>
            {hintId && <FieldHint id={hintId} size={11} />}
          </h3>
          {trailing && <div className="flex items-center gap-2">{trailing}</div>}
        </div>
      )}
      <div className={cn(padded && "p-4", label !== undefined && padded && "pt-3")}>{children}</div>
    </div>
  );
}
