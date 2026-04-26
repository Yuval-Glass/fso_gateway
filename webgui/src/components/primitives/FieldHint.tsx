"use client";

import { Info } from "lucide-react";
import { useEffect, useId, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { FIELD_HINTS, type FieldHintId } from "@/lib/fieldHints";
import { cn } from "@/lib/utils";

interface FieldHintProps {
  /** Lookup key in the central FIELD_HINTS registry. */
  id?: FieldHintId;
  /** Inline title (overrides registry). */
  title?: string;
  /** Inline body (overrides registry). */
  body?: string;
  /** Optional source-of-truth pointer (e.g. "src/stats.c — stats_inc_recovered()"). */
  source?: string;
  /** Icon size in px. */
  size?: number;
  /** Extra class on the trigger. */
  className?: string;
}

const TOOLTIP_W = 320;
const TOOLTIP_GAP = 8;

export function FieldHint({
  id,
  title: titleProp,
  body: bodyProp,
  source: sourceProp,
  size = 12,
  className,
}: FieldHintProps) {
  const entry = id ? FIELD_HINTS[id] : undefined;
  const title = titleProp ?? entry?.title;
  const body = bodyProp ?? entry?.body;
  const source = sourceProp ?? entry?.source;

  const [open, setOpen] = useState(false);
  const [pos, setPos] = useState<{ left: number; top: number; placement: "top" | "bottom" }>({
    left: 0,
    top: 0,
    placement: "top",
  });
  const triggerRef = useRef<HTMLButtonElement | null>(null);
  const tooltipId = useId();
  const [mounted, setMounted] = useState(false);

  useEffect(() => {
    setMounted(true);
  }, []);

  useLayoutEffect(() => {
    if (!open || !triggerRef.current) return;
    const rect = triggerRef.current.getBoundingClientRect();
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    // Prefer top placement; flip if not enough space.
    const fitsAbove = rect.top > 200;
    const placement: "top" | "bottom" = fitsAbove ? "top" : "bottom";
    let left = rect.left + rect.width / 2 - TOOLTIP_W / 2;
    if (left < 8) left = 8;
    if (left + TOOLTIP_W > vw - 8) left = vw - TOOLTIP_W - 8;
    const top =
      placement === "top"
        ? rect.top - TOOLTIP_GAP
        : Math.min(rect.bottom + TOOLTIP_GAP, vh - 16);
    setPos({ left, top, placement });
  }, [open]);

  // Close on scroll/resize so the tooltip never floats stale.
  useEffect(() => {
    if (!open) return;
    const close = () => setOpen(false);
    window.addEventListener("scroll", close, true);
    window.addEventListener("resize", close);
    return () => {
      window.removeEventListener("scroll", close, true);
      window.removeEventListener("resize", close);
    };
  }, [open]);

  if (!body && !title) {
    // Nothing useful to show — render nothing so we don't pollute the UI.
    return null;
  }

  return (
    <>
      <button
        ref={triggerRef}
        type="button"
        aria-label={title ? `Hint: ${title}` : "Field information"}
        aria-describedby={open ? tooltipId : undefined}
        onMouseEnter={() => setOpen(true)}
        onMouseLeave={() => setOpen(false)}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onClick={(e) => {
          e.preventDefault();
          e.stopPropagation();
          setOpen((v) => !v);
        }}
        className={cn(
          "inline-flex items-center justify-center align-middle",
          "text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)]",
          "transition-colors cursor-help",
          "rounded-full focus:outline-none focus-visible:ring-1 focus-visible:ring-[color:var(--color-cyan-500)]",
          className,
        )}
      >
        <Info size={size} strokeWidth={1.8} />
      </button>
      {mounted && open &&
        createPortal(
          <div
            id={tooltipId}
            role="tooltip"
            className="fixed z-[1000] pointer-events-none"
            style={{
              left: pos.left,
              top: pos.top,
              width: TOOLTIP_W,
              transform: pos.placement === "top" ? "translateY(-100%)" : undefined,
            }}
          >
            <div
              className="glass-raised rounded-md px-3 py-2.5 shadow-[0_10px_40px_rgba(0,0,0,0.5)] border border-[color:var(--color-border-cyan)] animate-[fade-in_120ms_ease-out]"
              style={{
                background: "rgba(8, 14, 28, 0.96)",
                backdropFilter: "blur(8px)",
              }}
            >
              {title && (
                <div className="text-[11px] font-semibold tracking-[0.06em] text-[color:var(--color-cyan-300)] mb-1">
                  {title}
                </div>
              )}
              {body && (
                <div className="text-[11px] leading-snug text-[color:var(--color-text-primary)] whitespace-pre-line">
                  {body}
                </div>
              )}
              {source && (
                <div className="mt-1.5 pt-1.5 border-t border-[color:var(--color-border-hair)] text-[9px] tracking-[0.08em] uppercase text-[color:var(--color-text-muted)] font-mono">
                  {source}
                </div>
              )}
            </div>
          </div>,
          document.body,
        )}
    </>
  );
}

/**
 * `<HintLabel id="...">Label Text</HintLabel>` — wraps a label with a trailing
 * info icon. The most common usage pattern.
 */
export function HintLabel({
  id,
  children,
  className,
  iconSize = 11,
}: {
  id: FieldHintId;
  children: React.ReactNode;
  className?: string;
  iconSize?: number;
}) {
  return (
    <span className={cn("inline-flex items-center gap-1", className)}>
      <span>{children}</span>
      <FieldHint id={id} size={iconSize} />
    </span>
  );
}
