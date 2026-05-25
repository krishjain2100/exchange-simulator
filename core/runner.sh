#!/bin/bash
# This script runs inside the isolated Docker container.
# The container expects the user's code to be mounted at /sandbox/submission.cpp

cd /sandbox

echo "[Docker] Compiling code..."
# Compile the core wrapper with the user's submission
g++ -O3 -std=c++17 -I/core /core/Wrapper.cpp submission.cpp -o engine

# $? always holds the exit code of the last command that ran
if [ $? -ne 0 ]; then
    echo "[Docker] Compilation failed."
    exit 1
fi

echo "[Docker] Starting Engine..."
./engine & ENGINE_PID=$!
#$! is a variable that grabs the Process ID (PID) of the very last background job.
# Give the engine 1 second to initialize its memory and open Port 8080
sleep 1

echo "[Docker] Firing Bot Fleet..."
/core/bot_fleet

# Wait for the background engine process to finish dumping its CSVs and exit
wait $ENGINE_PID

if [ $? -ne 0 ]; then
    echo "[Docker ERROR] The C++ Engine crashed with exit code $?."
    exit 1 
fi

echo "[Docker] Execution complete."
exit 0