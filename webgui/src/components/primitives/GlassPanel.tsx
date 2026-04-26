import Link from "next/link";
import { Maximize2 } from "lucide-react";
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
  /** If set, the label becomes a Link to this href. */
  labelHref?: string;
  /** If set, the body is wrapped in a button-like clickable region. */
  onBodyClick?: () => void;
}

export function GlassPanel({
  variant = "default",
  label,
  trailing,
  padded = true,
  hintId,
  labelHref,
  onBodyClick,
  className,
  children,
  ...rest
}: GlassPanelProps) {
  const base =
    variant === "raised" ? "glass-raised" : variant === "cyan" ? "glass glass-cyan" : "glass";
  const isInteractive = !!onBodyClick;
  return (
    <div
      className={cn(
        base,
        "group/panel rounded-lg relative transition-[border-color,box-shadow] duration-200",
        "hover:border-[color:var(--color-border-strong)]",
        isInteractive && "hover:border-[color:var(--color-border-cyan)] hover:shadow-[0_0_24px_rgba(0,212,255,0.15)]",
        className,
      )}
      {...rest}
    >
      {label !== undefined && (
        <div className="flex items-center justify-between px-4 pt-3 pb-2 border-b border-[color:var(--color-border-hair)]">
          <h3 className="text-[10px] font-medium tracking-[0.18em] uppercase text-[color:var(--color-text-secondary)] inline-flex items-center gap-1">
            {labelHref ? (
              <Link
                href={labelHref}
                className="group/lbl inline-flex items-center gap-1 hover:text-[color:var(--color-cyan-300)] transition-colors"
                onClick={(e) => e.stopPropagation()}
              >
                <span>{label}</span>
                <span
                  aria-hidden
                  className="opacity-0 -translate-x-1 group-hover/lbl:opacity-100 group-hover/lbl:translate-x-0 transition-all duration-150 text-[color:var(--color-cyan-300)]"
                >
                  →
                </span>
              </Link>
            ) : (
              <span>{label}</span>
            )}
            {hintId && <FieldHint id={hintId} size={11} />}
          </h3>
          {trailing && <div className="flex items-center gap-2">{trailing}</div>}
        </div>
      )}
      <div
        className={cn(
          "relative",
          padded && "p-4",
          label !== undefined && padded && "pt-3",
          onBodyClick && "cursor-pointer hover:bg-white/[0.015] transition-colors",
        )}
        onClick={onBodyClick}
        role={onBodyClick ? "button" : undefined}
        tabIndex={onBodyClick ? 0 : undefined}
        onKeyDown={
          onBodyClick
            ? (e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  onBodyClick();
                }
              }
            : undefined
        }
      >
        {children}
        {isInteractive && (
          <span
            className="pointer-events-none absolute top-2 right-2 opacity-0 group-hover/panel:opacity-100 transition-opacity duration-200 text-[color:var(--color-cyan-300)]"
            style={{ filter: "drop-shadow(0 0 4px rgba(0,212,255,0.6))" }}
            aria-hidden
          >
            <Maximize2 size={13} strokeWidth={2} />
          </span>
        )}
      </div>
    </div>
  );
}
