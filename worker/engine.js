const { spawn } = require('child_process');
const net = require('net');
const path = require('path');
const createLogger = require('../shared/logger');
const { ENGINE_READY_LOG_MARKER, ENGINE_READY_TIMEOUT_MS, ENGINE_READY_SETTLE_MS } = require('./config');

const log = createLogger('Worker');

function buildPoisonPillBuffer() {
    const buf = Buffer.alloc(36);
    buf.writeUInt8(9, 35);
    return buf;
}

function sendPoisonPill(port) {
    return new Promise((resolve) => {
        const client = new net.Socket();
        client.setTimeout(3000);
        client.connect(port, '127.0.0.1', () => {
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

function schedulePoisonPill(port, delayMs, reason) {
    return setTimeout(async () => {
        log.info(reason);
        const delivered = await sendPoisonPill(port);
        if (!delivered) {
            log.warn('[Worker] Poison pill could not be delivered (engine may already be shut down).');
        }
    }, delayMs);
}

function waitForEngineReady(dockerProcess, port, timeoutMs = ENGINE_READY_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
        let stdoutBuf = '';
        let settled = false;

        const timer = setTimeout(() => {
            if (settled) return;
            settled = true;
            reject(new Error(
                `Timed out waiting for engine ready signal on port ${port} (${timeoutMs}ms)`
            ));
        }, timeoutMs);

        const finish = (err) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            if (err) reject(err);
            else resolve();
        };

        const onStdout = (data) => {
            if (settled) return;

            stdoutBuf += data.toString();
            if (!stdoutBuf.includes(ENGINE_READY_LOG_MARKER)) return;

            setTimeout(() => finish(), ENGINE_READY_SETTLE_MS);
        };

        dockerProcess.stdout.on('data', onStdout);
    });
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
    waitForEngineReady,
    runChild,
};
