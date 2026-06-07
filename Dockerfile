# Use a lightweight Linux image with a C++ compiler
FROM alpine:latest

# Install g++ and standard libraries (apk is Alpine's package manager)
# --no-cache keeps the image incredibly small by deleting installation temp files
RUN apk add --no-cache g++ make bash

# Trusted core infrastructure — overridden at runtime by worker.js via:
#   -v <repo>/core:/core:ro
WORKDIR /core
COPY core/ /core/
RUN chmod +x /core/runner.sh

# CRITICAL: Switch the working directory to where the participant's code will be injected
# When the worker.js runs `docker run -v ./run_env/job_123:/sandbox`, this is where it lands.
WORKDIR /sandbox

# The container will start by executing our runner script
ENTRYPOINT ["/core/runner.sh"]
