"""
Gateway telemetry source — connects to the C-side control_server UNIX socket
and enriches its raw counter snapshots into the full TelemetrySnapshot shape
expected by the dashboard.

The C side emits a minimal snapshot (atomic counters + config echo) at ~10 Hz.
This module:
  - Maintains a reconnect loop so the bridge keeps trying while the gateway
    daemon is down.
  - Derives throughput rates from byte-count deltas into a rolling history.
  - Synthesizes alerts from counter deltas.
  - Backfills fields the C side does not track (RSSI/SNR/BER from optics →
    null; per-stage queue depths → 0/healthy placeholders for now).
"""

from __future__ import annotations

import asyncio
import json
import time
from typing import Any

RECONNECT_SECONDS = 3.0
HISTORY_SAMPLES = 300
ALERT_RING_MAX = 200


class GatewaySource:
    def __init__(self, socket_path: str = "/tmp/fso_gw.sock") -> None:
        self.path = socket_path
        self._task: asyncio.Task[None] | None = None
        self._stop = asyncio.Event()
        self._connected = False
        self._last_raw: dict[str, Any] | None = None
        self._prev_raw: dict[str, Any] | None = None
        self._history: list[dict[str, Any]] = []
        self._alerts: list[dict[str, Any]] = []

    def is_connected(self) -> bool:
        return self._connected

    def start(self) -> None:
        if self._task is None:
            self._stop.clear()
            self._task = asyncio.create_task(self._run(), name="gateway-source")

    async def stop(self) -> None:
        self._stop.set()
        if self._task:
            try:
                await asyncio.wait_for(self._task, timeout=2.0)
            except asyncio.TimeoutError:
                self._task.cancel()
        self._task = None

    async def _run(self) -> None:
        while not self._stop.is_set():
            try:
                reader, writer = await asyncio.open_unix_connection(self.path)
            except (FileNotFoundError, ConnectionRefusedError, PermissionError, OSError):
                await self._sleep_or_stop(RECONNECT_SECONDS)
                continue

            self._connected = True
            try:
                await self._read_loop(reader)
            except asyncio.CancelledError:
                raise
            except Exception:
                pass
            finally:
                self._connected = False
                writer.close()
                try:
                    await writer.wait_closed()
                except Exception:
                    pass

            # Drop history after disconnect so stale rates don't linger.
            self._prev_raw = None
            self._last_raw = None
            self._history.clear()
            await self._sleep_or_stop(RECONNECT_SECONDS)

    async def _sleep_or_stop(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self._stop.wait(), timeout=seconds)
        except asyncio.TimeoutError:
            return

    async def _read_loop(self, reader: asyncio.StreamReader) -> None:
        while not self._stop.is_set():
            line = await reader.readline()
            if not line:
                return  # EOF → triggers reconnect
            try:
                raw = json.loads(line)
            except json.JSONDecodeError:
                continue
            if raw.get("schema") != "fso-gw-stats/1":
                continue
            self._ingest(raw)

    # ---- State updates -----------------------------------------------------

    def _ingest(self, raw: dict[str, Any]) -> None:
        self._prev_raw = self._last_raw
        self._last_raw = raw
        if self._prev_raw is None:
            return

        prev = self._prev_raw
        stats = raw["stats"]
        prev_stats = prev["stats"]
        dt_s = (raw["ts_ms"] - prev["ts_ms"]) / 1000.0
        if dt_s <= 0.01:
            return

        d_tx_bytes = max(0, stats["transmitted_bytes"] - prev_stats["transmitted_bytes"])
        d_rx_bytes = max(0, stats["recovered_bytes"]   - prev_stats["recovered_bytes"])
        d_tx_pkts  = max(0, stats["transmitted_packets"] - prev_stats["transmitted_packets"])
        d_rx_pkts  = max(0, stats["recovered_packets"]   - prev_stats["recovered_packets"])

        self._history.append({
            "t": raw["ts_ms"],
            "txBps": d_tx_bytes * 8.0 / dt_s,
            "rxBps": d_rx_bytes * 8.0 / dt_s,
            "txPps": d_tx_pkts / dt_s,
            "rxPps": d_rx_pkts / dt_s,
        })
        if len(self._history) > HISTORY_SAMPLES:
            del self._history[: len(self._history) - HISTORY_SAMPLES]

        self._detect_alerts(raw, prev)

    def _detect_alerts(self, raw: dict[str, Any], prev: dict[str, Any]) -> None:
        stats = raw["stats"]
        prev_stats = prev["stats"]
        ts = raw["ts_ms"]

        d_blocks_failed = stats["blocks_failed"] - prev_stats["blocks_failed"]
        if d_blocks_failed >= 3:
            self._push_alert(ts, "critical", "FEC", f"{d_blocks_failed} FEC blocks failed this tick")

        d_crc = stats["symbols_dropped_crc"] - prev_stats["symbols_dropped_crc"]
        if d_crc >= 20:
            self._push_alert(ts, "warning", "CRC", f"{d_crc} symbols dropped on CRC")

        if (stats["max_burst_length"] > prev_stats["max_burst_length"]
                and stats["max_burst_length"] > 20):
            self._push_alert(ts, "warning", "BURST",
                             f"Max burst length climbed to {stats['max_burst_length']}")

    def _push_alert(self, ts: int, severity: str, module: str, message: str) -> None:
        self._alerts.insert(0, {
            "id": f"{ts}-{module}-{len(self._alerts)}",
            "t": ts,
            "severity": severity,
            "module": module,
            "message": message,
        })
        del self._alerts[ALERT_RING_MAX:]

    # ---- Snapshot builder --------------------------------------------------

    def snapshot(self) -> dict[str, Any] | None:
        raw = self._last_raw
        if raw is None:
            return None

        stats = raw["stats"]
        cfg = raw.get("config", {})

        blocks_att = stats["blocks_attempted"]
        blocks_fail = stats["blocks_failed"]
        flr = (blocks_fail / blocks_att) if blocks_att > 0 else 0.0
        quality_pct = max(0.0, min(100.0, 100.0 - flr * 100.0))
        link_state = (
            "online" if quality_pct > 96
            else "degraded" if quality_pct > 88
            else "offline"
        )

        total_sym = stats["total_symbols"]
        lost_sym = stats["lost_symbols"]
        symbol_bits = max(1, cfg.get("symbol_size", 1) * 8)
        ber = (lost_sym / (total_sym * symbol_bits)) if total_sym > 0 else None

        link = {
            "state": link_state,
            "qualityPct": quality_pct,
            "rssiDbm": None,
            "snrDb": None,
            "berEstimate": ber,
            "latencyMsAvg": 0.0,
            "latencyMsMax": 0.0,
            "uptimeSec": raw["uptime_sec"],
        }

        errors = {
            "ber": ber,
            "flrPct": flr,
            "crcDrops": stats["symbols_dropped_crc"],
            "recoveredPackets": stats["recovered_packets"],
            "lostBlocks": blocks_fail,
            "blocksAttempted": blocks_att,
            "blocksRecovered": stats["blocks_recovered"],
            "blocksFailed": blocks_fail,
        }

        history = self._history if self._history else [{
            "t": raw["ts_ms"],
            "txBps": 0.0, "rxBps": 0.0, "txPps": 0.0, "rxPps": 0.0,
        }]

        # Placeholder pipeline — real per-stage queue depths would require
        # further instrumentation in block_builder/deinterleaver/etc.
        pipeline_names = [
            "LAN RX", "Fragment", "FEC Encode", "Interleave", "FSO TX",
            "FSO RX", "Deinterleave", "FEC Decode", "Reassemble", "LAN TX",
        ]
        latest_tps = history[-1]["txPps"] if history else 0.0
        pipeline = [
            {"name": n, "queueDepth": 0, "processingUs": 0.0,
             "throughputPps": latest_tps, "healthy": True}
            for n in pipeline_names
        ]

        burst_histogram = [
            {"label": "1",       "count": stats["burst_len_1"]},
            {"label": "2-5",     "count": stats["burst_len_2_5"]},
            {"label": "6-10",    "count": stats["burst_len_6_10"]},
            {"label": "11-50",   "count": stats["burst_len_11_50"]},
            {"label": "51-100",  "count": stats["burst_len_51_100"]},
            {"label": "101-500", "count": stats["burst_len_101_500"]},
            {"label": "501+",    "count": stats["burst_len_501_plus"]},
        ]

        decoder_stress = {
            "blocksWithLoss":           stats.get("blocks_with_loss", 0),
            "worstHolesInBlock":        stats.get("worst_holes_in_block", 0),
            "totalHolesInBlocks":       stats.get("total_holes_in_blocks", 0),
            "recoverableBursts":        stats.get("recoverable_bursts", 0),
            "criticalBursts":           stats.get("critical_bursts", 0),
            "burstsExceedingFecSpan":   stats.get("bursts_exceeding_fec_span", 0),
        }
        config_echo = {
            "k":                 int(cfg.get("k", 0)),
            "m":                 int(cfg.get("m", 0)),
            "depth":             int(cfg.get("depth", 0)),
            "symbolSize":        int(cfg.get("symbol_size", 0)),
            "lanIface":          str(cfg.get("lan_iface", "")),
            "fsoIface":          str(cfg.get("fso_iface", "")),
            "internalSymbolCrc": bool(cfg.get("internal_symbol_crc", True)),
        }

        return {
            "source": "gateway",
            "generatedAt": int(time.time() * 1000),
            "link": link,
            "throughput": list(history),
            "errors": errors,
            "pipeline": pipeline,
            "burstHistogram": burst_histogram,
            "system": _system_info(cfg),
            "alerts": list(self._alerts),
            "decoderStress": decoder_stress,
            "configEcho": config_echo,
        }


def _system_info(cfg: dict[str, Any]) -> dict[str, Any]:
    cpu_pct = _read_cpu_pct()
    mem_pct = _read_mem_pct()
    profile = (
        f"K={cfg.get('k', '?')}/M={cfg.get('m', '?')}/depth={cfg.get('depth', '?')}"
        if cfg else "UNKNOWN"
    )
    return {
        "version": "v3.1-phase8-gateway",
        "build": "live",
        "configProfile": profile,
        "gatewayId": "FSO-GW-001",
        "firmware": "3.1.7",
        "cpuPct": cpu_pct,
        "memoryPct": mem_pct,
        "temperatureC": 48.0,
        "fpgaAccel": False,
    }


def _read_cpu_pct() -> float:
    try:
        with open("/proc/loadavg") as f:
            load1 = float(f.read().split()[0])
        import os
        cores = os.cpu_count() or 1
        return min(100.0, (load1 / cores) * 100.0)
    except Exception:
        return 0.0


def _read_mem_pct() -> float:
    try:
        info: dict[str, int] = {}
        with open("/proc/meminfo") as f:
            for line in f:
                key, _, rest = line.partition(":")
                parts = rest.strip().split()
                if parts:
                    info[key.strip()] = int(parts[0])
        total = info.get("MemTotal")
        avail = info.get("MemAvailable")
        if total and avail is not None:
            return 100.0 * (total - avail) / total
    except Exception:
        pass
    return 0.0
