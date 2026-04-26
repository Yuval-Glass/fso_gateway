"use client";

import { Laptop } from "lucide-react";
import { cn } from "@/lib/utils";

interface ClientBranchProps {
  side: "A" | "B";
  clients: Array<{ name: string; ip: string }>;
  dim?: boolean;
}

export function ClientBranch({ side, clients, dim }: ClientBranchProps) {
  return (
    <div className={cn(
      "flex flex-col items-center gap-3",
      dim && "opacity-50",
    )}>
      {/* Branch lines from endpoint */}
      <div className="relative h-8 w-full flex justify-center">
        <div
          className="absolute top-0 w-px bg-gradient-to-b from-[color:var(--color-cyan-500)]/40 to-transparent"
          style={{ height: "100%" }}
        />
      </div>
      <div className="flex gap-3">
        {clients.map((c) => (
          <div
            key={c.name}
            className="glass rounded-md px-2.5 py-1.5 flex items-center gap-2 min-w-[112px]"
          >
            <Laptop size={14} className="text-[color:var(--color-cyan-300)] shrink-0" />
            <div className="min-w-0">
              <div className="text-[10px] font-semibold tracking-[0.1em] uppercase text-[color:var(--color-text-primary)] truncate">
                {c.name}
              </div>
              <div className="font-mono text-[9px] text-[color:var(--color-text-muted)] truncate">
                {c.ip}
              </div>
            </div>
          </div>
        ))}
      </div>
      <div className="text-[9px] tracking-[0.25em] uppercase text-[color:var(--color-text-muted)]">
        LAN · Node {side} side
      </div>
    </div>
  );
}
