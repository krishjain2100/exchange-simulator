#!/bin/bash
# This script runs inside the isolated Docker container.
# The container expects the user's code to be mounted at /sandbox/submission.cpp
# and the trusted core mounted (or baked in) at /core.

set -euo pipefail

cd /sandbox

shopt -s nullglob
CORE_SOURCES=(/core/src/*.cpp)

if [ ${#CORE_SOURCES[@]} -eq 0 ]; then
    echo "[Docker ERROR] No core sources found under /core/src."
    exit 1
fi

if [ -x ./engine ]; then
    echo "[Docker] Using cached engine binary (skip compile)."
else
    echo "[Docker] Compiling C++ Engine..."
    if ! g++ -O3 -std=c++17 -I/core/include \
      "${CORE_SOURCES[@]}" \
      submission.cpp -o engine -lpthread 2>&1; then
      echo "[Docker ERROR] Compilation failed. Check g++ output above."
      exit 1
    fi
    echo "[Docker] Compilation successful."
fi

echo "[Docker] Booting Engine on Port 8080. Awaiting Distributed Swarm..."

./engine

EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
    echo "[Docker ERROR] The C++ Engine crashed with exit code $EXIT_CODE."
    exit $EXIT_CODE
fi

echo "[Docker] Engine shutdown cleanly. Binary state flushed to SSD."
exit 0
