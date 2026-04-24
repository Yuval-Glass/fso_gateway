"use client";

interface TextFieldProps {
  label: string;
  value: string;
  onChange: (v: string) => void;
  placeholder?: string;
  maxLength?: number;
  mono?: boolean;
}

export function TextField({
  label,
  value,
  onChange,
  placeholder,
  maxLength,
  mono = false,
}: TextFieldProps) {
  return (
    <div>
      <label className="block text-[10px] font-medium tracking-[0.2em] uppercase text-[color:var(--color-text-secondary)] mb-1">
        {label}
      </label>
      <input
        type="text"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={placeholder}
        maxLength={maxLength}
        spellCheck={false}
        className={`w-full bg-white/[0.02] border border-[color:var(--color-border-subtle)] rounded-md px-3 py-1.5 text-sm text-[color:var(--color-text-primary)] outline-none focus:border-[color:var(--color-cyan-500)] focus:ring-1 focus:ring-[color:var(--color-cyan-500)]/40 ${mono ? "font-mono tabular" : ""}`}
      />
    </div>
  );
}
