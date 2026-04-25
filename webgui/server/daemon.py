"""
Phase 3B — Gateway daemon supervisor.

Lets the bridge spawn and manage a single fso_gw_runner subprocess so the
"Start / Stop / Restart" buttons in the Configuration page actually do
something. The supervisor:

  * builds an argv from the persisted config_store.GatewayConfig
  * runs the binary as a child process, redirecting stderr to the same
    file the LogManager already tails (so logs flow into /ws/logs the
    moment the daemon starts)
  * tracks state: stopped / starting / running / stopping / failed
  * stop() does SIGTERM, waits up to STOP_GRACE_SEC, then SIGKILL
  * detects unexpected exits via a watcher task and flips state to
    "failed" with last_error set to whatever was on the wait()'s rc
  * restart() = stop() then start()

Environment knobs
-----------------
  FSO_DAEMON_BINARY   absolute path to the binary to launch.
                      Default: <repo>/build/bin/fso_gw_runner.
                      For local development you can point this at
                      build/bin/control_server_demo to exercise the same
                      lifecycle without root or NICs.

  FSO_DAEMON_SUDO     "1" → prepend `sudo -n` to the argv. Use this when
                      the bridge runs as a non-root user but the daemon
                      needs CAP_NET_RAW (libpcap). The user must have
                      configured passwordless sudo for the binary.

  FSO_DAEMON_LOG      stderr redirect target. Defaults to
                      /tmp/fso_gw.log so the existing LogManager picks
                      up the daemon's output automatically.

  FSO_DAEMON_KILL_ON_EXIT  "1" → on bridge shutdown, terminate the
                           daemon too. Default off so a bridge restart
                           does not blip traffic.

The supervisor is *not* designed to adopt a stray daemon left running
from a previous bridge process. It always considers itself the only
manager of its own child.
"""

from __future__ import annotations

import asyncio
import os
import shlex
import shutil
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from config_store import GatewayConfig, load as config_load

# Default to the repo binary; override via env in lab deployments.
DEFAULT_BINARY = str(
    (Path(__file__).resolve().parent.parent.parent / "build" / "bin" / "fso_gw_runner")
)
DEFAULT_LOG = "/tmp/fso_gw.log"
STOP_GRACE_SEC = 4.0


class DaemonError(RuntimeError):
    pass


def _binary_path() -> str:
    return os.environ.get("FSO_DAEMON_BINARY", DEFAULT_BINARY)


def _use_sudo() -> bool:
    return os.environ.get("FSO_DAEMON_SUDO", "0") == "1"


def _log_path() -> str:
    return os.environ.get("FSO_DAEMON_LOG", DEFAULT_LOG)


def _kill_on_exit() -> bool:
    return os.environ.get("FSO_DAEMON_KILL_ON_EXIT", "0") == "1"


def _build_argv(cfg: GatewayConfig) -> list[str]:
    """Translate a GatewayConfig into the runner's CLI."""
    argv: list[str] = []
    if _use_sudo():
        argv += ["sudo", "-n"]
    argv += [
        _binary_path(),
        "--lan-iface", cfg.lan_iface,
        "--fso-iface", cfg.fso_iface,
        "--k", str(cfg.k),
        "--m", str(cfg.m),
        "--depth", str(cfg.depth),
        "--symbol-size", str(cfg.symbol_size),
    ]
    return argv


class DaemonSupervisor:
    def __init__(self) -> None:
        self._proc: subprocess.Popen[bytes] | None = None
        self._state: str = "stopped"
        self._started_at: float | None = None
        self._argv: list[str] = []
        self._cfg: GatewayConfig | None = None
        self._last_error: str | None = None
        self._lock = asyncio.Lock()
        self._watcher_task: asyncio.Task[None] | None = None

    # ---- public state ---------------------------------------------------

    def status(self) -> dict[str, Any]:
        return {
            "state":          self._state,
            "pid":            self._proc.pid if self._proc else None,
            "startedAt":      int(self._started_at * 1000) if self._started_at else None,
            "uptimeSec":      (time.time() - self._started_at) if self._started_at and self._proc else None,
            "binary":         _binary_path(),
            "binaryFound":    self._binary_present(),
            "useSudo":        _use_sudo(),
            "logFile":        _log_path(),
            "command":        " ".join(shlex.quote(a) for a in self._argv) if self._argv else None,
            "config":         _cfg_summary(self._cfg) if self._cfg else None,
            "lastError":      self._last_error,
        }

    @staticmethod
    def _binary_present() -> bool:
        path = _binary_path()
        if "/" in path:
            return os.access(path, os.X_OK)
        return shutil.which(path) is not None

    # ---- lifecycle ------------------------------------------------------

    async def start(self) -> dict[str, Any]:
        async with self._lock:
            if self._state in ("starting", "running"):
                return self.status()
            cfg = await asyncio.to_thread(config_load)
            argv = _build_argv(cfg)
            if not self._binary_present():
                self._state = "failed"
                self._last_error = f"binary not found or not executable: {_binary_path()}"
                return self.status()

            self._state = "starting"
            self._cfg = cfg
            self._argv = argv
            self._last_error = None

            try:
                # Append-mode so the LogManager's tail picks it up cleanly.
                log = open(_log_path(), "ab")
                self._proc = subprocess.Popen(
                    argv,
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=log,
                    close_fds=True,
                    start_new_session=True,  # detach from bridge's process group
                )
                log.close()
            except FileNotFoundError as e:
                self._state = "failed"
                self._last_error = str(e)
                return self.status()
            except Exception as e:
                self._state = "failed"
                self._last_error = str(e)
                return self.status()

            self._started_at = time.time()

            # Briefly check that it didn't exit immediately.
            await asyncio.sleep(0.2)
            rc = self._proc.poll()
            if rc is not None:
                self._state = "failed"
                self._last_error = f"daemon exited immediately (rc={rc})"
                self._proc = None
                self._started_at = None
                return self.status()

            self._state = "running"
            self._watcher_task = asyncio.create_task(self._watch())
            return self.status()

    async def stop(self) -> dict[str, Any]:
        async with self._lock:
            proc = self._proc
            if proc is None or self._state in ("stopped", "stopping"):
                return self.status()
            self._state = "stopping"
            self._last_error = None

        # Lock released so _watch() can update state on natural exit.
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass

        try:
            await asyncio.wait_for(asyncio.to_thread(proc.wait), timeout=STOP_GRACE_SEC)
        except asyncio.TimeoutError:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            await asyncio.to_thread(proc.wait)

        async with self._lock:
            self._proc = None
            self._started_at = None
            self._state = "stopped"
            if self._watcher_task:
                self._watcher_task.cancel()
                self._watcher_task = None
        return self.status()

    async def restart(self) -> dict[str, Any]:
        await self.stop()
        return await self.start()

    async def shutdown(self) -> None:
        """Called from the FastAPI lifespan on bridge shutdown."""
        if _kill_on_exit() and self._proc is not None:
            await self.stop()

    # ---- internals ------------------------------------------------------

    async def _watch(self) -> None:
        proc = self._proc
        if proc is None:
            return
        try:
            rc = await asyncio.to_thread(proc.wait)
        except asyncio.CancelledError:
            return
        async with self._lock:
            # If we're already stopping, the stop() path will set state.
            if self._state == "stopping":
                return
            self._proc = None
            self._started_at = None
            if rc == 0:
                self._state = "stopped"
                self._last_error = None
            else:
                self._state = "failed"
                self._last_error = f"daemon exited unexpectedly (rc={rc})"


def _cfg_summary(cfg: GatewayConfig) -> dict[str, Any]:
    return {
        "lanIface":          cfg.lan_iface,
        "fsoIface":          cfg.fso_iface,
        "k":                 cfg.k,
        "m":                 cfg.m,
        "depth":             cfg.depth,
        "symbolSize":        cfg.symbol_size,
        "internalSymbolCrc": cfg.internal_symbol_crc,
    }
