"use client";

import { X } from "lucide-react";
import Link from "next/link";
import { useEffect, useState } from "react";
import { createPortal } from "react-dom";

interface ChartZoomModalProps {
  title: string;
  /** Optional link to a page where this chart lives in detail. */
  href?: string;
  onClose: () => void;
  children: React.ReactNode;
}

/**
 * Full-screen overlay that hosts an enlarged chart. Closes on:
 *   - Escape key
 *   - Click on the dimmed backdrop
 *   - The X button
 */
export function ChartZoomModal({ title, href, onClose, children }: ChartZoomModalProps) {
  const [mounted, setMounted] = useState(false);
  useEffect(() => {
    setMounted(true);
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    // Lock page scroll while modal is open.
    const prev = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    return () => {
      window.removeEventListener("keydown", onKey);
      document.body.style.overflow = prev;
    };
  }, [onClose]);

  if (!mounted) return null;

  return createPortal(
    <div
      className="fixed inset-0 z-[300] flex items-center justify-center p-6 bg-black/75 backdrop-blur-sm animate-[fade-in_140ms_ease-out]"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
      aria-label={title}
    >
      <div
        // Explicit height (not just max-h) so the modal always claims the
        // full vertical envelope — the inner chart is `height: 100%` and
        // would otherwise collapse to its intrinsic size.
        className="glass-raised rounded-lg w-full max-w-[1500px] h-[88vh] flex flex-col"
        style={{ background: "rgba(8, 14, 28, 0.95)" }}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-5 py-3 border-b border-[color:var(--color-border-hair)]">
          <h2 className="text-[12px] font-semibold tracking-[0.22em] uppercase text-[color:var(--color-text-primary)] inline-flex items-center gap-3">
            {href ? (
              <Link
                href={href}
                className="hover:text-[color:var(--color-cyan-300)] transition-colors"
                onClick={onClose}
              >
                {title}
              </Link>
            ) : (
              <span>{title}</span>
            )}
            {href && (
              <Link
                href={href}
                onClick={onClose}
                className="text-[9px] font-mono tracking-[0.18em] uppercase text-[color:var(--color-text-muted)] hover:text-[color:var(--color-cyan-300)]"
              >
                Open page →
              </Link>
            )}
          </h2>
          <button
            type="button"
            onClick={onClose}
            aria-label="Close"
            className="w-8 h-8 rounded-md flex items-center justify-center text-[color:var(--color-text-secondary)] hover:text-[color:var(--color-cyan-300)] hover:bg-white/[0.05] transition-colors"
          >
            <X size={16} strokeWidth={1.8} />
          </button>
        </div>
        <div className="flex-1 min-h-0 p-5 overflow-hidden">{children}</div>
      </div>
    </div>,
    document.body,
  );
}
