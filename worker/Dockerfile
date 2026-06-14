FROM node:20-bullseye

# Install Docker CLI
RUN apt-get update && \
    apt-get install -y ca-certificates curl gnupg && \
    install -m 0755 -d /etc/apt/keyrings && \
    curl -fsSL https://download.docker.com/linux/debian/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg && \
    chmod a+r /etc/apt/keyrings/docker.gpg && \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/debian bullseye stable" > /etc/apt/sources.list.d/docker.list && \
    apt-get update && \
    apt-get install -y docker-ce-cli && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/worker

# Cache NPM install
COPY worker/package*.json ./
RUN npm install

# Copy source and compile natively inside the image
COPY . /workspace
RUN g++ -std=c++17 -O2 -pthread -I/workspace/core/include bot_replay.cpp -o /usr/local/bin/bot_replay && \
    g++ -std=c++17 -O2 -pthread -I/workspace/core/include verifier.cpp /workspace/core/src/Hash.cpp -o /usr/local/bin/verifier

CMD ["node", "worker.js"]
