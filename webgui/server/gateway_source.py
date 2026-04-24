"""
Gateway telemetry source — connects to the C-side control_server UNIX socket
and translates the raw atomic counter snapshot into the shape the dashboard
consumes.

The translation is intentionally thin. Every field in the output maps to a
real C counter from struct stats_container (include/stats.h) or to the live
config echo that control_server stamps on every frame. No synthesized
RSSI/SNR/latency — those do not exist in this software.

Derivations done here (and only here):
  * throughput time-series: per-second rates computed from byte-count deltas
  * qualityPct: 100 × blocks_recovered / blocks_attempted
  * symbolLossRatio: lost_symbols / total_symbols
  * alerts: emitted when a per-tick counter delta crosses a threshold
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

        # Throughput:
        #   TX side = wire bytes on FSO (transmitted_bytes)
        #   RX side = reassembled Ethernet bytes on LAN (recovered_bytes)
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

        d_exceed = stats["bursts_exceeding_fec_span"] - prev_stats["bursts_exceeding_fec_span"]
        if d_exceed > 0:
            self._push_alert(ts, "critical", "BURST",
                             f"{d_exceed} burst(s) exceeded FEC span this tick")

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
        blocks_rec = stats["blocks_recovered"]
        blocks_fail = stats["blocks_failed"]
        quality_pct = (100.0 * blocks_rec / blocks_att) if blocks_att > 0 else 100.0
        link_state = (
            "online" if quality_pct > 99.5
            else "degraded" if quality_pct > 95
            else "offline"
        )

        total_sym = stats["total_symbols"]
        lost_sym = stats["lost_symbols"]
        symbol_loss = (lost_sym / total_sym) if total_sym > 0 else None

        link = {
            "state": link_state,
            "qualityPct": quality_pct,
            "uptimeSec": raw["uptime_sec"],
        }

        errors = {
            "symbolLossRatio": symbol_loss,
            "blockFailRatio": (blocks_fail / blocks_att) if blocks_att > 0 else 0.0,
            "crcDrops": stats["symbols_dropped_crc"],
            "recoveredPackets": stats["recovered_packets"],
            "failedPackets": stats["failed_packets"],
            "blocksAttempted": blocks_att,
            "blocksRecovered": blocks_rec,
            "blocksFailed": blocks_fail,
        }

        history = self._history if self._history else [{
            "t": raw["ts_ms"],
            "txBps": 0.0, "rxBps": 0.0, "txPps": 0.0, "rxPps": 0.0,
        }]

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
            "blocksWithLoss":        stats.get("blocks_with_loss", 0),
            "worstHolesInBlock":     stats.get("worst_holes_in_block", 0),
            "totalHolesInBlocks":    stats.get("total_holes_in_blocks", 0),
            "recoverableBursts":     stats.get("recoverable_bursts", 0),
            "criticalBursts":        stats.get("critical_bursts", 0),
            "burstsExceedingFecSpan": stats.get("bursts_exceeding_fec_span", 0),
            "configuredFecBurstSpan": stats.get("configured_fec_burst_span", 0),
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
            "burstHistogram": burst_histogram,
            "decoderStress": decoder_stress,
            "configEcho": config_echo,
            "alerts": list(self._alerts),
        }
