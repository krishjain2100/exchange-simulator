#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INCLUDE="-I$ROOT_DIR/core/include"
CXX_FLAGS="-std=c++17 -O2 -pthread"
LEVELS_DIR="$ROOT_DIR/worker/levels"

echo "[Build] bot_fleet..."
g++ $CXX_FLAGS $INCLUDE "$ROOT_DIR/bot_node/bot_fleet.cpp" -o "$ROOT_DIR/bot_node/bot_fleet"

echo "[Build] generate_nasdaq_level (build-time level generator)..."
g++ $CXX_FLAGS $INCLUDE \
  "$ROOT_DIR/scripts/generate_nasdaq_level.cpp" \
  -o "$ROOT_DIR/scripts/generate_nasdaq_level"

echo "[Build] bot_replay..."
g++ $CXX_FLAGS $INCLUDE "$ROOT_DIR/worker/bot_replay.cpp" -o "$ROOT_DIR/worker/bot_replay"

mkdir -p "$LEVELS_DIR"

if ! node "$ROOT_DIR/scripts/build_benchmark_levels.js" --check; then
  echo "[Build] Generating missing NASDAQ benchmark levels (existing .bin files are reused)..."
  node "$ROOT_DIR/scripts/build_benchmark_levels.js"
else
  echo "[Build] NASDAQ benchmark levels OK"
fi

echo "[Build] verifier..."
g++ $CXX_FLAGS $INCLUDE \
  "$ROOT_DIR/worker/verifier.cpp" \
  "$ROOT_DIR/core/src/Hash.cpp" \
  -o "$ROOT_DIR/worker/verifier"

echo "[Build] done."
