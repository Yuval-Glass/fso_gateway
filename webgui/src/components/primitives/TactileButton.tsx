"use client";

import type { ButtonHTMLAttributes, ReactNode } from "react";
import { cn } from "@/lib/utils";

type Variant = "primary" | "secondary" | "danger" | "ghost";

interface TactileButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: Variant;
  icon?: ReactNode;
  loading?: boolean;
}

const variants: Record<Variant, string> = {
  primary:
    "bg-[color:var(--color-cyan-900)]/50 border-[color:var(--color-border-cyan)] text-[color:var(--color-cyan-300)] hover:bg-[color:var(--color-cyan-900)]/80 hover:border-[color:var(--color-cyan-500)]",
  secondary:
    "bg-white/[0.03] border-[color:var(--color-border-subtle)] text-[color:var(--color-text-secondary)] hover:bg-white/[0.06] hover:text-[color:var(--color-text-primary)]",
  danger:
    "bg-[color:var(--color-danger)]/10 border-[color:var(--color-border-danger)] text-[color:var(--color-danger)] hover:bg-[color:var(--color-danger)]/20",
  ghost:
    "bg-transparent border-transparent text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-text-primary)] hover:bg-white/[0.03]",
};

const glows: Record<Variant, string> = {
  primary: "0 0 20px rgba(0, 212, 255, 0.35)",
  secondary: "none",
  danger: "0 0 18px rgba(255, 45, 92, 0.35)",
  ghost: "none",
};

export function TactileButton({
  variant = "primary",
  icon,
  loading,
  children,
  className,
  disabled,
  ...rest
}: TactileButtonProps) {
  return (
    <button
      {...rest}
      disabled={disabled || loading}
      className={cn(
        "inline-flex items-center justify-center gap-2 px-4 py-2 rounded-md border",
        "text-xs font-semibold tracking-[0.18em] uppercase",
        "transition-all duration-200 select-none",
        "disabled:opacity-40 disabled:cursor-not-allowed",
        "active:scale-[0.98]",
        variants[variant],
        className,
      )}
      style={!disabled && !loading ? { boxShadow: glows[variant] } : undefined}
    >
      {loading ? (
        <span className="w-3 h-3 rounded-full border-2 border-current border-t-transparent animate-spin" />
      ) : (
        icon
      )}
      <span>{children}</span>
    </button>
  );
}
