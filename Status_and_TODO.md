# Exchange Simulator - Project Status, Design, and Roadmap

Last updated: 2026-05-29

## 1) What Exists Today

This repository currently implements a working single-node prototype of a hackathon exchange simulator with:

- Frontend (`frontend/`): React + Vite UI for code submission and live leaderboard.
- Backend API (`backend/server.js`): Express API for file upload and leaderboard retrieval using Redis as an in memory database.
- Worker (`backend/worker.js`): Job processor that compiles submitted code, runs load test, validates with golden model, and posts scores.
- Matching engine core (`core/` + root `MockEngine.cpp`): C++ exchange engine implementation + wrapper + bot fleet + Python golden validator.
- Redis: queue and leaderboard state.

## 2) Current End-to-End Flow

1. User uploads a `.cpp` file from the UI (`/api/submit`).
2. API stores the uploaded file with Multer under `backend/uploads/` and pushes a JSON job to the Redis list `hackathon:queue`.
3. A worker process blocks on `BRPOP` and processes jobs sequentially:
  - the worker moves the uploaded file into a per-job sandbox (`run_env/<job_id>`) and sets job status `compiling` in Redis.
  - the worker runs a Docker sandbox (`hft-sandbox`) to compile and exercise the submission inside an isolated container. As soon as the Docker container starts the worker marks the job `running` in Redis and records `started_at`.
  - the worker executes the compiled engine binary (the wrapper) and waits for the engine to print the readiness marker ("Awaiting Bot Fleet"). When that appears the worker launches `core/bot_fleet` to drive the load against the engine.
  - after the bots complete, the worker runs the local `core/verifier` binary on the host to verify correctness of generated CSV artifacts.
  - if verification passes, the worker reads `latency.txt` produced by the sandbox, computes the composite score, `ZADD`s the score into the `leaderboard`, and writes `duration_ms` and per-team metrics into Redis.
  - the worker now avoids overwriting an existing terminal job `status`/`error` in Redis; `duration_ms` is always persisted.
4. Frontend polls `/api/leaderboard` (every 3s) and renders rank/team/latency.

Notes:
- Redis is expected at `127.0.0.1:6379` by default.
- Docker must be available on the host to run the `hft-sandbox` container used for compilation and load testing.
- The `core/verifier` binary runs on the host (not inside the container) and must be present and executable.
- `core/bot_fleet` now tracks and reports a thread-safe network orders counter at shutdown to aid debugging.

## 3) Component Design Summary

### Frontend

Files:
- `frontend/src/App.jsx`
- `frontend/src/App.css`
- `frontend/vite.config.js`

Behavior:
- Submission form with team name + `.cpp` file.
- Polling leaderboard every 3 seconds.
- Scrollable leaderboard body has been implemented (`.leaderboardBody`).

### Backend API

Files:
- `backend/server.js`

Endpoints:
- `POST /api/submit`: enqueue compile/test jobs.
- `GET /api/leaderboard`: reads Redis sorted set via `zRangeWithScores`.

### Worker

Files:
- `backend/worker.js`

Behavior:
- Sequential processing loop (`while true` + `await processJob`) that consumes jobs from Redis `hackathon:queue`.
- Moves uploads into a per-job sandbox (`run_env/<job_id>`), sets `compiling` in Redis, then runs the `hft-sandbox` Docker container to compile and exercise the submission.
- Marks the job `running` in Redis as soon as the Docker container starts and records `started_at`.
- Runs the compiled engine (wrapper) and waits for the wrapper's readiness marker ("Awaiting Bot Fleet"); launches `core/bot_fleet` to drive load against the engine when ready.
- Runs the host `core/verifier` binary to validate generated CSV artifacts.
- On success reads `latency.txt`, computes a composite score, `ZADD`s into `leaderboard`, and writes `duration_ms` plus per-team metrics into Redis.
- Cleans uploaded source and compiled executable in `finally` and logs job duration/metrics.

### Core Engine and Validator

Files:
- `MockEngine.cpp`
- `core/Wrapper.cpp`
- `core/ExchangeEngine.h`
- `core/Order.h`
- `core/Telemetry.h`
- `core/bot_fleet.cpp`
- `core/golden_model.py`

Behavior:
- Limit/market/cancel processing for buy/sell books.
- Trade reporting through `Telemetry::ReportTrade`.
- `core/Wrapper.cpp` handles compilation wrapper behavior, TCP ingestion for orders, readiness marker emission ("Awaiting Bot Fleet"), and CSV dumps used by the verifier.
- `core/bot_fleet.cpp` simulates multiple bots, sends orders/cancels to the engine, and now tracks a thread-safe network orders counter reported at shutdown to help debug load behavior.
- `core/verifier` replay inputs and diff trades/book state to validate correctness.


## 4) Todo


## Verification

- [ ] Verify latencies for different engines and verify cpu pining

## Stabilize Core Pipeline

- [ ] Add API validation for missing file/team name and return clear 4xx responses.
- [ ] Add Multer file size/type limits and reject non-`.cpp` uploads.
- [ ] Preserve best latency per team (update only if new score is lower).

## Reliability and Operations

- [ ] Introduce processing queue semantics with retries + dead-letter queue.
  - Option A: list + processing list (`BRPOPLPUSH`) + retry count.
  - Option B: Redis Streams with consumer groups.
- [ ] Add graceful shutdown for worker and API.
- [ ] Add health endpoints (`/healthz`) and readiness checks.

## Scale
- [ ] Add rate limiting and request throttling.
- [ ] Verifier's binary is being used, so sometimes I forget to compile

## Wrapper
- [ ] Add TPS, don't know how to calculate

## UX and Developer Experience

- [ ] Replace polling with SSE/WebSocket for live updates
- [ ] Add complete runbook docs and architecture diagram in README.

## 5) Commands

Backend API:

```bash
cd backend
node server.js
```

Worker:

```bash
cd backend
node worker.js
```

Frontend:

```bash
cd frontend
npm run dev
```

Redis:

```bash
redis-server --port 6379
```

---
