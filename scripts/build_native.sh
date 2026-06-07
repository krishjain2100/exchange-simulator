#!/bin/bash
# Build host-side native binaries used by the worker and bot fleet.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INCLUDE="-I$ROOT_DIR/core/include"
CXX_FLAGS="-std=c++17 -O2 -pthread"

echo "[Build] bot_fleet..."
g++ $CXX_FLAGS $INCLUDE "$ROOT_DIR/bot_node/bot_fleet.cpp" -o "$ROOT_DIR/bot_node/bot_fleet"

echo "[Build] bot_replay..."
g++ $CXX_FLAGS $INCLUDE "$ROOT_DIR/bot_node/bot_replay.cpp" -o "$ROOT_DIR/bot_node/bot_replay"

echo "[Build] generate_benchmark_load..."
g++ -std=c++17 -O2 $INCLUDE \
  "$ROOT_DIR/bot_node/generate_benchmark_load.cpp" \
  -o "$ROOT_DIR/bot_node/generate_benchmark_load"

if [ ! -f "$ROOT_DIR/bot_node/benchmark_load.bin" ] \
  || [ "$ROOT_DIR/bot_node/generate_benchmark_load.cpp" -nt "$ROOT_DIR/bot_node/benchmark_load.bin" ]; then
  echo "[Build] benchmark_load.bin (~5M orders)..."
  "$ROOT_DIR/bot_node/generate_benchmark_load" \
    "$ROOT_DIR/bot_node/benchmark_load.bin"
fi

echo "[Build] verifier..."
g++ $CXX_FLAGS $INCLUDE \
  "$ROOT_DIR/worker/verifier.cpp" \
  "$ROOT_DIR/core/src/Hash.cpp" \
  -o "$ROOT_DIR/worker/verifier"

echo "[Build] Native helpers ready."
