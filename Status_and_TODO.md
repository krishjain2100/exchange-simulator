# Exchange Simulator - Project Status, Design, and Roadmap

Last updated: 2026-05-25

## 1) What Exists Today

This repository currently implements a working single-node prototype of a hackathon exchange simulator with:

- Frontend (`frontend/`): React + Vite UI for code submission and live leaderboard.
- Backend API (`backend/server.js`): Express API for file upload and leaderboard retrieval using Redis as an in memory database.
- Worker (`backend/worker.js`): Job processor that compiles submitted code, runs load test, validates with golden model, and posts scores.
- Matching engine core (`core/` + root `MockEngine.cpp`): C++ exchange engine implementation + wrapper + bot fleet + Python golden validator.
- Redis: queue and leaderboard state.

## 2) Current End-to-End Flow

1. User uploads `.cpp` file from UI (`/api/submit`).
2. API stores file with Multer under `backend/uploads/` and pushes a JSON job to Redis list `hackathon:queue`.
3. Worker blocks on `BRPOP` and processes one job at a time:
   - compile: `Wrapper.cpp + uploaded file` into `run_env/job_<id>_engine`
   - run engine executable
   - when output includes "Awaiting Bot Fleet", launch `core/bot_fleet`
   - run `core/golden_model.py` against generated CSV artifacts
   - if success, read `latency.txt` and `ZADD` score into `leaderboard`
4. Frontend polls `/api/leaderboard` every 3 seconds and renders rank/team/latency.

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
- Sequential processing loop (`while true` + `await processJob`).
- Compiles user code with `g++` and wrapper.
- Executes engine, triggers bot fleet, runs correctness/latency validation.
- Writes leaderboard score to Redis.
- Cleans uploaded source and compiled executable in `finally`.

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
- Wrapper handles TCP ingestion, dispatching, ledger/trade/book CSV dump, and avg latency output.
- Golden model replays inputs and diffs trades/book state.


## 4) Todo

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

## Bot Fleet
- [ ] Do proper simulation of market traffic

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
