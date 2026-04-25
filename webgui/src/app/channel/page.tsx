"use client";

import { AlertTriangle, CloudLightning, Eraser, Play, Sparkles } from "lucide-react";
import { useEffect, useState } from "react";
import { GlassPanel } from "@/components/primitives/GlassPanel";
import { Slider } from "@/components/primitives/Slider";
import { TactileButton } from "@/components/primitives/TactileButton";
import { TextField } from "@/components/primitives/TextField";
import { useChannel } from "@/lib/useChannel";
import { cn, formatPercent } from "@/lib/utils";

/**
 * Channel Impairment — drives `tc qdisc ... netem loss gemodel ...` on the
 * FSO interface so you can watch the FEC layer respond to controlled burst
 * loss without leaving the GUI. Mirrors the env-var workflow in
 * scripts/two_machine_run_test.sh.
 *
 * Requires `tc` on PATH. The bridge is not normally root; if your tc
 * needs sudo, start the bridge with `FSO_TC_SUDO=1` (and pre-grant the
 * sudo entry) or run the bridge as root in the lab.
 */

interface PresetCfg {
  id: string;
  name: string;
  description: string;
  enterPct: number;
  exitPct: number;
  lossPct: number;
}

const PRESETS: PresetCfg[] = [
  { id: "clear",    name: "Clear",            description: "No artificial loss — baseline.",      enterPct: 0,  exitPct: 0,  lossPct: 0 },
  { id: "drizzle",  name: "Drizzle",          description: "Sparse single-symbol losses.",        enterPct: 1,  exitPct: 70, lossPct: 0.5 },
  { id: "weather",  name: "Weather",          description: "Moderate fades — well within FEC.",   enterPct: 5,  exitPct: 50, lossPct: 5 },
  { id: "haze",     name: "Haze",             description: "Sustained low-level loss.",           enterPct: 8,  exitPct: 30, lossPct: 8 },
  { id: "storm",    name: "Storm",            description: "Heavy bursts — pushes FEC budget.",   enterPct: 12, exitPct: 25, lossPct: 20 },
  { id: "blackout", name: "Blackout (severe)",description: "Pathological loss — likely unrecoverable.", enterPct: 30, exitPct: 10, lossPct: 50 },
];

export default function ChannelImpairmentPage() {
  const ch = useChannel();
  const [enter, setEnter] = useState(5);
  const [exit, setExit]   = useState(50);
  const [loss, setLoss]   = useState(5);
  const [activePreset, setActivePreset] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  // Sync sliders to current state when polled.
  useEffect(() => {
    if (!ch.state) return;
    if (ch.state.active) {
      if (ch.state.enterPct != null) setEnter(ch.state.enterPct);
      if (ch.state.exitPct != null) setExit(ch.state.exitPct);
      if (ch.state.lossPct != null) setLoss(ch.state.lossPct);
    }
  }, [ch.state?.active, ch.state?.lossPct]);

  const apply = async () => {
    setBusy(true);
    await ch.apply(enter, exit, loss);
    setBusy(false);
  };
  const clear = async () => {
    setBusy(true);
    await ch.clear();
    setBusy(false);
  };
  const usePreset = (p: PresetCfg) => {
    setEnter(p.enterPct);
    setExit(p.exitPct);
    setLoss(p.lossPct);
    setActivePreset(p.id);
  };

  const tcAvailable = ch.state?.available ?? true;

  return (
    <div className="flex flex-col gap-5">
      <div className="flex items-baseline justify-between">
        <div>
          <div className="text-[10px] tracking-[0.3em] uppercase text-[color:var(--color-cyan-300)]">
            Mission Control
          </div>
          <h2 className="font-display text-2xl font-bold tracking-tight text-[color:var(--color-text-primary)] mt-0.5">
            Channel Impairment
          </h2>
          <div className="text-xs text-[color:var(--color-text-secondary)] mt-1">
            Apply Linux <span className="font-mono">netem</span> Gilbert-Elliott
            burst-loss to the FSO interface and watch the FEC layer respond live.
            The GUI runs the same{" "}
            <span className="font-mono">tc qdisc add … netem loss gemodel</span>{" "}
            you would invoke from{" "}
            <span className="font-mono">scripts/two_machine_run_test.sh</span>.
          </div>
        </div>
      </div>

      {!tcAvailable && (
        <GlassPanel>
          <div className="flex items-start gap-3 py-2 text-[12px]"
               style={{ color: "var(--color-warning)" }}>
            <AlertTriangle size={16} className="mt-0.5 shrink-0" />
            <span>
              The bridge could not find <span className="font-mono">tc</span> on its
              PATH. Install <span className="font-mono">iproute2</span> on the
              bridge host and (if running as a non-root user) start the bridge
              with <span className="font-mono">FSO_TC_SUDO=1</span> after
              granting <span className="font-mono">sudo tc</span> NOPASSWD.
              Until then this page is read-only.
            </span>
          </div>
        </GlassPanel>
      )}

      {ch.error && (
        <GlassPanel>
          <div className="flex items-start gap-3 py-2 text-[12px]"
               style={{ color: "var(--color-danger)" }}>
            <AlertTriangle size={16} className="mt-0.5 shrink-0" />
            <span>{ch.error}</span>
          </div>
        </GlassPanel>
      )}

      <div className="grid grid-cols-1 xl:grid-cols-[2fr_1fr] gap-4">
        {/* ---------------- Sliders ---------------- */}
        <GlassPanel
          label="Gilbert-Elliott Parameters"
          trailing={
            <span
              className="text-[10px] tracking-[0.18em] uppercase font-mono"
              style={{
                color: ch.state?.active ? "var(--color-warning)" : "var(--color-success)",
              }}
            >
              {ch.state?.active ? "active" : "clear"}
            </span>
          }
        >
          <div className="flex flex-col gap-5 pt-1">
            <Slider
              label="Enter Burst (P_GB)"
              value={enter} min={0} max={100} step={0.5}
              onChange={(v) => { setEnter(v); setActivePreset(null); }}
              unit="%"
              hint="Good→Bad transition"
            />
            <Slider
              label="Exit Burst (P_BG)"
              value={exit} min={0} max={100} step={0.5}
              onChange={(v) => { setExit(v); setActivePreset(null); }}
              unit="%"
              hint="Bad→Good transition"
            />
            <Slider
              label="Loss in Bad"
              value={loss} min={0} max={100} step={0.5}
              onChange={(v) => { setLoss(v); setActivePreset(null); }}
              unit="%"
              hint="loss probability while in bad state"
            />
          </div>

          <div className="mt-5 grid grid-cols-3 gap-3">
            <DerivedTile label="Avg Burst Length" value={`${(100 / Math.max(0.1, exit)).toFixed(1)} pkts`} />
            <DerivedTile label="Bad Fraction" value={formatPercent(enter / Math.max(0.001, enter + exit), 2)} />
            <DerivedTile label="Avg Loss" value={formatPercent((enter / Math.max(0.001, enter + exit)) * (loss / 100), 3)} />
          </div>

          <div className="mt-4 flex items-center gap-2">
            <TactileButton variant="primary" icon={<Play size={13} />}
                           onClick={apply} loading={busy} disabled={!tcAvailable || busy}>
              Apply on {ch.state?.iface ?? ch.iface}
            </TactileButton>
            <TactileButton variant="secondary" icon={<Eraser size={13} />}
                           onClick={clear} loading={busy} disabled={!tcAvailable || busy}>
              Clear netem
            </TactileButton>
          </div>

          <div className="mt-4 text-[10px] text-[color:var(--color-text-muted)] leading-snug">
            Translates to:{" "}
            <span className="font-mono">tc qdisc replace dev {ch.state?.iface ?? ch.iface} root netem loss gemodel {enter}% {exit}% {loss}% 0%</span>
          </div>
        </GlassPanel>

        {/* ---------------- Presets ---------------- */}
        <GlassPanel label="Presets" variant="cyan">
          <ul className="space-y-2 py-1">
            {PRESETS.map((p) => {
              const active = activePreset === p.id;
              return (
                <li key={p.id}>
                  <button
                    onClick={() => usePreset(p)}
                    className={cn(
                      "w-full text-left px-3 py-2 rounded-md border transition-all",
                      active
                        ? "border-[color:var(--color-border-cyan)] bg-[color:var(--color-cyan-900)]/40"
                        : "border-[color:var(--color-border-subtle)] bg-white/[0.02] hover:bg-white/[0.04]",
                    )}
                  >
                    <div className="text-xs font-semibold tracking-[0.12em] uppercase text-[color:var(--color-text-primary)]">
                      {p.name}
                    </div>
                    <div className="text-[10px] text-[color:var(--color-text-secondary)] mt-0.5">
                      {p.description}
                    </div>
                    <div className="mt-1 text-[9px] font-mono text-[color:var(--color-text-muted)]">
                      enter={p.enterPct}% · exit={p.exitPct}% · loss={p.lossPct}%
                    </div>
                  </button>
                </li>
              );
            })}
          </ul>
        </GlassPanel>
      </div>

      {/* ---------------- Target iface + raw output ---------------- */}
      <GlassPanel
        label="Target Interface"
        trailing={
          <span className="text-[10px] tracking-[0.18em] uppercase text-[color:var(--color-text-muted)]">
            override the default with the FSO_IFACE env var on the bridge
          </span>
        }
      >
        <div className="flex items-end gap-3 pt-1">
          <div className="flex-1 max-w-[260px]">
            <TextField
              label="FSO Interface"
              value={ch.iface}
              onChange={ch.setIface}
              placeholder="enp1s0f1np1"
              maxLength={31}
              mono
            />
          </div>
          <div className="flex-1">
            <div className="text-[9px] tracking-[0.22em] uppercase text-[color:var(--color-text-muted)] mb-1">
              tc qdisc raw
            </div>
            <pre className="font-mono text-[11px] text-[color:var(--color-text-primary)] bg-black/30 rounded px-3 py-2 max-h-[80px] overflow-auto whitespace-pre-wrap">
{ch.state?.raw || (ch.loading ? "loading…" : "(no output)")}
            </pre>
          </div>
        </div>
      </GlassPanel>

      <GlassPanel label="What this drives" variant="raised">
        <div className="flex items-start gap-3 py-1">
          <CloudLightning size={20} className="text-[color:var(--color-cyan-300)] shrink-0 mt-0.5" />
          <div className="text-[11px] leading-relaxed text-[color:var(--color-text-secondary)]">
            The Linux <span className="font-mono">netem</span> qdisc is applied
            to the FSO-side interface, so loss is injected on the wire between
            the two gateways. The TX side does not see the impairment;
            the RX side will report it through the deinterleaver and FEC counters
            on the dashboard. Combined with <span className="font-mono">stats_set_burst_fec_span(m × depth)</span>{" "}
            in <span className="font-mono">gateway.c</span>, you get a live read on
            how many bursts are recoverable vs how many exceed the FEC span.
          </div>
        </div>
      </GlassPanel>
      <Sparkles className="hidden" size={1} />
    </div>
  );
}

function DerivedTile({ label, value }: { label: string; value: string }) {
  return (
    <div className="glass rounded-md px-3 py-2 border-[color:var(--color-border-hair)]">
      <div className="text-[9px] tracking-[0.2em] uppercase text-[color:var(--color-text-muted)]">
        {label}
      </div>
      <div className="font-display text-base font-semibold tabular mt-0.5 text-[color:var(--color-cyan-300)]">
        {value}
      </div>
    </div>
  );
}
