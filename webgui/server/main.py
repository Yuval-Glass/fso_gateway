"""
FSO Gateway — Bridge server.

Serves the dashboard:
  * live telemetry over WebSocket (/ws/live)
  * log stream over WebSocket (/ws/logs)
  * config read/write (/api/config)
  * run history (/api/runs...)

The telemetry schema exactly mirrors what the C daemon's control_server emits
via its UNIX socket: atomic counter snapshots from struct stats_container plus
the live config echo, with one layer of derivation (per-second rates from
byte-count deltas, FEC recovery % from block counters, alerts from spikes).

When the C daemon is running, GatewaySource streams real counters. When it is
not, the Python mock below increments a set of synthetic counters that use the
same schema — so the UI behaves identically either way and never shows fields
that the real daemon cannot produce.

Run:
    uv run uvicorn main:app --reload --port 8000
"""

from __future__ import annotations

import asyncio
import random
import time
from contextlib import asynccontextmanager
from dataclasses import asdict, dataclass, field
from typing import Any

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import PlainTextResponse

import channel as channel_ctrl
import experiments as experiments_store
from config_store import ConfigError, load as config_load, save as config_save, validate as config_validate
from daemon import DaemonSupervisor
from gateway_source import GatewaySource
from log_source import LogManager
import run_store

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

TICK_HZ = 1.0
HISTORY_SAMPLES = 300
ALERT_RING_MAX = 200
GATEWAY_SOCKET_PATH = "/tmp/fso_gw.sock"
ALLOWED_ORIGINS = [
    "http://localhost:3100",
    "http://localhost:3000",
    "http://127.0.0.1:3100",
]

# ---------------------------------------------------------------------------
# Mock telemetry
# ---------------------------------------------------------------------------
# Mirrors what fso_gw_runner + control_server would emit when the daemon is
# running. Fields that the real daemon cannot measure (optical RSSI/SNR, per-
# packet latency, per-stage queue depths) are intentionally absent.
# ---------------------------------------------------------------------------


def _walk(current: float, step: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, current + (random.random() - 0.5) * step * 2))


@dataclass
class SimState:
    """Synthetic equivalent of struct stats_container in the C side."""
    # Monotonic counters (mirror struct stats_container)
    ingress_packets: int = 0
    ingress_bytes: int = 0
    transmitted_packets: int = 0   # count of wire symbols on FSO
    transmitted_bytes: int = 0
    recovered_packets: int = 0     # reassembled LAN frames
    recovered_bytes: int = 0
    failed_packets: int = 0
    total_symbols: int = 0
    lost_symbols: int = 0
    symbols_dropped_crc: int = 0
    blocks_attempted: int = 0
    blocks_recovered: int = 0
    blocks_failed: int = 0
    max_burst_length: int = 0
    burst_len_1: int = 0
    burst_len_2_5: int = 0
    burst_len_6_10: int = 0
    burst_len_11_50: int = 0
    burst_len_51_100: int = 0
    burst_len_101_500: int = 0
    burst_len_501_plus: int = 0
    bursts_exceeding_fec_span: int = 0
    recoverable_bursts: int = 0
    critical_bursts: int = 0
    blocks_with_loss: int = 0
    worst_holes_in_block: int = 0
    total_holes_in_blocks: int = 0

    # Traffic shape drivers (not counters themselves)
    tx_bps: float = 0.0
    rx_bps: float = 0.0
    tx_pps: float = 0.0
    rx_pps: float = 0.0

    # Derived bookkeeping
    history: list[dict[str, Any]] = field(default_factory=list)
    last_t_ms: int = 0
    uptime_start_ms: int = 0
    alerts: list[dict[str, Any]] = field(default_factory=list)
    block_events: list[dict[str, Any]] = field(default_factory=list)

    @classmethod
    def fresh(cls) -> "SimState":
        now_ms = int(time.time() * 1000)
        return cls(last_t_ms=now_ms, uptime_start_ms=now_ms)


# Config matches defaults from README's recommended hardware test
# (README.md "Recommended FEC parameters for hardware testing").
MOCK_CONFIG = {
    "k": 8,
    "m": 4,
    "depth": 2,
    "symbol_size": 1500,
    "internal_symbol_crc": True,
    "lan_iface": "enp1s0f0np0",
    "fso_iface": "enp1s0f1np1",
}


def _step(s: SimState) -> None:
    """Advance the synthetic counters by one tick in a way that matches
    the semantics of the real C daemon instrumentation."""
    now_ms = int(time.time() * 1000)
    dt_s = max(0.001, (now_ms - s.last_t_ms) / 1000.0)

    # Ingress rate ~40 Mbps (matches Phase 8 UDP validation of ~42.8 Mbps).
    s.tx_bps = _walk(s.tx_bps or 40e6, 1.5e6, 20e6, 55e6)
    s.rx_bps = _walk(s.rx_bps or 38e6, 1.5e6, 18e6, 52e6)
    avg_pkt = 1200.0  # realistic avg (iperf3 UDP default ~1470B)
    s.tx_pps = s.tx_bps / 8.0 / avg_pkt
    s.rx_pps = s.rx_bps / 8.0 / avg_pkt

    d_tx_pkts = int(s.tx_pps * dt_s)
    d_rx_pkts = int(s.rx_pps * dt_s)

    # ingress = LAN frames entering TX pipeline = roughly same as tx pkts
    # (before fragmentation).
    s.ingress_packets += d_tx_pkts
    s.ingress_bytes += int(d_tx_pkts * avg_pkt)

    # transmitted = wire symbols on FSO = ingress_pkts × (K+M)/K × syms_per_pkt.
    # For avg_pkt ≤ symbol_size, syms_per_pkt = 1.
    k = MOCK_CONFIG["k"]; m = MOCK_CONFIG["m"]
    syms_per_pkt = max(1, int(avg_pkt / MOCK_CONFIG["symbol_size"]) + 1)
    wire_syms = d_tx_pkts * syms_per_pkt * (k + m) // k
    wire_bytes_each = MOCK_CONFIG["symbol_size"] + 18  # + wire header
    s.transmitted_packets += wire_syms
    s.transmitted_bytes += wire_syms * wire_bytes_each

    # RX side: most blocks decode OK; a small fraction fail.
    blocks_this_tick = max(1, d_rx_pkts // syms_per_pkt)
    s.blocks_attempted += blocks_this_tick
    failures = sum(1 for _ in range(blocks_this_tick) if random.random() < 0.001)
    successes = blocks_this_tick - failures
    s.blocks_recovered += successes
    s.blocks_failed += failures
    s.recovered_packets += d_rx_pkts
    s.recovered_bytes += int(d_rx_pkts * avg_pkt)
    if failures > 0:
        s.failed_packets += failures * syms_per_pkt

    # Symbols: every block contributes K+M to total. Holes are rare.
    sym_total = blocks_this_tick * (k + m)
    holes = failures * (m + 1)  # failed blocks have >m holes by definition
    sparse_holes = sum(1 for _ in range(successes) if random.random() < 0.01)
    holes += sparse_holes
    s.total_symbols += sym_total
    s.lost_symbols += holes
    if random.random() < 0.05:
        s.symbols_dropped_crc += 1

    if holes > 0:
        s.blocks_with_loss += max(1, failures + (sparse_holes // 2))
        s.total_holes_in_blocks += holes
        s.worst_holes_in_block = max(s.worst_holes_in_block,
                                     min(holes, m + 2))

    # Bucket bursts (1 symbol bursts dominate).
    for _ in range(sparse_holes):
        s.burst_len_1 += 1
        s.recoverable_bursts += 1
    for _ in range(failures):
        b = m + 1 + random.randint(0, 3)
        if b <= 5: s.burst_len_2_5 += 1
        elif b <= 10: s.burst_len_6_10 += 1
        else: s.burst_len_11_50 += 1
        if b > m * MOCK_CONFIG["depth"]:
            s.bursts_exceeding_fec_span += 1
            s.critical_bursts += 1
        else:
            s.recoverable_bursts += 1
        s.max_burst_length = max(s.max_burst_length, b)

    # History ring for throughput chart
    s.history.append({
        "t": now_ms,
        "txBps": s.tx_bps,
        "rxBps": s.rx_bps,
        "txPps": s.tx_pps,
        "rxPps": s.rx_pps,
    })
    if len(s.history) > HISTORY_SAMPLES:
        del s.history[: len(s.history) - HISTORY_SAMPLES]

    s.last_t_ms = now_ms

    _maybe_emit_alert(s)
    _maybe_emit_block_events(s)


_ALERT_TEMPLATES: list[tuple[str, str, list[str]]] = [
    ("info",     "LINK",   ["FSO link forwarding stable", "Quality recovered above threshold"]),
    ("info",     "FEC",    ["Block recovery rate improved", "All blocks decoded cleanly"]),
    ("info",     "CONFIG", ["Gateway configuration applied"]),
    ("info",     "ARP",    ["ARP cache learned new peer MAC"]),
    ("warning",  "CRC",    ["CRC drop rate elevated"]),
    ("warning",  "BURST",  ["Burst length creeping up"]),
    ("critical", "FEC",    ["FEC block failed to decode", "Multiple FEC failures this tick"]),
    ("critical", "BURST",  ["Burst exceeded FEC recovery span"]),
]


def _maybe_emit_alert(s: SimState) -> None:
    if random.random() > 0.15:
        return
    sev, mod, msgs = random.choice(_ALERT_TEMPLATES)
    now = int(time.time() * 1000)
    s.alerts.insert(0, {
        "id": f"{now}-{mod}-{random.randint(1000, 9999)}",
        "t": now,
        "severity": sev,
        "module": mod,
        "message": random.choice(msgs),
    })
    del s.alerts[ALERT_RING_MAX:]


def _link(s: SimState) -> dict[str, Any]:
    quality = (100.0 * s.blocks_recovered / s.blocks_attempted
               if s.blocks_attempted > 0 else 100.0)
    state = "online" if quality > 99.5 else "degraded" if quality > 95 else "offline"
    return {
        "state": state,
        "qualityPct": quality,
        "uptimeSec": (int(time.time() * 1000) - s.uptime_start_ms) / 1000.0,
    }


def _errors(s: SimState) -> dict[str, Any]:
    return {
        "symbolLossRatio": (s.lost_symbols / s.total_symbols) if s.total_symbols > 0 else None,
        "blockFailRatio": (s.blocks_failed / s.blocks_attempted) if s.blocks_attempted > 0 else 0.0,
        "crcDrops": s.symbols_dropped_crc,
        "recoveredPackets": s.recovered_packets,
        "failedPackets": s.failed_packets,
        "blocksAttempted": s.blocks_attempted,
        "blocksRecovered": s.blocks_recovered,
        "blocksFailed": s.blocks_failed,
    }


def _burst_histogram(s: SimState) -> list[dict[str, Any]]:
    return [
        {"label": "1",       "count": s.burst_len_1},
        {"label": "2-5",     "count": s.burst_len_2_5},
        {"label": "6-10",    "count": s.burst_len_6_10},
        {"label": "11-50",   "count": s.burst_len_11_50},
        {"label": "51-100",  "count": s.burst_len_51_100},
        {"label": "101-500", "count": s.burst_len_101_500},
        {"label": "501+",    "count": s.burst_len_501_plus},
    ]


def _decoder_stress(s: SimState) -> dict[str, Any]:
    return {
        "blocksWithLoss":         s.blocks_with_loss,
        "worstHolesInBlock":      s.worst_holes_in_block,
        "totalHolesInBlocks":     s.total_holes_in_blocks,
        "recoverableBursts":      s.recoverable_bursts,
        "criticalBursts":         s.critical_bursts,
        "burstsExceedingFecSpan": s.bursts_exceeding_fec_span,
        "configuredFecBurstSpan": MOCK_CONFIG["m"] * MOCK_CONFIG["depth"],
    }


def _dil_stats(s: SimState) -> dict[str, Any]:
    """Synthesize dil_stats so the UI shows non-zero values in mock mode."""
    return {
        "droppedDuplicate":     0,
        "droppedFrozen":        max(0, s.blocks_attempted * 3),
        "droppedErasure":       s.lost_symbols,
        "droppedCrcFail":       s.symbols_dropped_crc,
        "evictedFilling":       0,
        "evictedDone":          0,
        "blocksReady":          s.blocks_attempted,
        "blocksFailedTimeout":  0,
        "blocksFailedHoles":    s.blocks_failed,
        "activeBlocks":         random.randint(0, MOCK_CONFIG["depth"]),
        "readyCount":           random.randint(0, 2),
    }


def _arp_entries() -> list[dict[str, Any]]:
    now = int(time.time() * 1000)
    return [
        {"ip": "192.168.50.1", "mac": "90:2e:16:d6:96:ba", "lastSeenMs": now - 4000},
        {"ip": "192.168.50.2", "mac": "c4:ef:bb:5f:cd:5c", "lastSeenMs": now - 2000},
    ]


def _maybe_emit_block_events(s: SimState) -> None:
    """Generate ~5 SUCCESS events per tick, an occasional failure or eviction."""
    now = int(time.time() * 1000)
    base_block = s.blocks_attempted
    for i in range(5):
        s.block_events.insert(0, {
            "blockId":  base_block - i,
            "t":        now - i,
            "reason":   "SUCCESS",
            "evicted":  False,
        })
    if random.random() < 0.05:
        s.block_events.insert(0, {
            "blockId": base_block + 100,
            "t":       now,
            "reason":  random.choice(["TOO_MANY_HOLES", "TIMEOUT", "DECODE_FAILED"]),
            "evicted": False,
        })
    if random.random() < 0.02:
        s.block_events.insert(0, {
            "blockId": base_block + 200,
            "t":       now,
            "reason":  "EVICTED_FILLING",
            "evicted": True,
        })
    del s.block_events[200:]


def _config_echo() -> dict[str, Any]:
    return {
        "k": MOCK_CONFIG["k"],
        "m": MOCK_CONFIG["m"],
        "depth": MOCK_CONFIG["depth"],
        "symbolSize": MOCK_CONFIG["symbol_size"],
        "lanIface": MOCK_CONFIG["lan_iface"],
        "fsoIface": MOCK_CONFIG["fso_iface"],
        "internalSymbolCrc": MOCK_CONFIG["internal_symbol_crc"],
    }


def snapshot(s: SimState, source: str = "mock") -> dict[str, Any]:
    _step(s)
    return {
        "source": source,
        "generatedAt": int(time.time() * 1000),
        "link": _link(s),
        "throughput": list(s.history),
        "errors": _errors(s),
        "burstHistogram": _burst_histogram(s),
        "decoderStress": _decoder_stress(s),
        "configEcho": _config_echo(),
        "alerts": s.alerts,
        "dilStats": _dil_stats(s),
        "arpEntries": _arp_entries(),
        "blockEvents": list(s.block_events),
    }


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------

SIM = SimState.fresh()
GATEWAY = GatewaySource(GATEWAY_SOCKET_PATH)
LOGS = LogManager()
DAEMON = DaemonSupervisor()

RECORDER_INTERVAL_SEC = 1.0
_recorder_task: asyncio.Task[None] | None = None
_active_run_id: int | None = None


async def _recorder_loop() -> None:
    global _active_run_id
    while True:
        try:
            await asyncio.sleep(RECORDER_INTERVAL_SEC)
            if _active_run_id is None:
                continue
            snap = current_snapshot()
            await asyncio.to_thread(run_store.append_sample, _active_run_id, snap)
        except asyncio.CancelledError:
            raise
        except Exception:
            await asyncio.sleep(1.0)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _recorder_task, _active_run_id
    GATEWAY.start()
    await LOGS.start()
    await asyncio.to_thread(run_store.end_active_runs)
    _active_run_id = await asyncio.to_thread(run_store.create_run, None, None)
    _recorder_task = asyncio.create_task(_recorder_loop(), name="recorder")
    try:
        yield
    finally:
        if _recorder_task:
            _recorder_task.cancel()
            try:
                await _recorder_task
            except (asyncio.CancelledError, Exception):
                pass
        if _active_run_id is not None:
            await asyncio.to_thread(run_store.end_run, _active_run_id)
        await GATEWAY.stop()
        await LOGS.stop()
        await DAEMON.shutdown()


app = FastAPI(title="FSO Gateway Bridge", version="1.3.0", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=ALLOWED_ORIGINS,
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


def current_snapshot() -> dict[str, Any]:
    if GATEWAY.is_connected():
        snap = GATEWAY.snapshot()
        if snap is not None:
            return snap
    return snapshot(SIM)


def current_source() -> str:
    return "gateway" if (GATEWAY.is_connected() and GATEWAY.snapshot() is not None) else "mock"


# ---------------------------------------------------------------------------
# Config API
# ---------------------------------------------------------------------------


@app.get("/api/config")
async def get_config() -> dict[str, Any]:
    return {"config": asdict(config_load())}


@app.post("/api/config")
async def put_config(request: Request) -> dict[str, Any]:
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="request body must be valid JSON")
    if not isinstance(body, dict) or "config" not in body:
        raise HTTPException(status_code=400, detail="expected { config: {...} }")
    try:
        cfg = config_validate(body["config"])
    except ConfigError as e:
        raise HTTPException(status_code=422, detail=str(e))
    config_save(cfg)
    return {"config": asdict(cfg), "status": "saved", "requires_restart": True}


# ---------------------------------------------------------------------------
# Health + live telemetry
# ---------------------------------------------------------------------------


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "status": "ok",
        "source": current_source(),
        "gateway_socket": GATEWAY_SOCKET_PATH,
        "logs_mode": LOGS.mode,
        "active_run_id": _active_run_id,
        "uptime_sec": (int(time.time() * 1000) - SIM.uptime_start_ms) / 1000,
        "tick_hz": TICK_HZ,
    }


@app.websocket("/ws/live")
async def ws_live(ws: WebSocket) -> None:
    await ws.accept()
    try:
        await ws.send_json(current_snapshot())
        while True:
            await asyncio.sleep(1.0 / TICK_HZ)
            await ws.send_json(current_snapshot())
    except WebSocketDisconnect:
        return
    except Exception:
        try:
            await ws.close()
        except Exception:
            pass


@app.websocket("/ws/logs")
async def ws_logs(ws: WebSocket) -> None:
    await ws.accept()
    q = LOGS.subscribe()
    try:
        await ws.send_json({"type": "meta", "mode": LOGS.mode})
    except Exception:
        LOGS.unsubscribe(q)
        return
    try:
        while True:
            ev = await q.get()
            await ws.send_json({"type": "event", **ev.as_dict()})
    except WebSocketDisconnect:
        return
    except Exception:
        try:
            await ws.close()
        except Exception:
            pass
    finally:
        LOGS.unsubscribe(q)


# ---------------------------------------------------------------------------
# Runs API
# ---------------------------------------------------------------------------


@app.get("/api/runs")
async def runs_list() -> dict[str, Any]:
    runs = await asyncio.to_thread(run_store.list_runs, 200)
    return {
        "active_run_id": _active_run_id,
        "runs": [r.to_dict() for r in runs],
    }


@app.get("/api/runs/{run_id}")
async def runs_get(run_id: int) -> dict[str, Any]:
    run = await asyncio.to_thread(run_store.get_run, run_id)
    if run is None:
        raise HTTPException(status_code=404, detail=f"run {run_id} not found")
    summary = await asyncio.to_thread(run_store.stats, run_id)
    return {"run": run.to_dict(), "stats": summary}


@app.get("/api/runs/{run_id}/samples")
async def runs_samples(run_id: int, max_points: int = 1000) -> dict[str, Any]:
    run = await asyncio.to_thread(run_store.get_run, run_id)
    if run is None:
        raise HTTPException(status_code=404, detail=f"run {run_id} not found")
    samples = await asyncio.to_thread(run_store.get_samples, run_id, max_points)
    return {"runId": run_id, "samples": samples}


@app.get("/api/runs/{run_id}/export.csv", response_class=PlainTextResponse)
async def runs_export_csv(run_id: int) -> str:
    run = await asyncio.to_thread(run_store.get_run, run_id)
    if run is None:
        raise HTTPException(status_code=404, detail=f"run {run_id} not found")
    return await asyncio.to_thread(run_store.export_csv, run_id)


@app.post("/api/runs/new")
async def runs_new(request: Request) -> dict[str, Any]:
    global _active_run_id
    body: dict[str, Any] = {}
    try:
        body = await request.json()
    except Exception:
        body = {}
    name = body.get("name") if isinstance(body.get("name"), str) else None
    notes = body.get("notes") if isinstance(body.get("notes"), str) else None
    new_id = await asyncio.to_thread(run_store.create_run, name, notes)
    _active_run_id = new_id
    run = await asyncio.to_thread(run_store.get_run, new_id)
    return {"run": run.to_dict() if run else None, "active_run_id": _active_run_id}


@app.post("/api/runs/{run_id}/end")
async def runs_end(run_id: int) -> dict[str, Any]:
    global _active_run_id
    ok = await asyncio.to_thread(run_store.end_run, run_id)
    if not ok:
        raise HTTPException(status_code=404, detail="run not active or not found")
    if _active_run_id == run_id:
        _active_run_id = None
    return {"ended": run_id, "active_run_id": _active_run_id}


@app.delete("/api/runs/{run_id}")
async def runs_delete(run_id: int) -> dict[str, Any]:
    global _active_run_id
    if _active_run_id == run_id:
        raise HTTPException(status_code=409, detail="cannot delete the active run; end it first")
    ok = await asyncio.to_thread(run_store.delete_run, run_id)
    if not ok:
        raise HTTPException(status_code=404, detail="run not found")
    return {"deleted": run_id}


# ---------------------------------------------------------------------------
# Channel impairment (tc netem) API
# ---------------------------------------------------------------------------


@app.get("/api/channel/netem")
async def netem_show(iface: str | None = None) -> dict[str, Any]:
    target = iface or channel_ctrl.DEFAULT_IFACE
    return await asyncio.to_thread(channel_ctrl.show, target)


@app.post("/api/channel/netem")
async def netem_apply(request: Request) -> dict[str, Any]:
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="request body must be JSON")
    if not isinstance(body, dict):
        raise HTTPException(status_code=400, detail="expected an object")
    iface = body.get("iface") or channel_ctrl.DEFAULT_IFACE
    try:
        if body.get("clear"):
            return await asyncio.to_thread(channel_ctrl.clear, iface)
        enter = float(body.get("enterPct", 0.0))
        ex    = float(body.get("exitPct", 0.0))
        loss  = float(body.get("lossPct", 0.0))
    except (TypeError, ValueError):
        raise HTTPException(status_code=422, detail="enterPct/exitPct/lossPct must be numeric")
    try:
        return await asyncio.to_thread(channel_ctrl.apply_gemodel, iface, enter, ex, loss)
    except channel_ctrl.TcNotAvailable as e:
        raise HTTPException(status_code=503, detail=str(e))
    except ValueError as e:
        raise HTTPException(status_code=422, detail=str(e))


# ---------------------------------------------------------------------------
# Experiment artefact API (build/stats/experiment_*.txt)
# ---------------------------------------------------------------------------


@app.get("/api/experiments")
async def experiments_list() -> dict[str, Any]:
    items = await asyncio.to_thread(experiments_store.list_experiments)
    return {"experiments": items}


@app.get("/api/experiments/{name}")
async def experiments_get(name: str) -> dict[str, Any]:
    try:
        return await asyncio.to_thread(experiments_store.read_experiment, name)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail=f"experiment '{name}' not found")


# ---------------------------------------------------------------------------
# Daemon supervision (Phase 3B)
# ---------------------------------------------------------------------------


@app.get("/api/daemon")
async def daemon_status() -> dict[str, Any]:
    return DAEMON.status()


@app.post("/api/daemon/start")
async def daemon_start() -> dict[str, Any]:
    return await DAEMON.start()


@app.post("/api/daemon/stop")
async def daemon_stop() -> dict[str, Any]:
    return await DAEMON.stop()


@app.post("/api/daemon/restart")
async def daemon_restart() -> dict[str, Any]:
    return await DAEMON.restart()
