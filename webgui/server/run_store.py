"""
SQLite-backed persistence for telemetry "runs".

A *run* is a contiguous span of recorded snapshots, typically one per
bridge process (auto-started at lifespan begin, ended at shutdown). The
user can also start/end runs manually via the API.

Schema is intentionally narrow — only the fields that the C daemon
actually produces (via control_server) and that the dashboard plots.
There are no slots for RSSI/SNR/BER-from-optics, per-packet latency,
or daemon-side CPU/memory: those do not exist in this software.

All public functions are sync; FastAPI handlers should call them via
`asyncio.to_thread` to keep the event loop responsive.
"""

from __future__ import annotations

import os
import sqlite3
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

DB_PATH = Path(os.environ.get("FSO_RUNS_DB", str(Path(__file__).parent / "runs.db")))
SCHEMA_VERSION = 2  # bump → triggers automatic drop+recreate of stale schemas


# ---------------------------------------------------------------------------
# Connection / schema
# ---------------------------------------------------------------------------


def _connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, isolation_level=None, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


_SCHEMA = [
    """
    CREATE TABLE IF NOT EXISTS schema_meta (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS runs (
        id           INTEGER PRIMARY KEY AUTOINCREMENT,
        name         TEXT NOT NULL,
        started_at   INTEGER NOT NULL,
        ended_at     INTEGER,
        notes        TEXT,
        sample_count INTEGER NOT NULL DEFAULT 0
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS samples (
        run_id             INTEGER NOT NULL,
        t                  INTEGER NOT NULL,
        source             TEXT NOT NULL,
        link_state         TEXT NOT NULL,
        link_quality_pct   REAL,
        tx_bps             REAL,
        rx_bps             REAL,
        tx_pps             REAL,
        rx_pps             REAL,
        blocks_attempted   INTEGER,
        blocks_recovered   INTEGER,
        blocks_failed      INTEGER,
        recovered_packets  INTEGER,
        failed_packets     INTEGER,
        crc_drops          INTEGER,
        symbol_loss_ratio  REAL,
        PRIMARY KEY (run_id, t),
        FOREIGN KEY (run_id) REFERENCES runs(id) ON DELETE CASCADE
    )
    """,
    "CREATE INDEX IF NOT EXISTS idx_samples_run ON samples(run_id, t)",
]


def _ensure_schema(conn: sqlite3.Connection) -> None:
    # Check stored schema version. If absent or stale, wipe + recreate.
    try:
        cur = conn.execute(
            "SELECT value FROM schema_meta WHERE key = 'version'"
        )
        row = cur.fetchone()
        stored = int(row["value"]) if row else 0
    except sqlite3.OperationalError:
        stored = 0

    if stored != SCHEMA_VERSION:
        conn.execute("DROP TABLE IF EXISTS samples")
        conn.execute("DROP TABLE IF EXISTS runs")
        conn.execute("DROP TABLE IF EXISTS schema_meta")

    for stmt in _SCHEMA:
        conn.execute(stmt)

    conn.execute(
        "INSERT OR REPLACE INTO schema_meta(key, value) VALUES ('version', ?)",
        (str(SCHEMA_VERSION),),
    )


@contextmanager
def _cursor() -> Iterator[sqlite3.Cursor]:
    conn = _connect()
    try:
        _ensure_schema(conn)
        cur = conn.cursor()
        yield cur
    finally:
        conn.close()


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


@dataclass
class RunSummary:
    id: int
    name: str
    started_at: int
    ended_at: int | None
    notes: str | None
    sample_count: int

    def to_dict(self) -> dict[str, Any]:
        return {
            "id":          self.id,
            "name":        self.name,
            "startedAt":   self.started_at,
            "endedAt":     self.ended_at,
            "notes":       self.notes,
            "sampleCount": self.sample_count,
            "active":      self.ended_at is None,
        }


def list_runs(limit: int = 100) -> list[RunSummary]:
    with _cursor() as cur:
        cur.execute(
            "SELECT id, name, started_at, ended_at, notes, sample_count "
            "FROM runs ORDER BY started_at DESC LIMIT ?",
            (limit,),
        )
        return [
            RunSummary(
                id=row["id"],
                name=row["name"],
                started_at=row["started_at"],
                ended_at=row["ended_at"],
                notes=row["notes"],
                sample_count=row["sample_count"],
            )
            for row in cur.fetchall()
        ]


def get_run(run_id: int) -> RunSummary | None:
    with _cursor() as cur:
        cur.execute(
            "SELECT id, name, started_at, ended_at, notes, sample_count "
            "FROM runs WHERE id = ?",
            (run_id,),
        )
        row = cur.fetchone()
        if row is None:
            return None
        return RunSummary(
            id=row["id"],
            name=row["name"],
            started_at=row["started_at"],
            ended_at=row["ended_at"],
            notes=row["notes"],
            sample_count=row["sample_count"],
        )


def create_run(name: str | None = None, notes: str | None = None) -> int:
    """Start a new run. Auto-ends any currently active run."""
    now_ms = int(time.time() * 1000)
    if name is None or not name.strip():
        ts = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(now_ms / 1000))
        name = f"session-{ts}"
    with _cursor() as cur:
        cur.execute(
            "UPDATE runs SET ended_at = ? WHERE ended_at IS NULL",
            (now_ms,),
        )
        cur.execute(
            "INSERT INTO runs(name, started_at, notes) VALUES (?, ?, ?)",
            (name.strip(), now_ms, (notes or None)),
        )
        return int(cur.lastrowid or 0)


def end_run(run_id: int) -> bool:
    now_ms = int(time.time() * 1000)
    with _cursor() as cur:
        cur.execute(
            "UPDATE runs SET ended_at = ? WHERE id = ? AND ended_at IS NULL",
            (now_ms, run_id),
        )
        return cur.rowcount > 0


def end_active_runs() -> int:
    now_ms = int(time.time() * 1000)
    with _cursor() as cur:
        cur.execute(
            "UPDATE runs SET ended_at = ? WHERE ended_at IS NULL",
            (now_ms,),
        )
        return cur.rowcount


def active_run_id() -> int | None:
    with _cursor() as cur:
        cur.execute(
            "SELECT id FROM runs WHERE ended_at IS NULL ORDER BY started_at DESC LIMIT 1"
        )
        row = cur.fetchone()
        return int(row["id"]) if row else None


def delete_run(run_id: int) -> bool:
    with _cursor() as cur:
        cur.execute("DELETE FROM runs WHERE id = ?", (run_id,))
        return cur.rowcount > 0


def append_sample(run_id: int, snap: dict[str, Any]) -> None:
    """Insert one snapshot row. Silently skips if (run_id, t) already exists."""
    link = snap.get("link", {})
    errors = snap.get("errors", {})
    throughput = snap.get("throughput") or []
    latest = throughput[-1] if throughput else {}
    t = int(snap.get("generatedAt") or (time.time() * 1000))
    with _cursor() as cur:
        try:
            cur.execute(
                """
                INSERT INTO samples (
                    run_id, t, source,
                    link_state, link_quality_pct,
                    tx_bps, rx_bps, tx_pps, rx_pps,
                    blocks_attempted, blocks_recovered, blocks_failed,
                    recovered_packets, failed_packets, crc_drops,
                    symbol_loss_ratio
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    run_id, t, snap.get("source", "unknown"),
                    link.get("state", "unknown"),
                    link.get("qualityPct"),
                    latest.get("txBps"),
                    latest.get("rxBps"),
                    latest.get("txPps"),
                    latest.get("rxPps"),
                    errors.get("blocksAttempted"),
                    errors.get("blocksRecovered"),
                    errors.get("blocksFailed"),
                    errors.get("recoveredPackets"),
                    errors.get("failedPackets"),
                    errors.get("crcDrops"),
                    errors.get("symbolLossRatio"),
                ),
            )
            cur.execute(
                "UPDATE runs SET sample_count = sample_count + 1 WHERE id = ?",
                (run_id,),
            )
        except sqlite3.IntegrityError:
            # Duplicate (run_id, t) — skip silently.
            pass


def get_samples(run_id: int, max_points: int = 1000) -> list[dict[str, Any]]:
    """Return samples for a run, downsampling evenly if there are too many."""
    with _cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS n FROM samples WHERE run_id = ?",
            (run_id,),
        )
        total = int(cur.fetchone()["n"])
        if total == 0:
            return []
        stride = max(1, total // max_points)
        cur.execute(
            """
            SELECT t, source, link_state, link_quality_pct,
                   tx_bps, rx_bps, tx_pps, rx_pps,
                   blocks_attempted, blocks_recovered, blocks_failed,
                   recovered_packets, failed_packets, crc_drops,
                   symbol_loss_ratio,
                   ROW_NUMBER() OVER (ORDER BY t) AS rn
            FROM samples WHERE run_id = ?
            ORDER BY t ASC
            """,
            (run_id,),
        )
        rows = cur.fetchall()
        if stride <= 1:
            picked = rows
        else:
            picked = [r for r in rows if (r["rn"] - 1) % stride == 0]
            if rows and picked[-1]["t"] != rows[-1]["t"]:
                picked.append(rows[-1])
        return [
            {
                "t":                r["t"],
                "source":           r["source"],
                "linkState":        r["link_state"],
                "linkQualityPct":   r["link_quality_pct"],
                "txBps":            r["tx_bps"],
                "rxBps":            r["rx_bps"],
                "txPps":            r["tx_pps"],
                "rxPps":            r["rx_pps"],
                "blocksAttempted":  r["blocks_attempted"],
                "blocksRecovered":  r["blocks_recovered"],
                "blocksFailed":     r["blocks_failed"],
                "recoveredPackets": r["recovered_packets"],
                "failedPackets":    r["failed_packets"],
                "crcDrops":         r["crc_drops"],
                "symbolLossRatio":  r["symbol_loss_ratio"],
            }
            for r in picked
        ]


def export_csv(run_id: int) -> str:
    with _cursor() as cur:
        cur.execute(
            """
            SELECT t, source, link_state, link_quality_pct,
                   tx_bps, rx_bps, tx_pps, rx_pps,
                   blocks_attempted, blocks_recovered, blocks_failed,
                   recovered_packets, failed_packets, crc_drops,
                   symbol_loss_ratio
            FROM samples WHERE run_id = ?
            ORDER BY t ASC
            """,
            (run_id,),
        )
        rows = cur.fetchall()
    header = ",".join([
        "t_ms", "source", "link_state", "link_quality_pct",
        "tx_bps", "rx_bps", "tx_pps", "rx_pps",
        "blocks_attempted", "blocks_recovered", "blocks_failed",
        "recovered_packets", "failed_packets", "crc_drops",
        "symbol_loss_ratio",
    ])
    lines = [header]
    for r in rows:
        cells = [
            str(r["t"]), r["source"], r["link_state"],
            _csv_num(r["link_quality_pct"]),
            _csv_num(r["tx_bps"]),    _csv_num(r["rx_bps"]),
            _csv_num(r["tx_pps"]),    _csv_num(r["rx_pps"]),
            _csv_num(r["blocks_attempted"]), _csv_num(r["blocks_recovered"]),
            _csv_num(r["blocks_failed"]),    _csv_num(r["recovered_packets"]),
            _csv_num(r["failed_packets"]),   _csv_num(r["crc_drops"]),
            _csv_num(r["symbol_loss_ratio"]),
        ]
        lines.append(",".join(cells))
    return "\n".join(lines)


def _csv_num(v: Any) -> str:
    if v is None:
        return ""
    if isinstance(v, float):
        return f"{v:.6g}"
    return str(v)


def stats(run_id: int) -> dict[str, Any] | None:
    with _cursor() as cur:
        cur.execute(
            """
            SELECT
                MIN(t) AS t_min, MAX(t) AS t_max, COUNT(*) AS n,
                AVG(tx_bps) AS tx_avg, MAX(tx_bps) AS tx_peak,
                AVG(rx_bps) AS rx_avg, MAX(rx_bps) AS rx_peak,
                AVG(link_quality_pct) AS quality_avg,
                MIN(link_quality_pct) AS quality_min,
                AVG(symbol_loss_ratio) AS sym_loss_avg,
                MAX(symbol_loss_ratio) AS sym_loss_peak,
                MAX(blocks_attempted) AS blocks_attempted,
                MAX(blocks_recovered) AS blocks_recovered,
                MAX(blocks_failed)    AS blocks_failed,
                MAX(crc_drops)        AS crc_drops
            FROM samples WHERE run_id = ?
            """,
            (run_id,),
        )
        row = cur.fetchone()
        if not row or row["n"] == 0:
            return None
        return {k: row[k] for k in row.keys()}
