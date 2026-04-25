"use client";

import { cn } from "@/lib/utils";
import { FieldHint } from "./FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";

interface ToggleProps {
  label: string;
  checked: boolean;
  onChange: (v: boolean) => void;
  description?: string;
  hintId?: FieldHintId;
}

export function Toggle({ label, checked, onChange, description, hintId }: ToggleProps) {
  // Explicit pixel layout — bypasses any Tailwind arbitrary-value variance.
  //   Pill outer:  46×24 (border 1px each side → inner 44×22).
  //   Knob:        18×18, sits 2px from top.
  //   Off:         left = 3   (3..21  → 3px clearance from right inner edge)
  //   On:          left = 25  (25..43 → 1px clearance from right inner edge)
  const PILL_W = 46;
  const PILL_H = 24;
  const KNOB = 18;
  const KNOB_OFF_LEFT = 3;
  const KNOB_ON_LEFT = PILL_W - KNOB - KNOB_OFF_LEFT - 2; // = 23

  return (
    <label className="flex items-start gap-4 cursor-pointer select-none group">
      <button
        type="button"
        role="switch"
        aria-checked={checked}
        onClick={() => onChange(!checked)}
        className={cn(
          "relative shrink-0 rounded-full border transition-colors duration-200",
          checked
            ? "bg-[color:var(--color-cyan-900)] border-[color:var(--color-border-cyan)]"
            : "bg-white/[0.03] border-[color:var(--color-border-subtle)]",
        )}
        style={{
          width: PILL_W,
          height: PILL_H,
          boxShadow: checked
            ? "inset 0 0 10px rgba(0, 212, 255, 0.35)"
            : undefined,
        }}
      >
        <span
          className="absolute rounded-full transition-[left,background-color,box-shadow] duration-200"
          style={{
            top: (PILL_H - 2 - KNOB) / 2, // vertically centered inside the inner area
            width: KNOB,
            height: KNOB,
            left: checked ? KNOB_ON_LEFT : KNOB_OFF_LEFT,
            background: checked
              ? "var(--color-cyan-300)"
              : "var(--color-text-muted)",
            boxShadow: checked
              ? "0 0 6px rgba(0, 212, 255, 0.7)"
              : undefined,
          }}
        />
      </button>
      <span className="flex-1 min-w-0">
        <span className="block text-xs font-medium tracking-[0.1em] uppercase text-[color:var(--color-text-primary)]">
          <span>{label}</span>
          {hintId && <FieldHint id={hintId} size={11} className="ml-1" />}
        </span>
        {description && (
          <span className="block text-[10px] text-[color:var(--color-text-muted)] mt-0.5 leading-tight">
            {description}
          </span>
        )}
      </span>
    </label>
  );
}
