const dgram = require('dgram');

const PACKET_SIZE = 20; // uint64 seq + uint32 queue_depth + uint64 process_time_ns

// Receives per-order UDP health samples from the engine during a single benchmark
// probe. It counts samples seen inside the measure window (engine throughput) and
// trips a health_breach when queue depth or per-order latency stays over the limit
// for `debounceConsecutive` samples in a row.
class TelemetryListener {
    constructor({ maxQueueDepth, maxProcessTimeNs, debounceConsecutive = 3 }) {
        this.maxQueueDepth = maxQueueDepth;
        this.maxProcessTimeNs = maxProcessTimeNs;
        this.debounceConsecutive = debounceConsecutive;
        this.socket = null;
        this.port = null;
        this.measuring = false;
        this.consecutiveBreaches = 0;
        this.failReason = null;
        this.sampleCount = 0;
    }

    async bind(port = 0) {
        this.socket = dgram.createSocket('udp4');
        await new Promise((resolve, reject) => {
            this.socket.once('error', reject);
            this.socket.bind(port, '0.0.0.0', () => {
                this.port = this.socket.address().port;
                resolve();
            });
        });
        this.socket.on('message', (msg) => this._onPacket(msg));
    }

    _onPacket(msg) {
        if (msg.length < PACKET_SIZE || !this.measuring)
            return;

        this.sampleCount += 1000;
        const queueDepth = msg.readUInt32LE(8);
        const processTimeNs = Number(msg.readBigUInt64LE(12));

        const breached =
            queueDepth > this.maxQueueDepth || processTimeNs > this.maxProcessTimeNs;

        if (!breached) {
            this.consecutiveBreaches = 0;
            return;
        }

        this.consecutiveBreaches += 1;
        if (this.consecutiveBreaches >= this.debounceConsecutive)
            this.failReason = 'health_breach';
    }

    startMeasureWindow() {
        this.measuring = true;
        this.consecutiveBreaches = 0;
        this.failReason = null;
        this.sampleCount = 0;
    }

    stopMeasureWindow() {
        this.measuring = false;
    }

    getSampleCount() {
        return this.sampleCount;
    }

    getFailReason() {
        return this.failReason;
    }

    close() {
        if (!this.socket)
            return;
        this.socket.close();
        this.socket = null;
    }
}

module.exports = { TelemetryListener };
