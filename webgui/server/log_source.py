"""
Log sources for the bridge.

Two implementations:
  - FileTailLogSource: tail a log file (the C daemon's stderr, redirected via
    `fso_gw_runner ... 2>> /tmp/fso_gw.log`). Uses simple polling with seek to
    keep things portable (inotify is Linux-only but would add a dep).
  - MockLogSource: synthesizes realistic-looking log events for development
    when the daemon is not running.

A LogManager picks the active source: if the log file exists and is non-empty,
it tails that; otherwise it emits mock events. Either way, events are fanned
out via asyncio.Queue subscribers to any number of WebSocket clients.
"""

from __future__ import annotations

import asyncio
import os
import random
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

LOG_FILE_PATH = Path(os.environ.get("FSO_LOG_FILE", "/tmp/fso_gw.log"))
TAIL_POLL_MS = 150         # how often to check file for new data
TAIL_MAX_LINE = 4096       # safety cap per line
RECONNECT_SECONDS = 3.0
SUBSCRIBER_QUEUE_CAP = 512 # per-subscriber backlog before drops


# ---------------------------------------------------------------------------
# Log event shape
# ---------------------------------------------------------------------------

LEVELS = ("DEBUG", "INFO", "WARN", "ERROR")


@dataclass
class LogEvent:
    ts_ms: int
    level: str   # one of LEVELS
    module: str  # e.g. "TX", "RX", "FEC", "GATEWAY", "SYSTEM"
    message: str
    raw: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "ts_ms": self.ts_ms,
            "level": self.level,
            "module": self.module,
            "message": self.message,
            "raw": self.raw,
        }


# Parse lines emitted by src/logging.c. Expected shape (may vary):
#   [YYYY-MM-DD HH:MM:SS.uuuuuu] [LEVEL] [thread_id] message
# `message` typically starts with either `[module]` or `module: ...`.
_LOG_LINE = re.compile(
    r"^\[(?P<ts>[^\]]+)\]\s+"
    r"\[(?P<level>[A-Z]+)\]\s+"
    r"(?:\[[^\]]+\]\s+)?"  # optional [tid]
    r"(?P<rest>.*)$"
)
_MODULE_BRACKET = re.compile(r"^\s*\[(?P<mod>[^\]\s]+)\]\s*(?P<msg>.*)$")
_MODULE_COLON   = re.compile(r"^\s*(?P<mod>[A-Za-z_][A-Za-z0-9_]{1,31}):\s*(?P<msg>.*)$")


def parse_line(raw: str) -> LogEvent:
    """Best-effort parse. Falls back to a SYSTEM info event if no match."""
    raw = raw.rstrip("\r\n")
    if not raw:
        raw = " "
    m = _LOG_LINE.match(raw)
    if not m:
        return LogEvent(
            ts_ms=int(time.time() * 1000),
            level="INFO",
            module="SYSTEM",
            message=raw,
            raw=raw,
        )

    level = m.group("level").upper()
    if level not in LEVELS:
        level = "INFO"

    # Convert timestamp if possible. We accept whatever the logger writes and
    # fall back to now() if parsing fails — we just need a monotonic-ish ms.
    ts_ms = _parse_ts_ms(m.group("ts"))

    rest = m.group("rest")
    mb = _MODULE_BRACKET.match(rest)
    if mb:
        module = mb.group("mod").upper()
        message = mb.group("msg")
    else:
        mc = _MODULE_COLON.match(rest)
        if mc:
            module = mc.group("mod").upper()
            message = mc.group("msg")
        else:
            module = "SYSTEM"
            message = rest

    return LogEvent(ts_ms=ts_ms, level=level, module=module, message=message, raw=raw)


def _parse_ts_ms(ts: str) -> int:
    # Expected: "YYYY-MM-DD HH:MM:SS.uuuuuu"
    try:
        import datetime as dt
        d = dt.datetime.strptime(ts.strip(), "%Y-%m-%d %H:%M:%S.%f")
        return int(d.timestamp() * 1000)
    except Exception:
        return int(time.time() * 1000)


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------


class _SourceBase:
    async def run(self, emit) -> None:
        raise NotImplementedError


class FileTailLogSource(_SourceBase):
    """Tail a file line-by-line. Re-opens if the file is rotated."""

    def __init__(self, path: Path) -> None:
        self.path = path

    async def run(self, emit) -> None:
        fd: "object | None" = None
        inode: int | None = None
        buf = b""

        while True:
            try:
                st = self.path.stat()
            except FileNotFoundError:
                # File not (yet) there: caller should have fallen back to mock.
                await asyncio.sleep(RECONNECT_SECONDS)
                continue

            if fd is None or inode != st.st_ino:
                if fd is not None:
                    try: fd.close()  # type: ignore[attr-defined]
                    except Exception: pass
                try:
                    fd = open(self.path, "rb")
                except FileNotFoundError:
                    fd = None
                    await asyncio.sleep(RECONNECT_SECONDS)
                    continue
                inode = st.st_ino
                # Start at end to avoid replaying full history. Phase 4B could
                # add an "initial backlog" option.
                fd.seek(0, os.SEEK_END)
                buf = b""

            chunk = fd.read(65536)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line:
                        continue
                    text = line.decode("utf-8", errors="replace")
                    if len(text) > TAIL_MAX_LINE:
                        text = text[:TAIL_MAX_LINE] + "…"
                    emit(parse_line(text))
            else:
                # Check if file shrank (rotation handled on next iteration).
                if st.st_size < fd.tell():
                    fd.seek(0)
                    buf = b""
                await asyncio.sleep(TAIL_POLL_MS / 1000.0)


class MockLogSource(_SourceBase):
    """Synthesize a stream of realistic-looking gateway log events."""

    MODULES = [
        ("GATEWAY",     "INFO",  ["TX thread started", "RX thread started",
                                  "Both pipeline threads running",
                                  "Created: lan=eth0 fso=eth1 k=8 m=4 depth=16"]),
        ("TX",          "INFO",  ["fragment pkt_id={pid} len={len} -> symbols={sym}",
                                  "block_builder block {bid} complete (K={k})",
                                  "fso_tx burst transmitted size={sz}"]),
        ("RX",          "INFO",  ["reassemble_packet success pkt_id={pid} len={len}",
                                  "deinterleaver flush depth={d} blocks_flushed=1",
                                  "packet decoded, forwarded to LAN"]),
        ("FEC",         "INFO",  ["encode_block block_id={bid} symbols_generated={sym}",
                                  "decode_block block_id={bid} recovered K={k} M={m}"]),
        ("ARP",         "INFO",  ["proxy_arp target 192.168.50.{oct} handled locally",
                                  "arp_cache learned {mac} -> 192.168.50.{oct}"]),
        ("INTERLEAVER", "DEBUG", ["flush timeout depth=16 rows_flushed=4",
                                  "block_seq={bid} mapping row={row}"]),
        ("LINK",        "INFO",  ["fso_tx burst transmitted size={sz}"]),
        ("LINK",        "WARN",  ["CRC mismatch on symbol {sid} — treating as erasure"]),
        ("FEC",         "WARN",  ["block_id={bid} required {m} repair symbols"]),
        ("FEC",         "ERROR", ["block_id={bid} FEC decode FAILED — {missing} missing"]),
    ]

    async def run(self, emit) -> None:
        pid = 2300
        bid = 120
        while True:
            module, level, templates = random.choices(
                self.MODULES,
                weights=[14, 26, 26, 16, 7, 4, 2, 2, 2, 1],
                k=1,
            )[0]
            tmpl = random.choice(templates)
            msg = tmpl.format(
                pid=pid,
                len=random.randint(60, 1500),
                sym=random.randint(2, 16),
                bid=bid,
                k=random.choice([2, 4, 8]),
                m=random.choice([1, 2, 4]),
                d=random.randint(2, 16),
                row=random.randint(0, 15),
                oct=random.randint(2, 254),
                mac=":".join(f"{random.randint(0,255):02x}" for _ in range(6)),
                sid=random.randint(1000, 99999),
                sz=random.randint(64, 9000),
                missing=random.randint(3, 9),
            )
            pid += 1
            if random.random() < 0.15:
                bid += 1

            emit(LogEvent(
                ts_ms=int(time.time() * 1000),
                level=level,
                module=module,
                message=msg,
                raw=f"[mock] [{level}] [{module}] {msg}",
            ))

            # Uneven cadence — mostly quick, occasional pauses.
            await asyncio.sleep(random.uniform(0.08, 0.45))


# ---------------------------------------------------------------------------
# Manager + fan-out
# ---------------------------------------------------------------------------


class LogManager:
    def __init__(self, log_path: Path = LOG_FILE_PATH) -> None:
        self.path = log_path
        self._subscribers: set[asyncio.Queue[LogEvent]] = set()
        self._task: asyncio.Task[None] | None = None
        self._stop = asyncio.Event()
        self._mode: str = "idle"  # "tail" | "mock" | "idle"

    @property
    def mode(self) -> str:
        return self._mode

    async def start(self) -> None:
        if self._task is None:
            self._stop.clear()
            self._task = asyncio.create_task(self._run(), name="log-manager")

    async def stop(self) -> None:
        self._stop.set()
        if self._task:
            try:
                await asyncio.wait_for(self._task, timeout=2.0)
            except asyncio.TimeoutError:
                self._task.cancel()
        self._task = None

    def subscribe(self) -> asyncio.Queue[LogEvent]:
        q: asyncio.Queue[LogEvent] = asyncio.Queue(maxsize=SUBSCRIBER_QUEUE_CAP)
        self._subscribers.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue[LogEvent]) -> None:
        self._subscribers.discard(q)

    def _emit(self, ev: LogEvent) -> None:
        for q in list(self._subscribers):
            try:
                q.put_nowait(ev)
            except asyncio.QueueFull:
                # Drop oldest so new events get through.
                try:
                    q.get_nowait()
                    q.put_nowait(ev)
                except Exception:
                    pass

    async def _run(self) -> None:
        """Pick a source based on file presence and only switch when that changes.
        This avoids cancelling a healthy source every few seconds."""
        current_mode: str | None = None
        task: asyncio.Task[None] | None = None

        async def cancel_current() -> None:
            nonlocal task
            if task is not None:
                task.cancel()
                try:
                    await task
                except (asyncio.CancelledError, Exception):
                    pass
                task = None

        while not self._stop.is_set():
            desired = "tail" if (self.path.exists() and self.path.is_file()) else "mock"
            if desired != current_mode:
                await cancel_current()
                source: _SourceBase = (
                    FileTailLogSource(self.path) if desired == "tail" else MockLogSource()
                )
                task = asyncio.create_task(source.run(self._emit), name=f"log-{desired}")
                current_mode = desired
                self._mode = desired

            try:
                await asyncio.wait_for(self._stop.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                pass  # re-evaluate desired mode

        await cancel_current()
        self._mode = "idle"
