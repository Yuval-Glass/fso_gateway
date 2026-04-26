import { ReactNode } from "react";
import { Sidebar } from "./Sidebar";
import { TopBar } from "./TopBar";

export function AppShell({ children }: { children: ReactNode }) {
  return (
    <div className="relative flex min-h-screen">
      <Sidebar />
      <div className="flex-1 flex flex-col min-w-0 relative z-[1]">
        <TopBar />
        <main className="flex-1 px-6 py-6">{children}</main>
        <footer className="px-6 py-3 border-t border-[color:var(--color-border-hair)] text-[10px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)] flex items-center justify-between">
          <span>Phase 8 — two-machine FSO demo (Win1 ── GW-A ══ GW-B ── Win2)</span>
          <span>Symbol CRC: <span className="text-[color:var(--color-cyan-300)]">CRC-32C</span></span>
        </footer>
      </div>
    </div>
  );
}
