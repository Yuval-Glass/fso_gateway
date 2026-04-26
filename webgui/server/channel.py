"""
Channel impairment control via Linux `tc qdisc` netem.

Mirrors the script-driven workflow from scripts/two_machine_run_test.sh
which applies a Gilbert-Elliott burst-loss model on the FSO interface to
exercise the FEC layer:

    tc qdisc add dev IFACE root netem loss gemodel ENTER% EXIT% LOSS% 0%

Read state with `tc qdisc show dev IFACE`. We parse only what we need
(presence of netem, loss model, percentages) — anything richer just shows
up in the raw text returned by GET.

Requires `tc` (iproute2) and root. The bridge is not normally root; users
are expected to either grant `tc` via `sudo` NOPASSWD, run the bridge as
root in the lab, or set FSO_TC_SUDO=1 to prepend `sudo` to every call.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from typing import Any

USE_SUDO = os.environ.get("FSO_TC_SUDO", "0") == "1"
DEFAULT_IFACE = os.environ.get("FSO_IFACE", "enp1s0f1np1")


class TcNotAvailable(RuntimeError):
    """Raised when /sbin/tc (or equivalent) is missing on PATH."""


def _tc_argv(args: list[str]) -> list[str]:
    if shutil.which("tc") is None:
        raise TcNotAvailable("tc binary not found on PATH")
    base = ["sudo", "-n", "tc"] if USE_SUDO else ["tc"]
    return base + args


def _run(args: list[str]) -> tuple[int, str, str]:
    p = subprocess.run(
        _tc_argv(args),
        capture_output=True,
        text=True,
        timeout=5,
    )
    return p.returncode, p.stdout, p.stderr


_NETEM_RE = re.compile(r"netem\b")
_LOSS_GEMODEL_RE = re.compile(
    r"loss gemodel\s+(?P<enter>[\d.]+)%\s+(?P<exit>[\d.]+)%\s+(?P<loss>[\d.]+)%(?:\s+(?P<recover>[\d.]+)%)?"
)
_LOSS_PCT_RE = re.compile(r"loss\s+(?P<pct>[\d.]+)%")


def show(iface: str = DEFAULT_IFACE) -> dict[str, Any]:
    """Read the current root qdisc on iface."""
    if shutil.which("tc") is None:
        return {
            "iface": iface,
            "available": False,
            "active": False,
            "raw": "",
            "error": "tc binary not found",
        }
    rc, out, err = _run(["qdisc", "show", "dev", iface])
    if rc != 0:
        return {
            "iface": iface,
            "available": True,
            "active": False,
            "raw": out,
            "error": err.strip() or f"tc returned rc={rc}",
        }

    has_netem = bool(_NETEM_RE.search(out))
    parsed: dict[str, Any] = {
        "iface": iface,
        "available": True,
        "active": has_netem,
        "raw": out.strip(),
        "model": None,
        "lossPct": None,
        "enterPct": None,
        "exitPct": None,
    }
    m = _LOSS_GEMODEL_RE.search(out)
    if m:
        parsed["model"]    = "gemodel"
        parsed["enterPct"] = float(m.group("enter"))
        parsed["exitPct"]  = float(m.group("exit"))
        parsed["lossPct"]  = float(m.group("loss"))
    else:
        m2 = _LOSS_PCT_RE.search(out)
        if m2:
            parsed["model"]   = "uniform"
            parsed["lossPct"] = float(m2.group("pct"))
    return parsed


def clear(iface: str = DEFAULT_IFACE) -> dict[str, Any]:
    """Remove root qdisc — equivalent to `tc qdisc del dev IFACE root`."""
    rc, out, err = _run(["qdisc", "del", "dev", iface, "root"])
    # rc != 0 is common when there's nothing to delete; treat as benign.
    return {"ok": True, "iface": iface, "rc": rc, "stderr": err.strip()}


def apply_gemodel(
    iface: str,
    enter_pct: float,
    exit_pct: float,
    loss_pct: float,
) -> dict[str, Any]:
    """Apply Gilbert-Elliott burst loss with the given percentages."""
    for v in (enter_pct, exit_pct, loss_pct):
        if not (0.0 <= v <= 100.0):
            raise ValueError(f"percentage out of range: {v}")
    # Always start clean so callers don't have to worry about replace-vs-add.
    clear(iface)
    rc, out, err = _run([
        "qdisc", "add", "dev", iface, "root", "netem",
        "loss", "gemodel",
        f"{enter_pct}%", f"{exit_pct}%", f"{loss_pct}%", "0%",
    ])
    if rc != 0:
        return {
            "ok": False,
            "iface": iface,
            "rc": rc,
            "stderr": err.strip(),
            "command": " ".join(_tc_argv([
                "qdisc", "add", "dev", iface, "root", "netem",
                "loss", "gemodel",
                f"{enter_pct}%", f"{exit_pct}%", f"{loss_pct}%", "0%",
            ])),
        }
    return {"ok": True, "iface": iface, "rc": 0, "stderr": err.strip()}
