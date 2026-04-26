"use client";

import { Minus, Plus } from "lucide-react";

interface NumberFieldProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange: (v: number) => void;
  unit?: string;
}

function clamp(v: number, lo: number, hi: number) {
  return Math.max(lo, Math.min(hi, v));
}

export function NumberField({
  label,
  value,
  min,
  max,
  step = 1,
  onChange,
  unit,
}: NumberFieldProps) {
  const apply = (v: number) => onChange(clamp(Math.round(v / step) * step, min, max));

  return (
    <div>
      <label className="block text-[10px] font-medium tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)] mb-1">
        {label}
      </label>
      <div className="flex items-stretch gap-0 rounded-md border border-[color:var(--color-border-subtle)] bg-white/[0.02] overflow-hidden">
        <button
          type="button"
          onClick={() => apply(value - step)}
          disabled={value <= min}
          aria-label="Decrement"
          className="w-8 flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.03] disabled:opacity-30 disabled:cursor-not-allowed"
        >
          <Minus size={12} />
        </button>
        <input
          type="number"
          inputMode="numeric"
          value={value}
          min={min}
          max={max}
          step={step}
          onChange={(e) => {
            const n = Number(e.target.value);
            if (Number.isFinite(n)) apply(n);
          }}
          className="flex-1 bg-transparent text-center font-mono text-sm tabular text-[color:var(--color-text-primary)] outline-none border-x border-[color:var(--color-border-hair)] focus:border-[color:var(--color-cyan-500)] py-1.5 [appearance:textfield] [&::-webkit-outer-spin-button]:appearance-none [&::-webkit-inner-spin-button]:appearance-none"
        />
        <button
          type="button"
          onClick={() => apply(value + step)}
          disabled={value >= max}
          aria-label="Increment"
          className="w-8 flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.03] disabled:opacity-30 disabled:cursor-not-allowed"
        >
          <Plus size={12} />
        </button>
      </div>
      {unit && (
        <div className="text-[9px] text-right font-mono text-[color:var(--color-text-muted)] mt-0.5">
          {unit}
        </div>
      )}
    </div>
  );
}
