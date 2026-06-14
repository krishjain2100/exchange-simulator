const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const createLogger = require('../shared/logger');
const { sendPoisonPill } = require('./engine');
const {
    BENCHMARK_LEVEL_SETTLE_MS,
    BENCHMARK_LEVEL_MEASURE_MS,
    BENCHMARK_LEVEL_TOTAL_MS,
    BENCHMARK_MIN_THROUGHPUT_FRACTION,
    BOT_REPLAY_BINARY,
    levelFilePath,
    targetOpsForMultiplier,
} = require('./config');
const { parseEngineCsv } = require('../shared/jobMetrics');

const log = createLogger('Worker');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const PHASE2_METRICS_FILE = 'phase2_metrics.txt';

function expectedOpsForTarget(targetOps, windowMs) {
    return Math.floor(
        targetOps * (windowMs / 1000) * BENCHMARK_MIN_THROUGHPUT_FRACTION,
    );
}

function runReplay(host, port, filePath, targetOps) {
    return new Promise((resolve, reject) => {
        const child = spawn(BOT_REPLAY_BINARY, [
            host, String(port), filePath,
            '--paced', String(targetOps), String(BENCHMARK_LEVEL_TOTAL_MS),
        ]);
        let stdout = '';
        let stderr = '';
        child.stdout.on('data', (d) => { stdout += d.toString(); });
        child.stderr.on('data', (d) => { stderr += d.toString(); });
        child.on('error', reject);
        child.on('close', (code) => {
            if (code === 0) {
                return resolve({ stdout, stderr });
            }
            reject(new Error(stderr.trim() || stdout.trim() || `bot_replay exited ${code}`));
        });
    });
}

function evaluateProbe({ engineMetrics, replayError }) {
    if (engineMetrics && engineMetrics.shutdown_reason === 'health_breach') {
        return { failed: true, fail_reason: 'health_breach' };
    }
    if (replayError) {
        return { failed: true, fail_reason: 'replay_error' };
    }
    if (!engineMetrics) {
        return { failed: true, fail_reason: 'no_metrics' };
    }
    return { failed: false, fail_reason: null };
}

async function executeBenchmarkProbe({ port, jobDir, multiplier, waitForNextProbe }) {
    const key = String(multiplier);
    const filePath = levelFilePath(key);
    const target_ops = targetOpsForMultiplier(key);

    let replayError = null;

    const replayPromise = runReplay(
        '127.0.0.1', port, filePath, target_ops,
    ).catch((err) => {
        replayError = err;
    });

    await sleep(BENCHMARK_LEVEL_SETTLE_MS);
    await sleep(BENCHMARK_LEVEL_MEASURE_MS);
    await replayPromise;

    const delivered = await sendPoisonPill(port);
    let engineMetrics = null;

    if (!delivered) {
        await sleep(500);
        const probeMetricsPath = path.join(jobDir, PHASE2_METRICS_FILE);
        engineMetrics = fs.existsSync(probeMetricsPath)
            ? parseEngineCsv(fs.readFileSync(probeMetricsPath, 'utf8'), true)
            : null;

        if (engineMetrics && engineMetrics.shutdown_reason === 'health_breach') {
            log.info(`[Worker] Engine shut down due to health breach at ${multiplier}x. Poison pill delivery skipped.`);
        } else {
            throw new Error('Poison pill could not be delivered');
        }
    } else {
        if (waitForNextProbe) {
            await waitForNextProbe();
        }
        const probeMetricsPath = path.join(jobDir, PHASE2_METRICS_FILE);
        engineMetrics = fs.existsSync(probeMetricsPath)
            ? parseEngineCsv(fs.readFileSync(probeMetricsPath, 'utf8'), true)
            : null;
    }

    if (replayError) {
        log.warn(`[Worker] bot_replay error at ${multiplier}x: ${replayError.message}`);
    }

    const verdict = evaluateProbe({
        engineMetrics,
        replayError,
    });

    const engineOps = engineMetrics ? engineMetrics.orders_processed : 0;

    return {
        multiplier: key,
        peak_multiplier: Number(key),
        target_ops,
        peak_target_ops: target_ops,
        engine_ops: engineOps,
        failed: verdict.failed,
        fail_reason: verdict.fail_reason,
        ...(engineMetrics ?? {
            orders_processed: 0,
            trades_executed: 0,
            processing_duration_ns: 0,
            max_ops: 0,
            shutdown_reason: 'unknown',
            p50: 0,
            p90: 0,
            p99: 0,
        }),
    };
}

module.exports = { executeBenchmarkProbe };
