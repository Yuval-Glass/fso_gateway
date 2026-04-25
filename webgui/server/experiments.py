"""
Reads experiment artefacts produced by scripts/two_machine_run_test.sh.

Each successful run drops two files in `build/stats/`:
  * gateway_stats_<TS>.csv      — single-row scalar summary
  * burst_histogram_<TS>.csv    — per-bucket counts
plus an `experiment_<TS>.txt` summary that bundles iperf3 results +
gateway log + the CSV. The bridge surfaces those files in the Analytics
page so the user does not have to switch back to the terminal.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any

DEFAULT_ROOT = Path(os.environ.get(
    "FSO_EXPERIMENTS_DIR",
    Path(__file__).resolve().parent.parent.parent / "build" / "stats",
))

_EXP_RE = re.compile(r"experiment_(\d{8}_\d{6})\.txt$")


def _root() -> Path:
    return DEFAULT_ROOT


def list_experiments() -> list[dict[str, Any]]:
    root = _root()
    if not root.exists():
        return []
    items: list[dict[str, Any]] = []
    for p in sorted(root.glob("experiment_*.txt"), key=lambda x: x.stat().st_mtime, reverse=True):
        m = _EXP_RE.search(p.name)
        if not m:
            continue
        ts_token = m.group(1)
        st = p.stat()
        items.append({
            "name":   p.name,
            "ts":     ts_token,         # YYYYMMDD_HHMMSS
            "size":   st.st_size,
            "mtime":  int(st.st_mtime * 1000),
        })
    return items


def read_experiment(name: str) -> dict[str, Any]:
    root = _root()
    # No traversal — only basenames.
    if "/" in name or "\\" in name or not name.startswith("experiment_"):
        raise FileNotFoundError(name)
    p = root / name
    if not p.exists():
        raise FileNotFoundError(str(p))
    text = p.read_text(errors="replace")
    return {
        "name":   p.name,
        "size":   p.stat().st_size,
        "mtime":  int(p.stat().st_mtime * 1000),
        "text":   text,
        "summary": _summarize(text),
    }


# ---------------------------------------------------------------------------
# Summary parser — best-effort, no hard requirements on file layout
# ---------------------------------------------------------------------------

_PARAM_RE     = re.compile(r"^\s*(GW_K|GW_M|GW_DEPTH|GW_SYMBOL_SIZE|BURST_LOSS_PCT|BURST_ENTER_PCT|BURST_EXIT_PCT|BASELINE_MODE|IPERF_DURATION|IPERF_BITRATE|IPERF_UDP|IPERF_PARALLEL)\s*=\s*(\S+)\s*$", re.MULTILINE)
_PROTO_RE     = re.compile(r"^Protocol:\s*(\S+)", re.MULTILINE)
_THROUGHPUT_RE = re.compile(r"^Total throughput:\s*([\d.]+)\s*(\S+)", re.MULTILINE)
_DATA_RE      = re.compile(r"^Total data transferred:\s*([\d.]+)\s*(\S+)", re.MULTILINE)
_LOSS_RE      = re.compile(r"^Packet loss %:\s*([\d.]+)%", re.MULTILINE)
_MODE_RE      = re.compile(r"Mode:\s*(BASELINE|FSO GATEWAY[^\n]*)", re.MULTILINE)


def _summarize(text: str) -> dict[str, Any]:
    params: dict[str, str] = {}
    for m in _PARAM_RE.finditer(text):
        params[m.group(1)] = m.group(2)

    summary: dict[str, Any] = {"params": params}

    m = _MODE_RE.search(text)
    if m: summary["mode"] = m.group(1).strip()

    m = _PROTO_RE.search(text)
    if m: summary["protocol"] = m.group(1)

    m = _THROUGHPUT_RE.search(text)
    if m:
        try: summary["throughput"] = {"value": float(m.group(1)), "unit": m.group(2)}
        except ValueError: pass

    m = _DATA_RE.search(text)
    if m:
        try: summary["totalData"] = {"value": float(m.group(1)), "unit": m.group(2)}
        except ValueError: pass

    m = _LOSS_RE.search(text)
    if m:
        try: summary["lossPct"] = float(m.group(1))
        except ValueError: pass

    return summary
