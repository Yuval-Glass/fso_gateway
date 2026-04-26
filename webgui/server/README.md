# FSO Gateway Bridge

Python FastAPI server that bridges the Next.js dashboard to the FSO Gateway C daemon.

## Status

- **Phase 2A (current):** serves mock telemetry over WebSocket — no dependency on the C gateway running.
- **Phase 2B (planned):** connects to the C-side `control_server` UNIX socket and streams real stats from a running `fso_gw_runner`.

## Run

```bash
cd webgui/server
uv run uvicorn main:app --reload --port 8000
```

## Endpoints

| Path         | Protocol  | Purpose                                            |
| ------------ | --------- | -------------------------------------------------- |
| `/health`    | GET       | Bridge health + current data source (`mock` or `gateway`) |
| `/ws/live`   | WebSocket | Live telemetry snapshots at 1 Hz                   |

## Wire format

Every WS message is a full `TelemetrySnapshot` (see `webgui/src/types/telemetry.ts`), plus:
- `source`: `"mock"` | `"gateway"`
- `generatedAt`: epoch ms
