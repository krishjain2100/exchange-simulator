# Use  Linux image with a C++ compiler
FROM alpine:latest

# This pulls a bare-bones version of Linux called Alpine. 
# It is incredibly small (about 5MB), sandbox will boot up in milliseconds rather than seconds.

# Install g++ and standard libraries
RUN apk add --no-cache g++ make bash
# apk is Alpine's package manager (like brew for Mac or apt for Ubuntu)
# --no-cache flag: instruction to download the software, install it, and completely delete all the indexes and temporary files used to get it.

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
# The above line compiles the bot_fleet.cpp file into an executable called bot_fleet for linux.

# Ensure the executables can be run
RUN chmod +x /core/bot_fleet
RUN chmod +x /core/runner.sh
# By default, copied files are just readable text. 
# This command changes their permissions to make them "eXecutable" programs

# The container will start by running our script
ENTRYPOINT ["/core/runner.sh"]