"use client";

import { LogConsole } from "@/components/logs/LogConsole";

export default function LogsPage() {
  return (
    <div className="flex flex-col gap-5">
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            System Logs
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Live stream from the gateway daemon (or a mock feed when the daemon
            is not running). Filter by level or module, search, pause, and
            export.
          </div>
        </div>
        <div className="text-[10px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)]">
          Phase 4A · Live Log Stream
        </div>
      </div>

      <LogConsole />

      <div className="text-[11px] text-[color:var(--color-text-muted)] leading-snug max-w-3xl">
        <span className="text-[color:var(--color-text-secondary)]">Tailing a real gateway:</span>{" "}
        redirect the daemon's stderr to <span className="font-mono">/tmp/fso_gw.log</span> (or set the
        <span className="font-mono"> FSO_LOG_FILE</span> env var before starting the bridge). Example:{" "}
        <span className="font-mono">sudo ./build/bin/fso_gw_runner ... 2&gt;&gt; /tmp/fso_gw.log</span>.
        The bridge switches from mock to tail automatically within ~5 seconds of the file appearing.
      </div>
    </div>
  );
}
