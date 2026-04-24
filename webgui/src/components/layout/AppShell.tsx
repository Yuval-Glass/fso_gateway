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
          <span>Gateway ID: FSO-GW-001 · Firmware 3.1.7</span>
          <span>FPGA Accel: <span className="text-[color:var(--color-success)]">Enabled</span></span>
        </footer>
      </div>
    </div>
  );
}
