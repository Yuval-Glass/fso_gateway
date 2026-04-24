"""
FSO Gateway — Bridge server.

Phase 2A: serves mock telemetry over WebSocket matching the TypeScript
`TelemetrySnapshot` shape. In Phase 2B this module will also connect
to the C-side control_server UNIX socket and stream real data when
available, falling back to mock otherwise.

Run:
    uv run uvicorn main:app --reload --port 8000
"""

from __future__ import annotations

import asyncio
import random
import time
from dataclasses import dataclass, field
from typing import Any

from contextlib import asynccontextmanager
from dataclasses import asdict

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware

from fastapi.responses import PlainTextResponse

from config_store import ConfigError, load as config_load, save as config_save, validate as config_validate
from gateway_source import GatewaySource
from log_source import LogManager
import run_store

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

TICK_HZ = 1.0
HISTORY_SAMPLES = 300
GATEWAY_SOCKET_PATH = "/tmp/fso_gw.sock"
ALLOWED_ORIGINS = [
    "http://localhost:3100",
    "http://localhost:3000",
    "http://127.0.0.1:3100",
]

# ---------------------------------------------------------------------------
# Mock telemetry — mirror of webgui/src/lib/mockTelemetry.ts
# ---------------------------------------------------------------------------


def _clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def _walk(current: float, step: float, lo: float, hi: float) -> float:
    return _clamp(current + (random.random() - 0.5) * step * 2, lo, hi)


@dataclass
class SimState:
    last_t_ms: int
    tx_bps: float = 572e6
    rx_bps: float = 568e6
    tx_pps: float = 51236
    rx_pps: float = 51102
    history: list[dict[str, Any]] = field(default_factory=list)
    quality_pct: float = 98.7
    latency_ms: float = 1.62
    blocks_attempted: int = 192_530
    blocks_recovered: int = 192_434
    blocks_failed: int = 12
    recovered_packets: int = 3_847
    lost_blocks: int = 8
    crc_drops: int = 46
    uptime_start_ms: int = 0
    alerts: list[dict[str, Any]] = field(default_factory=list)

    @classmethod
    def fresh(cls) -> "SimState":
        now_ms = int(time.time() * 1000)
        hist = [
            {
                "t": now_ms - i * 1000,
                "txBps": 560e6 + random.random() * 40e6,
                "rxBps": 555e6 + random.random() * 40e6,
                "txPps": 50000 + random.random() * 3000,
                "rxPps": 49500 + random.random() * 3000,
            }
            for i in range(HISTORY_SAMPLES - 1, -1, -1)
        ]
        state = cls(
            last_t_ms=now_ms,
            history=hist,
            uptime_start_ms=now_ms - (12 * 86_400 + 4 * 3600 + 32 * 60) * 1000,
        )
        state.alerts = _seed_alerts(now_ms)
        return state


ALERT_RING_MAX = 200

_ALERT_TEMPLATES: list[tuple[str, str, list[str]]] = [
    ("info",     "LINK",    ["Link quality recovered",
                             "FSO beam alignment stable",
                             "Endpoint B re-registered",
                             "SNR back above threshold"]),
    ("info",     "FEC",     ["FEC recovery rate improved",
                             "Block throughput stabilized"]),
    ("info",     "CONFIG",  ["Configuration applied",
                             "Runtime profile rotated"]),
    ("info",     "ARP",     ["ARP cache grew to new size",
                             "New MAC observed"]),
    ("warning",  "LATENCY", ["High latency detected",
                             "Latency P99 exceeded 3 ms"]),
    ("warning",  "RX",      ["Symbol queue depth nearing threshold",
                             "Deinterleaver eviction rate elevated"]),
    ("warning",  "CRC",     ["CRC drop rate elevated",
                             "Multiple CRC errors clustered"]),
    ("warning",  "BURST",   ["Burst length creeping up",
                             "Burst histogram mid-bucket growing"]),
    ("critical", "BURST",   ["Burst loss detected - 8 symbols",
                             "Burst exceeding FEC span observed",
                             "Critical burst > 20 symbols"]),
    ("critical", "FEC",     ["FEC block failed to decode",
                             "Multiple FEC failures this tick"]),
    ("critical", "LINK",    ["Link drop detected",
                             "Peer became unreachable"]),
]


def _seed_alerts(now_ms: int) -> list[dict[str, Any]]:
    """Start with a reasonable backlog so the alerts page has content on first load."""
    result: list[dict[str, Any]] = []
    age = 5
    for _ in range(40):
        age += random.randint(4, 25)
        sev, mod, msgs = random.choice(_ALERT_TEMPLATES)
        t = now_ms - age * 1000
        result.append({
            "id": f"{t}-{mod}-{random.randint(1000, 9999)}",
            "t": t,
            "severity": sev,
            "module": mod,
            "message": random.choice(msgs),
        })
    # Newest first
    result.sort(key=lambda a: a["t"], reverse=True)
    return result


def _maybe_emit_alert(s: "SimState") -> None:
    """Roll the dice each tick to keep the mock alert feed alive."""
    if random.random() > 0.18:
        return
    sev, mod, msgs = random.choices(
        _ALERT_TEMPLATES,
        weights=[5, 5, 3, 3, 3, 3, 3, 3, 2, 2, 1],
        k=1,
    )[0]
    now = int(time.time() * 1000)
    event = {
        "id": f"{now}-{mod}-{random.randint(1000, 9999)}",
        "t": now,
        "severity": sev,
        "module": mod,
        "message": random.choice(msgs),
    }
    s.alerts.insert(0, event)
    del s.alerts[ALERT_RING_MAX:]


def _step(s: SimState) -> None:
    now_ms = int(time.time() * 1000)
    dt_ms = now_ms - s.last_t_ms

    s.tx_bps = _walk(s.tx_bps, 8e6, 420e6, 680e6)
    s.rx_bps = _walk(s.rx_bps, 8e6, 420e6, 680e6)
    s.tx_pps = _walk(s.tx_pps, 800, 42000, 58000)
    s.rx_pps = _walk(s.rx_pps, 800, 42000, 58000)

    s.history.append(
        {"t": now_ms, "txBps": s.tx_bps, "rxBps": s.rx_bps, "txPps": s.tx_pps, "rxPps": s.rx_pps}
    )
    if len(s.history) > HISTORY_SAMPLES:
        del s.history[0 : len(s.history) - HISTORY_SAMPLES]

    s.quality_pct = _walk(s.quality_pct, 0.15, 95.0, 99.9)
    s.latency_ms = _walk(s.latency_ms, 0.08, 0.9, 2.6)

    blocks_this_tick = int(dt_ms * 0.08 + random.random() * 4)
    s.blocks_attempted += blocks_this_tick
    s.blocks_recovered += max(0, blocks_this_tick - (1 if random.random() < 0.03 else 0))
    if random.random() < 0.02:
        s.blocks_failed += 1
    if random.random() < 0.15:
        s.recovered_packets += int(random.random() * 3)
    if random.random() < 0.01:
        s.lost_blocks += 1
    if random.random() < 0.06:
        s.crc_drops += 1

    s.last_t_ms = now_ms


def _link(s: SimState) -> dict[str, Any]:
    state = "online" if s.quality_pct > 96 else "degraded" if s.quality_pct > 88 else "offline"
    return {
        "state": state,
        "qualityPct": s.quality_pct,
        "rssiDbm": -21.3 + (random.random() - 0.5) * 1.8,
        "snrDb": 28.6 + (random.random() - 0.5) * 1.2,
        "berEstimate": 1.2e-9 * (1 + (random.random() - 0.5) * 0.6),
        "latencyMsAvg": s.latency_ms,
        "latencyMsMax": s.latency_ms + 1.8 + random.random() * 1.2,
        "uptimeSec": (int(time.time() * 1000) - s.uptime_start_ms) / 1000,
    }


def _errors(s: SimState) -> dict[str, Any]:
    total_symbols = s.blocks_attempted * 14
    lost_symbols = s.blocks_failed * 14 + s.crc_drops
    ber = lost_symbols / (total_symbols * 8 * 800) if total_symbols > 0 else None
    return {
        "ber": ber,
        "flrPct": (s.blocks_failed / s.blocks_attempted) if s.blocks_attempted > 0 else 0.0,
        "crcDrops": s.crc_drops,
        "recoveredPackets": s.recovered_packets,
        "lostBlocks": s.lost_blocks,
        "blocksAttempted": s.blocks_attempted,
        "blocksRecovered": s.blocks_recovered,
        "blocksFailed": s.blocks_failed,
    }


def _pipeline(s: SimState) -> list[dict[str, Any]]:
    base = s.tx_pps
    stages = [
        ("LAN RX", 12, 0.4, base),
        ("Fragment", 8, 0.8, base * 1.8),
        ("FEC Encode", 24, 3.2, base * 1.8),
        ("Interleave", 18, 1.1, base * 1.8),
        ("FSO TX", 10, 0.6, base * 1.8),
        ("FSO RX", 10, 0.5, base * 1.8),
        ("Deinterleave", 22, 1.3, base * 1.8),
        ("FEC Decode", 28, 4.1, base * 1.8),
        ("Reassemble", 6, 0.7, base),
        ("LAN TX", 12, 0.4, base),
    ]
    return [
        {
            "name": name,
            "queueDepth": int(random.random() * q_max),
            "processingUs": us,
            "throughputPps": tput,
            "healthy": True,
        }
        for (name, q_max, us, tput) in stages
    ]


def _burst_histogram() -> list[dict[str, Any]]:
    return [
        {"label": "1", "count": 1240 + int(random.random() * 50)},
        {"label": "2-5", "count": 380 + int(random.random() * 20)},
        {"label": "6-10", "count": 92 + int(random.random() * 8)},
        {"label": "11-20", "count": 34},
        {"label": "21-50", "count": 12},
        {"label": "51-100", "count": 4},
        {"label": "101-500", "count": 1},
        {"label": "501+", "count": 0},
    ]


def _system() -> dict[str, Any]:
    return {
        "version": "v3.1.0-phase8",
        "build": "a202b70",
        "configProfile": "LAB-TEST",
        "gatewayId": "FSO-GW-001",
        "firmware": "3.1.7",
        "cpuPct": 18 + (random.random() - 0.5) * 6,
        "memoryPct": 42 + (random.random() - 0.5) * 4,
        "temperatureC": 48 + (random.random() - 0.5) * 2,
        "fpgaAccel": True,
    }


def _decoder_stress(s: SimState) -> dict[str, Any]:
    # Plausible mock values that drift slightly so the panel isn't frozen.
    drift = lambda base, amp: max(0, int(base + (random.random() - 0.5) * amp))
    return {
        "blocksWithLoss":         drift(420, 8),
        "worstHolesInBlock":      2 + (1 if random.random() < 0.1 else 0),
        "totalHolesInBlocks":     drift(1180, 20),
        "recoverableBursts":      drift(1700, 40),
        "criticalBursts":         drift(48, 4),
        "burstsExceedingFecSpan": drift(3, 1),
    }


def _config_echo() -> dict[str, Any]:
    return {
        "k": 8, "m": 4, "depth": 16, "symbolSize": 800,
        "lanIface": "eth0", "fsoIface": "eth1", "internalSymbolCrc": True,
    }


def snapshot(s: SimState, source: str = "mock") -> dict[str, Any]:
    _step(s)
    _maybe_emit_alert(s)
    return {
        "source": source,
        "generatedAt": int(time.time() * 1000),
        "link": _link(s),
        "throughput": list(s.history),
        "errors": _errors(s),
        "pipeline": _pipeline(s),
        "burstHistogram": _burst_histogram(),
        "system": _system(),
        "alerts": s.alerts,
        "decoderStress": _decoder_stress(s),
        "configEcho": _config_echo(),
    }


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------

SIM = SimState.fresh()
GATEWAY = GatewaySource(GATEWAY_SOCKET_PATH)
LOGS = LogManager()

RECORDER_INTERVAL_SEC = 1.0
_recorder_task: asyncio.Task[None] | None = None
_active_run_id: int | None = None


async def _recorder_loop() -> None:
    """Periodically persists current_snapshot() into the active run."""
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
            # Don't let a transient persistence error kill the recorder.
            await asyncio.sleep(1.0)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _recorder_task, _active_run_id
    GATEWAY.start()
    await LOGS.start()
    # End any leftover active run from a prior process and start fresh.
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


app = FastAPI(title="FSO Gateway Bridge", version="0.3.0", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=ALLOWED_ORIGINS,
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


def current_snapshot() -> dict[str, Any]:
    """Prefer live gateway data; fall back to mock if the UNIX socket is down
    or has not yet produced its first usable snapshot."""
    if GATEWAY.is_connected():
        snap = GATEWAY.snapshot()
        if snap is not None:
            return snap
    return snapshot(SIM)


def current_source() -> str:
    return "gateway" if GATEWAY.is_connected() and GATEWAY.snapshot() is not None else "mock"


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


@app.websocket("/ws/logs")
async def ws_logs(ws: WebSocket) -> None:
    await ws.accept()
    q = LOGS.subscribe()
    # Announce current mode so the UI can label the source correctly.
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
