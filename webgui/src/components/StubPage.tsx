import { GlassPanel } from "@/components/primitives/GlassPanel";
import type { LucideIcon } from "lucide-react";

interface StubPageProps {
  icon: LucideIcon;
  title: string;
  tagline: string;
  phase: string;
  features: string[];
}

export function StubPage({ icon: Icon, title, tagline, phase, features }: StubPageProps) {
  return (
    <div className="flex flex-col gap-5">
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            {title}
          </h2>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Planned · {phase}
        </div>
      </div>

      <GlassPanel variant="raised" padded={false} className="overflow-hidden">
        <div className="relative scanlines">
          <div className="flex flex-col items-center justify-center text-center px-10 py-16 gap-5">
            <div className="relative">
              <div
                className="absolute inset-0 rounded-full pulse-ring"
                style={{ border: "2px solid var(--color-cyan-500)", opacity: 0.4 }}
              />
              <div
                className="w-20 h-20 rounded-full glass glass-cyan flex items-center justify-center"
                style={{ boxShadow: "0 0 30px rgba(0, 212, 255, 0.25)" }}
              >
                <Icon size={34} strokeWidth={1.5} className="text-[color:var(--color-cyan-300)] drop-shadow-[0_0_8px_rgba(0,212,255,0.6)]" />
              </div>
            </div>
            <div className="max-w-md">
              <div className="font-display text-xl font-semibold text-[color:var(--color-text-primary)]">
                {tagline}
              </div>
              <div className="mt-2 text-sm text-[color:var(--color-text-secondary)]">
                This module is scheduled for {phase.toLowerCase()}. The current build is focused on the
                live dashboard shell and visual system.
              </div>
            </div>
          </div>
        </div>
      </GlassPanel>

      <GlassPanel label="Planned Capabilities">
        <ul className="grid grid-cols-1 md:grid-cols-2 gap-x-8 gap-y-2 py-1">
          {features.map((f) => (
            <li key={f} className="flex items-start gap-2.5 text-sm text-[color:var(--color-text-secondary)]">
              <span
                className="mt-1.5 w-1.5 h-1.5 rounded-full shrink-0"
                style={{
                  background: "var(--color-cyan-500)",
                  boxShadow: "0 0 6px rgba(0, 212, 255, 0.7)",
                }}
              />
              <span>{f}</span>
            </li>
          ))}
        </ul>
      </GlassPanel>
    </div>
  );
}
