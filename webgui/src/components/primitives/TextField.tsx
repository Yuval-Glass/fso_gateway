"use client";

import { FieldHint } from "./FieldHint";
import type { FieldHintId } from "@/lib/fieldHints";

interface TextFieldProps {
  label: string;
  value: string;
  onChange: (v: string) => void;
  placeholder?: string;
  maxLength?: number;
  mono?: boolean;
  hintId?: FieldHintId;
  /** Fires when the user presses Enter — typically wired to a save action.
   *  The current value is passed so callers can avoid React's setState
   *  batching race when calling save() right after update(). */
  onCommit?: (value: string) => void;
}

export function TextField({
  label,
  value,
  onChange,
  placeholder,
  maxLength,
  mono = false,
  hintId,
  onCommit,
}: TextFieldProps) {
  return (
    <div>
      <label className="block text-[10px] font-medium tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)] mb-1">
        <span>{label}</span>
        {hintId && <FieldHint id={hintId} size={11} className="ml-1" />}
      </label>
      <input
        type="text"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Enter") {
            e.preventDefault();
            onCommit?.(value);
          }
        }}
        placeholder={placeholder}
        maxLength={maxLength}
        spellCheck={false}
        className={`w-full bg-white/[0.02] border border-[color:var(--color-border-subtle)] rounded-md px-3 py-1.5 text-sm text-[color:var(--color-text-primary)] outline-none focus:border-[color:var(--color-cyan-500)] focus:ring-1 focus:ring-[color:var(--color-cyan-500)]/40 ${mono ? "font-mono tabular" : ""}`}
      />
    </div>
  );
}
