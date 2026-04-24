"use client";

import { cn } from "@/lib/utils";

interface ToggleProps {
  label: string;
  checked: boolean;
  onChange: (v: boolean) => void;
  description?: string;
}

export function Toggle({ label, checked, onChange, description }: ToggleProps) {
  return (
    <label className="flex items-start gap-3 cursor-pointer select-none group">
      <button
        type="button"
        role="switch"
        aria-checked={checked}
        onClick={() => onChange(!checked)}
        className={cn(
          "relative shrink-0 w-10 h-[22px] rounded-full border transition-colors duration-200",
          checked
            ? "bg-[color:var(--color-cyan-900)] border-[color:var(--color-border-cyan)]"
            : "bg-white/[0.03] border-[color:var(--color-border-subtle)]",
        )}
        style={
          checked
            ? { boxShadow: "inset 0 0 10px rgba(0, 212, 255, 0.35)" }
            : undefined
        }
      >
        <span
          className={cn(
            "absolute top-[2px] w-[16px] h-[16px] rounded-full transition-all duration-200",
            checked
              ? "translate-x-[20px] bg-[color:var(--color-cyan-300)]"
              : "translate-x-[2px] bg-[color:var(--color-text-muted)]",
          )}
          style={
            checked
              ? { boxShadow: "0 0 8px rgba(0, 212, 255, 0.8)" }
              : undefined
          }
        />
      </button>
      <span className="flex-1 min-w-0">
        <span className="block text-xs font-medium tracking-[0.1em] uppercase text-[color:var(--color-text-primary)]">
          {label}
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
