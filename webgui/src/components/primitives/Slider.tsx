"use client";

import { useEffect, useRef, useState } from "react";
import { cn } from "@/lib/utils";
import { FieldHint } from "./FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";

interface SliderProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange: (v: number) => void;
  unit?: string;
  /** Optional one-line legend shown below the slider. */
  hint?: string;
  /** Tooltip on the label, looked up in FIELD_HINTS. */
  hintId?: FieldHintId;
  /** Fired when the user presses Enter on the slider or commits the inline
   *  number editor. The new value is passed explicitly so callers can save
   *  it directly without hitting React's setState batching race. */
  onCommit?: (value: number) => void;
}

export function Slider({
  label,
  value,
  min,
  max,
  step = 1,
  onChange,
  unit,
  hint,
  hintId,
  onCommit,
}: SliderProps) {
  const pct = Math.min(100, Math.max(0, ((value - min) / (max - min)) * 100));
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(String(value));
  const inputRef = useRef<HTMLInputElement | null>(null);

  // Keep draft text in sync when not editing.
  useEffect(() => {
    if (!editing) setDraft(String(value));
  }, [value, editing]);

  // Auto-select the text when entering edit mode.
  useEffect(() => {
    if (editing) inputRef.current?.select();
  }, [editing]);

  const commitDraft = (alsoFireCommit: boolean) => {
    const parsed = Number(draft);
    if (Number.isFinite(parsed)) {
      const clamped = Math.min(max, Math.max(min, Math.round(parsed / step) * step));
      if (clamped !== value) onChange(clamped);
      setDraft(String(clamped));
      if (alsoFireCommit) onCommit?.(clamped);
    } else {
      setDraft(String(value));
    }
    setEditing(false);
  };

  return (
    <div className="space-y-1.5">
      <div className="flex items-baseline justify-between">
        <label className="text-[10px] font-medium tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)] inline-flex items-center gap-1">
          <span>{label}</span>
          {hintId && <FieldHint id={hintId} size={11} />}
        </label>
        <div className="flex items-baseline gap-1">
          {editing ? (
            <input
              ref={inputRef}
              type="number"
              inputMode="numeric"
              min={min}
              max={max}
              step={step}
              value={draft}
              onChange={(e) => setDraft(e.target.value)}
              onBlur={() => commitDraft(false)}
              onKeyDown={(e) => {
                if (e.key === "Enter") {
                  e.preventDefault();
                  commitDraft(true);
                } else if (e.key === "Escape") {
                  e.preventDefault();
                  setDraft(String(value));
                  setEditing(false);
                }
              }}
              className="font-display text-base font-semibold tabular text-[color:var(--color-cyan-300)] bg-transparent border-b border-[color:var(--color-border-cyan)] outline-none w-20 text-right [appearance:textfield] [&::-webkit-inner-spin-button]:appearance-none [&::-webkit-outer-spin-button]:appearance-none"
            />
          ) : (
            <button
              type="button"
              onClick={() => setEditing(true)}
              title="Click to type a value · Enter to apply"
              className="font-display text-base font-semibold tabular text-[color:var(--color-cyan-300)] hover:underline decoration-[color:var(--color-cyan-300)] underline-offset-2 cursor-text"
            >
              {value}
            </button>
          )}
          {unit && (
            <span className="text-[10px] text-[color:var(--color-text-muted)]">{unit}</span>
          )}
        </div>
      </div>
      <div className="relative h-6 flex items-center">
        {/* Track */}
        <div className="absolute inset-x-0 h-1.5 rounded-full bg-white/[0.04] border border-[color:var(--color-border-hair)]" />
        {/* Fill */}
        <div
          className="absolute h-1.5 rounded-full pointer-events-none"
          style={{
            width: `${pct}%`,
            background:
              "linear-gradient(90deg, var(--color-cyan-600), var(--color-cyan-500))",
            boxShadow: "0 0 10px rgba(0, 212, 255, 0.45)",
          }}
        />
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={value}
          onChange={(e) => onChange(Number(e.target.value))}
          onKeyDown={(e) => {
            // Pressing Enter while focused on the slider commits the change
            // (typically wired to a save action by the parent form).
            if (e.key === "Enter") {
              e.preventDefault();
              onCommit?.(value);
            }
          }}
          className={cn(
            "relative w-full h-6 appearance-none bg-transparent cursor-pointer",
            "focus:outline-none",
            "[&::-webkit-slider-thumb]:appearance-none",
            "[&::-webkit-slider-thumb]:w-4",
            "[&::-webkit-slider-thumb]:h-4",
            "[&::-webkit-slider-thumb]:rounded-full",
            "[&::-webkit-slider-thumb]:bg-[color:var(--color-cyan-300)]",
            "[&::-webkit-slider-thumb]:border-2",
            "[&::-webkit-slider-thumb]:border-[color:var(--color-cyan-700)]",
            "[&::-webkit-slider-thumb]:shadow-[0_0_10px_rgba(0,212,255,0.8)]",
            "[&::-webkit-slider-thumb]:cursor-grab",
            "[&::-webkit-slider-thumb]:active:cursor-grabbing",
            "[&::-moz-range-thumb]:w-4",
            "[&::-moz-range-thumb]:h-4",
            "[&::-moz-range-thumb]:rounded-full",
            "[&::-moz-range-thumb]:bg-[color:var(--color-cyan-300)]",
            "[&::-moz-range-thumb]:border-2",
            "[&::-moz-range-thumb]:border-[color:var(--color-cyan-700)]",
            "[&::-moz-range-thumb]:border-none",
          )}
        />
      </div>
      <div className="flex justify-between text-[9px] font-mono text-[color:var(--color-text-muted)]">
        <span>{min}</span>
        {hint && <span className="tracking-widest uppercase">{hint}</span>}
        <span>{max}</span>
      </div>
    </div>
  );
}
