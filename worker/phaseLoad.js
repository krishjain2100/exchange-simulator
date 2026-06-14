const ip = require('ip');
const createLogger = require('../shared/logger');
const { schedulePoisonPill } = require('./engine');
const { PHASE1_TIMEOUT_MS, PHASE1_FORCE_STOP_MS } = require('./config');

const log = createLogger('Worker');

async function startPhase1Load(workerClient, job_id, port, { host, onForceStop } = {}) {
    log.info(`[Worker] Phase 1 (correctness) → ${host || ip.address()}:${port}`);

    await workerClient.lPush('hackathon:bot_queue', JSON.stringify({
        job_id,
        target_ip: host || process.env.ENGINE_HOST || ip.address(),
        target_port: port,
    }));

    const poisonNet = schedulePoisonPill(
        port,
        PHASE1_TIMEOUT_MS,
        `[Worker] Phase 1 poison pill (${PHASE1_TIMEOUT_MS / 1000}s)`,
        host,
    );

    const forceStopNet = setTimeout(() => {
        log.info(`[Worker] Phase 1 force stop (${PHASE1_FORCE_STOP_MS / 1000}s)`);
        onForceStop();
    }, PHASE1_FORCE_STOP_MS);

    return { poisonNet, forceStopNet };
}

function stopLoadTimers(timers) {
    if (!timers) return;
    if (timers.poisonNet) clearTimeout(timers.poisonNet);
    if (timers.forceStopNet) clearTimeout(timers.forceStopNet);
}

module.exports = { startPhase1Load, stopLoadTimers };
