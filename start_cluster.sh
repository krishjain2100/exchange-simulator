#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

# This function guarantees that when you press Ctrl+C, it kills all 
# background Node processes and shuts down the Redis container gracefully.
cleanup() {
    echo -e "\n[Cluster] Shutting down Local Infrastructure..."
    echo "[Cluster] Stopping Redis..."
    docker stop hft-redis > /dev/null 2>&1
    
    echo "[Cluster] Killing Node Microservices..."
    # 2>/dev/null hides the termination warnings from the terminal
    kill ${BACKEND_PID:-} ${FRONTEND_PID:-} ${WORKER_PID:-} ${BOT_PID:-} 2>/dev/null || true
    
    echo "[Cluster] Offline."
    exit 0
}

# Bind the cleanup function to SIGINT (Ctrl+C) and SIGTERM
trap cleanup SIGINT SIGTERM

ensure_docker_running() {
    if docker info > /dev/null 2>&1; then
        return 0
    fi

    echo "[Cluster] Docker daemon is not running. Launching Docker Desktop..."
    open -a Docker >/dev/null 2>&1 || true

    local waited=0
    local timeout=90
    while ! docker info > /dev/null 2>&1; do
        if [ "$waited" -ge "$timeout" ]; then
            echo "[Cluster ERROR] Docker daemon is still unavailable after ${timeout}s. Start Docker Desktop and rerun."
            exit 1
        fi
        sleep 2
        waited=$((waited + 2))
    done
}

echo "======================================="
echo "   Hackathon Local Testbed    "
echo "======================================="

ensure_docker_running

echo "[Cluster] Building sandbox image and native helpers..."
docker build -t hft-sandbox "$ROOT_DIR" > /dev/null
chmod +x "$ROOT_DIR/scripts/build_native.sh"
"$ROOT_DIR/scripts/build_native.sh"

# ==========================================
# 2. Boot Redis (The Message Broker)
# ==========================================
echo "[Cluster] 1/5 Booting Redis via Docker..."
# Remove any stale Redis container from a previous run so reruns are idempotent.
docker rm -f hft-redis > /dev/null 2>&1 || true
# --rm removes the container when stopped, --name allows us to target it in cleanup
docker run --rm -p 6379:6379 -d --name hft-redis redis > /dev/null 2>&1
sleep 2 # Give Redis exactly 2 seconds to bind to port 6379

# ==========================================
# 3. Boot the Gateway Backend (Express API)
# ==========================================
echo "[Cluster] 2/5 Starting Gateway API..."
cd "$ROOT_DIR/gateway/backend"
node server.js &
BACKEND_PID=$! # Capture the Process ID of the backend
cd "$ROOT_DIR"

# ==========================================
# 4. Boot the Fat Worker (Sandbox Manager)
# ==========================================
echo "[Cluster] 3/5 Starting Sandbox Worker..."
cd "$ROOT_DIR/worker"
node worker.js &
WORKER_PID=$!
cd "$ROOT_DIR"

# ==========================================
# 5. Boot the Bot Node (The Swarm)
# ==========================================
echo "[Cluster] 4/5 Starting Bot Fleet Orchestrator..."
cd "$ROOT_DIR/bot_node"
node bot_worker.js &
BOT_PID=$!
cd "$ROOT_DIR"

# ==========================================
# 6. Boot the Gateway Frontend (React)
# ==========================================
echo "[Cluster] 5/5 Starting React Frontend..."
cd "$ROOT_DIR/gateway/frontend"
# Assuming you use Vite (npm run dev). If using Create React App, change to npm start.
npm run dev & 
FRONTEND_PID=$!
cd "$ROOT_DIR"

echo "======================================================="
echo "[Cluster] ALL SYSTEMS ONLINE."
echo "[Cluster] Frontend running on http://localhost:5173"
echo "[Cluster] Backend API running on http://localhost:3000"
echo "[Cluster] Redis listening on Port 6379"
echo " "
echo "[Cluster] Watching logs... Press Ctrl+C to shut everything down."
echo "======================================================="

# The wait command blocks the script from exiting, keeping the terminal open 
# so you can see the combined `console.log` output from all your services.
wait
