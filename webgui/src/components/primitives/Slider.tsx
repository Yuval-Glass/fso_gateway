"use client";

import { cn } from "@/lib/utils";

interface SliderProps {
  label: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  onChange: (v: number) => void;
  unit?: string;
  hint?: string;
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
}: SliderProps) {
  const pct = ((value - min) / (max - min)) * 100;
  return (
    <div className="space-y-1.5">
      <div className="flex items-baseline justify-between">
        <label className="text-[10px] font-medium tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)]">
          {label}
        </label>
        <div className="flex items-baseline gap-1">
          <span className="font-display text-base font-semibold tabular text-[color:var(--color-cyan-300)]">
            {value}
          </span>
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
