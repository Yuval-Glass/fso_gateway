#!/bin/bash
# start_gui.sh — starts the FSO Gateway web GUI
#
# Runs two processes:
#   1. Python bridge (FastAPI) on port 8000  — reads from /tmp/fso_gw.sock
#   2. Next.js frontend on port 3000         — connects to bridge
#
# The gateway daemon itself (fso_gw_runner) can be started/stopped from
# the Configuration page in the GUI, or manually with:
#   sudo build/bin/fso_gw_runner --lan-iface <iface> --fso-iface <iface> [options]

set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$REPO/webgui/server"
FRONTEND_DIR="$REPO/webgui"
NODE_BIN="/usr/local/node20/bin"
UV_BIN="$HOME/.local/bin/uv"

# ---- sanity checks --------------------------------------------------------
if [ ! -f "$BRIDGE_DIR/.venv/bin/python" ]; then
    echo "[start_gui] Python venv not found. Run: cd webgui/server && uv sync"
    exit 1
fi
if [ ! -d "$FRONTEND_DIR/.next" ]; then
    echo "[start_gui] Next.js not built. Run: cd webgui && PATH=/usr/local/node20/bin:\$PATH npm run build"
    exit 1
fi

# ---- cleanup on exit ------------------------------------------------------
cleanup() {
    echo ""
    echo "[start_gui] Shutting down..."
    kill "$BRIDGE_PID" "$FRONTEND_PID" 2>/dev/null
    wait "$BRIDGE_PID" "$FRONTEND_PID" 2>/dev/null
    echo "[start_gui] Done."
}
trap cleanup EXIT INT TERM

# ---- start Python bridge --------------------------------------------------
echo "[start_gui] Starting Python bridge on http://localhost:8000 ..."
cd "$BRIDGE_DIR"
"$UV_BIN" run uvicorn main:app --host 0.0.0.0 --port 8000 &
BRIDGE_PID=$!

# give bridge a moment to bind
sleep 1

# ---- start Next.js frontend -----------------------------------------------
echo "[start_gui] Starting Next.js frontend on http://localhost:3000 ..."
cd "$FRONTEND_DIR"
PATH="$NODE_BIN:$PATH" npm start -- --port 3000 &
FRONTEND_PID=$!

echo ""
echo "  GUI ready at:  http://localhost:3000"
echo "  Bridge API at: http://localhost:8000"
echo "  Press Ctrl+C to stop."
echo ""

wait "$BRIDGE_PID" "$FRONTEND_PID"
