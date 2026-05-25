# Use  Linux image with a C++ compiler
FROM alpine:latest

# Install g++ and standard libraries
RUN apk add --no-cache g++ make bash

# Create a directory for  core files
WORKDIR /core

# Copy files from your Mac into the Docker image
COPY core/Wrapper.cpp /core/Wrapper.cpp
COPY core/ExchangeEngine.h /core/ExchangeEngine.h
COPY core/Order.h /core/Order.h
COPY core/Telemetry.h /core/Telemetry.h
COPY core/runner.sh /core/runner.sh

COPY core/bot_fleet.cpp /core/bot_fleet.cpp
RUN g++ -O3 -std=c++17 /core/bot_fleet.cpp -o /core/bot_fleet

# Ensure the executables can be run
RUN chmod +x /core/bot_fleet
RUN chmod +x /core/runner.sh

# The container will start by running our script
ENTRYPOINT ["/core/runner.sh"]