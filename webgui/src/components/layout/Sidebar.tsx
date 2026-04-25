"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import {
  Activity,
  AlertTriangle,
  BarChart3,
  Boxes,
  CloudLightning,
  Cpu,
  FileText,
  Info,
  LayoutDashboard,
  Network,
  PackageSearch,
  Settings,
  Signal,
  type LucideIcon,
} from "lucide-react";
import { cn } from "@/lib/utils";
import { BrandMark } from "./BrandMark";

interface NavItem {
  label: string;
  href: string;
  icon: LucideIcon;
  disabled?: boolean;
}

const primaryNav: NavItem[] = [
  { label: "Dashboard", href: "/", icon: LayoutDashboard },
  { label: "Link Status", href: "/link-status", icon: Signal },
  { label: "Traffic", href: "/traffic", icon: Activity },
  { label: "FEC Analytics", href: "/fec-analytics", icon: BarChart3 },
  { label: "Interleaver", href: "/interleaver", icon: Boxes },
  { label: "Packet Inspector", href: "/packet-inspector", icon: PackageSearch },
  { label: "Channel", href: "/channel", icon: CloudLightning },
  { label: "Configuration", href: "/configuration", icon: Settings },
  { label: "System Logs", href: "/logs", icon: FileText },
  { label: "Alerts", href: "/alerts", icon: AlertTriangle },
  { label: "Topology", href: "/topology", icon: Network },
  { label: "Analytics", href: "/analytics", icon: Cpu },
  { label: "About", href: "/about", icon: Info },
];

export function Sidebar() {
  const pathname = usePathname();

  return (
    <aside className="w-60 shrink-0 flex flex-col h-screen sticky top-0 z-20">
      <div className="glass-raised rounded-none border-l-0 border-t-0 border-b-0 flex-1 flex flex-col">
        {/* Brand block — compact so all nav items fit on short screens */}
        <div className="px-3 py-2 border-b border-[color:var(--color-border-hair)] flex items-center gap-2">
          <BrandMark size={36} />
          <div className="min-w-0 leading-tight">
            <div className="text-[10px] font-semibold tracking-[0.2em] text-[color:var(--color-text-primary)]">
              BER KILLERZ
            </div>
            <div className="text-[8px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
              FSO Gateway
            </div>
          </div>
        </div>

        {/* Nav */}
        <nav className="flex-1 py-2 px-2 overflow-y-auto">
          <div className="px-3 pb-1.5 text-[9px] font-medium tracking-[0.25em] uppercase text-[color:var(--color-text-muted)]">
            Mission Control
          </div>
          <ul className="space-y-0.5">
            {primaryNav.map((item) => {
              const active = pathname === item.href;
              const Icon = item.icon;
              const content = (
                <div
                  className={cn(
                    "group flex items-center gap-3 px-3 py-1.5 rounded-md text-xs font-medium transition-all",
                    "border border-transparent",
                    active
                      ? "bg-[color:var(--color-cyan-900)]/60 border-[color:var(--color-border-cyan)] text-[color:var(--color-cyan-300)]"
                      : "text-[color:var(--color-text-secondary)] hover:bg-white/[0.03] hover:text-[color:var(--color-text-primary)]",
                    item.disabled && "opacity-40 cursor-not-allowed hover:bg-transparent hover:text-[color:var(--color-text-secondary)]",
                  )}
                  style={
                    active
                      ? { boxShadow: "inset 2px 0 0 var(--color-cyan-500)" }
                      : undefined
                  }
                >
                  <Icon size={15} strokeWidth={1.8} className={active ? "drop-shadow-[0_0_6px_rgba(0,212,255,0.6)]" : ""} />
                  <span className="tracking-[0.08em] uppercase">{item.label}</span>
                  {item.disabled && (
                    <span className="ml-auto text-[8px] tracking-widest text-[color:var(--color-text-dim)]">
                      SOON
                    </span>
                  )}
                </div>
              );
              return (
                <li key={item.href}>
                  {item.disabled ? (
                    <div aria-disabled>{content}</div>
                  ) : (
                    <Link href={item.href}>{content}</Link>
                  )}
                </li>
              );
            })}
          </ul>
        </nav>

        {/* Compact footer — single-line health bar (keeps nav list visible) */}
        <div className="px-3 py-2 border-t border-[color:var(--color-border-hair)]">
          <div className="flex items-center justify-between mb-1">
            <span className="text-[8px] tracking-[0.25em] uppercase text-[color:var(--color-text-muted)]">
              Health
            </span>
            <span className="font-mono tabular text-[10px] text-[color:var(--color-cyan-300)]">
              98.7%
            </span>
          </div>
          <div className="h-0.5 rounded-full overflow-hidden bg-white/5">
            <div
              className="h-full rounded-full"
              style={{
                width: "98.7%",
                background: "linear-gradient(90deg, var(--color-cyan-500), var(--color-blue-500))",
                boxShadow: "0 0 8px rgba(0,212,255,0.5)",
              }}
            />
          </div>
        </div>
      </div>
    </aside>
  );
}
