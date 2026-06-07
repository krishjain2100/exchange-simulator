const ip = require('ip');
const createLogger = require('../shared/logger');
const { runChild, schedulePoisonPill, sendPoisonPill } = require('./engine');
const {BENCHMARK_LOAD_PATH, BOT_REPLAY_BINARY, MAX_PHASE1_SHARDS, SCALE_INTERVAL_MS, PHASE1_TIMEOUT_MS, PHASE1_FORCE_STOP_MS, PHASE2_POISON_MS, PHASE2_FORCE_STOP_MS} = require('./config');

const log = createLogger('Worker');

async function startPhase1Load(workerClient, job_id, port, { onForceStop } = {}) {
    const testStartsAt = Date.now();
    const targetIp = ip.address();
    let dispatchedShards = 0;
    let scaleLoop;

    log.info(`[Worker] Phase 1: scaling up to ${MAX_PHASE1_SHARDS} bot_fleet shards against ${targetIp}:${port}...`);

    const dispatchNextShard = async () => {
        if (dispatchedShards >= MAX_PHASE1_SHARDS) {
            clearInterval(scaleLoop);
            return;
        }

        const shardId = dispatchedShards++;
        log.info(`[Worker] Phase 1: dispatching bot_fleet shard ${shardId}...`);

        await workerClient.lPush('hackathon:bot_queue', JSON.stringify({
            job_id,
            phase: 'correctness',
            target_ip: targetIp,
            target_port: port,
            bot_group_id: shardId,
            test_starts_at: testStartsAt,
        }));
    };

    scaleLoop = setInterval(() => {
        dispatchNextShard().catch((err) => { log.error(`[Worker] Phase 1 shard dispatch failed: ${err.message}`); });
    }, SCALE_INTERVAL_MS);

    dispatchNextShard().catch((err) => { log.error(`[Worker] Phase 1 shard dispatch failed: ${err.message}`); });

    const poisonNet = schedulePoisonPill(
        port,
        PHASE1_TIMEOUT_MS,
        `[Worker] Phase 1 drain timeout (${PHASE1_TIMEOUT_MS / 1000}s). Delivering poison pill...`,
    );

    const forceStopNet = setTimeout(() => {
        log.info(
            `[Worker] Phase 1 force stop (${PHASE1_FORCE_STOP_MS / 1000}s). Tearing down container...`
        );
        onForceStop?.();
    }, PHASE1_FORCE_STOP_MS);

    return { scaleLoop, poisonNet, forceStopNet };
}

async function startPhase2Load(port, { onForceStop } = {}) {
    log.info(`[Worker] Phase 2 (benchmark): engine ready on port ${port}. Replaying fixed load...`);

    try {
        log.info(`[Worker] Launching benchmark replay on 127.0.0.1:${port}...`);
        await runChild(BOT_REPLAY_BINARY, ['127.0.0.1', port.toString(), BENCHMARK_LOAD_PATH]);
    } catch (replayErr) {
        log.error(`[Worker] Phase 2 replay failed: ${replayErr.message}. Sending poison pill...`);
        await sendPoisonPill(port);
        throw replayErr;
    }

    const poisonNet = schedulePoisonPill(
        port,
        PHASE2_POISON_MS,
        `[Worker] Phase 2 drain timeout (${PHASE2_POISON_MS / 1000}s after load delivery). Delivering poison pill...`,
    );

    const forceStopNet = setTimeout(() => {
        log.info(
            `[Worker] Phase 2 force stop (${PHASE2_FORCE_STOP_MS / 1000}s after load delivery). Tearing down container...`
        );
        onForceStop?.();
    }, PHASE2_FORCE_STOP_MS);

    return { poisonNet, forceStopNet };
}

function stopLoadTimers(timers) {
    if (!timers) return;
    if (timers.scaleLoop) clearInterval(timers.scaleLoop);
    if (timers.poisonNet) clearTimeout(timers.poisonNet);
    if (timers.forceStopNet) clearTimeout(timers.forceStopNet);
}

module.exports = {
    startPhase1Load,
    startPhase2Load,
    stopLoadTimers,
};
