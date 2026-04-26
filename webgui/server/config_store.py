"""
Config store — persists user-editable gateway parameters to a YAML file.

This mirrors `struct config` in include/config.h plus a schema version. Phase
3B will read this file when (re)launching the daemon. For Phase 3A the store
is standalone: UI writes here, nothing else reads yet.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Any

import yaml

CONFIG_PATH = Path(__file__).parent / "config.yaml"
SCHEMA_VERSION = 1

# Hard bounds aligned with the actual C limits — no artificial caps.
#   k:           >= 2 (fec_wrapper.c requires it for Wirehair to work)
#                <= 256 (MAX_SYMBOLS_PER_BLOCK in types.h)
#   m:           >= 0 (C allows zero-parity)
#                <= 256 (same MAX, with cross-check K+M<=256 in interleaver)
#   depth:       >= 1 (interleaver.c rejects 0)
#                <= 1024 (no hard C limit; cap chosen to keep matrix memory
#                practical — grows linearly with depth)
#   symbol_size: >= 1
#                <= 9000 (MAX_SYMBOL_DATA_SIZE in types.h:39, jumbo frame)
BOUNDS = {
    "k": (2, 256),
    "m": (0, 256),
    "depth": (1, 1024),
    "symbol_size": (1, 9000),
}


@dataclass
class GatewayConfig:
    lan_iface: str = "eth0"
    fso_iface: str = "eth1"
    k: int = 8
    m: int = 4
    depth: int = 16
    symbol_size: int = 800
    internal_symbol_crc: bool = True
    profile_name: str = "LAB-TEST"

    @classmethod
    def defaults(cls) -> "GatewayConfig":
        return cls()


class ConfigError(ValueError):
    """Raised when an incoming config payload fails validation."""


def _ensure_str(v: Any, field_name: str, max_len: int = 31) -> str:
    if not isinstance(v, str):
        raise ConfigError(f"{field_name}: expected string, got {type(v).__name__}")
    v = v.strip()
    if not v:
        raise ConfigError(f"{field_name}: must not be empty")
    if len(v) > max_len:
        raise ConfigError(f"{field_name}: must be at most {max_len} characters")
    return v


def _ensure_int(v: Any, field_name: str, lo: int, hi: int) -> int:
    if isinstance(v, bool) or not isinstance(v, int):
        raise ConfigError(f"{field_name}: expected integer")
    if v < lo or v > hi:
        raise ConfigError(f"{field_name}: must be between {lo} and {hi}")
    return v


def _ensure_bool(v: Any, field_name: str) -> bool:
    if not isinstance(v, bool):
        raise ConfigError(f"{field_name}: expected boolean")
    return v


def validate(payload: dict[str, Any]) -> GatewayConfig:
    """Validate an incoming dict and return a concrete GatewayConfig."""
    defaults = GatewayConfig.defaults()
    try:
        return GatewayConfig(
            lan_iface=_ensure_str(payload.get("lan_iface", defaults.lan_iface), "lan_iface"),
            fso_iface=_ensure_str(payload.get("fso_iface", defaults.fso_iface), "fso_iface"),
            k=_ensure_int(payload.get("k", defaults.k), "k", *BOUNDS["k"]),
            m=_ensure_int(payload.get("m", defaults.m), "m", *BOUNDS["m"]),
            depth=_ensure_int(payload.get("depth", defaults.depth), "depth", *BOUNDS["depth"]),
            symbol_size=_ensure_int(
                payload.get("symbol_size", defaults.symbol_size),
                "symbol_size",
                *BOUNDS["symbol_size"],
            ),
            internal_symbol_crc=_ensure_bool(
                payload.get("internal_symbol_crc", defaults.internal_symbol_crc),
                "internal_symbol_crc",
            ),
            profile_name=_ensure_str(
                payload.get("profile_name", defaults.profile_name),
                "profile_name",
                max_len=32,
            ),
        )
    except ConfigError:
        raise
    except Exception as e:
        raise ConfigError(f"invalid config payload: {e}")


def load() -> GatewayConfig:
    """Return the persisted config, or defaults if the file doesn't exist / is invalid."""
    if not CONFIG_PATH.exists():
        return GatewayConfig.defaults()
    try:
        with CONFIG_PATH.open("r") as f:
            data = yaml.safe_load(f) or {}
        body = data.get("config", {}) if isinstance(data, dict) else {}
        return validate(body)
    except (yaml.YAMLError, ConfigError):
        return GatewayConfig.defaults()


def save(cfg: GatewayConfig) -> None:
    """Atomically write config to disk."""
    out = {
        "schema_version": SCHEMA_VERSION,
        "config": asdict(cfg),
    }
    tmp = CONFIG_PATH.with_suffix(".tmp")
    with tmp.open("w") as f:
        yaml.safe_dump(out, f, default_flow_style=False, sort_keys=False)
    tmp.replace(CONFIG_PATH)
