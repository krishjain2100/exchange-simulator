const { spawn } = require('child_process');
const net = require('net');
const path = require('path');
const createLogger = require('../shared/logger');
const {
    ENGINE_READY_TIMEOUT_MS,
    ENGINE_READY_SETTLE_MS,
    PROBE_READY_LOG_MARKER,
    PROBE_READY_TIMEOUT_MS,
} = require('./config');

const log = createLogger('Worker');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function buildPoisonPillBuffer() {
    const buf = Buffer.alloc(32);
    buf.writeUInt8(9, 27);
    return buf;
}

function sendPoisonPill(port, host = process.env.ENGINE_HOST || '127.0.0.1') {
    return new Promise((resolve) => {
        const client = new net.Socket();
        client.setTimeout(3000);
        client.connect(port, host, () => {
            client.write(buildPoisonPillBuffer());
            client.end();
            resolve(true);
        });
        client.on('timeout', () => {
            client.destroy();
            resolve(false);
        });
        client.on('error', () => resolve(false));
    });
}

function schedulePoisonPill(port, delayMs, reason, host = process.env.ENGINE_HOST || '127.0.0.1') {
    return setTimeout(async () => {
        log.info(reason);
        const delivered = await sendPoisonPill(port, host);
        if (!delivered) {
            log.warn('[Worker] Poison pill could not be delivered (engine may already be shut down).');
        }
    }, delayMs);
}

function attachStdoutBuffer(dockerProcess) {
    if (dockerProcess._stdoutBuf)
        return dockerProcess._stdoutBuf;

    dockerProcess._stdoutBuf = { text: '', waiters: [] };
    dockerProcess.stdout.on('data', (data) => {
        const chunk = data.toString();
        const buf = dockerProcess._stdoutBuf;
        buf.text += chunk;
        const pending = buf.waiters.splice(0);
        for (const waiter of pending) {
            if (!waiter.check()) {
                buf.waiters.push(waiter);
            }
        }
    });

    return dockerProcess._stdoutBuf;
}

function waitForNewLogMarker(dockerProcess, marker, afterIndex, timeoutMs) {
    attachStdoutBuffer(dockerProcess);
    return new Promise((resolve, reject) => {
        let settled = false;

        const finish = (err) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            reject(err);
        };

        const check = () => {
            const buf = dockerProcess._stdoutBuf;
            if (buf.text.indexOf(marker, afterIndex) !== -1) {
                if (settled) return true;
                settled = true;
                clearTimeout(timer);
                resolve(buf.text.length);
                return true;
            }
            return false;
        };

        const timer = setTimeout(() => {
            finish(new Error(`Timed out waiting for log marker "${marker}" (${timeoutMs}ms)`));
        }, timeoutMs);

        if (check())
            return;

        dockerProcess._stdoutBuf.waiters.push({ check });
        // Data may have arrived between the check above and registering the waiter.
        check();
    });
}

function canConnect(port, host = process.env.ENGINE_HOST || '127.0.0.1') {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        socket.setTimeout(500);
        socket.once('connect', () => {
            socket.destroy();
            resolve(true);
        });
        socket.once('timeout', () => {
            socket.destroy();
            resolve(false);
        });
        socket.once('error', () => resolve(false));
        socket.connect(port, host);
    });
}

// Poll until the published Docker port accepts TCP — avoids stdout buffering races.
async function waitForEngineReady(dockerProcess, port, { timeoutMs = ENGINE_READY_TIMEOUT_MS, host = process.env.ENGINE_HOST || '127.0.0.1' } = {}) {
    if (dockerProcess) {
        const startLen = dockerProcess._stdoutBuf ? dockerProcess._stdoutBuf.text.length : 0;
        await waitForNewLogMarker(
            dockerProcess,
            "Listening on Port 8080",
            startLen,
            timeoutMs,
        ).catch((err) => {
            console.warn(`[Worker] Engine startup log marker check failed: ${err.message}. Proceeding to TCP check.`);
        });
    }

    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (await canConnect(port, host)) {
            await sleep(ENGINE_READY_SETTLE_MS);
            return;
        }
        await sleep(100);
    }
    throw new Error(`Timed out waiting for engine on port ${port} (${timeoutMs}ms)`);
}

// After a poison pill, wait for probe metrics flush then the next listen cycle.
async function waitForNextProbe(dockerProcess, port, host = process.env.ENGINE_HOST || '127.0.0.1', fromIndex) {
    attachStdoutBuffer(dockerProcess);
    const startLen = fromIndex !== undefined ? fromIndex : dockerProcess._stdoutBuf.text.length;
    await waitForNewLogMarker(
        dockerProcess,
        PROBE_READY_LOG_MARKER,
        startLen,
        PROBE_READY_TIMEOUT_MS,
    );
    await waitForEngineReady(dockerProcess, port, { host });
}

function runChild(binary, args, { onStdout, onStderrLine } = {}) {
    return new Promise((resolve, reject) => {
        const child = spawn(binary, args);
        let stderr = '';

        child.stdout.on('data', (data) => {
            const text = data.toString();
            if (onStdout) onStdout(text);
            else process.stdout.write(text);
        });
        child.stderr.on('data', (data) => {
            stderr += data.toString();
            if (onStderrLine) onStderrLine(data.toString());
            else process.stderr.write(data.toString());
        });
        child.on('error', reject);
        child.on('close', (code) => {
            if (code === 0) return resolve();
            const detail = stderr.trim().split('\n').filter(Boolean).pop();
            reject(new Error(detail || `${path.basename(binary)} exited with code ${code}`));
        });
    });
}

module.exports = {
    sendPoisonPill,
    schedulePoisonPill,
    attachStdoutBuffer,
    waitForEngineReady,
    waitForNextProbe,
    runChild,
};
