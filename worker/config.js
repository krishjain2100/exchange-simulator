const path = require('path');

const CORE_DIR = path.resolve(__dirname, '../core');
const BOT_NODE_DIR = path.resolve(__dirname, '../bot_node');
const WORK_DIR = path.resolve(__dirname, '../run_env');
const PHASE1_FORCE_STOP_MS = 150000;
const PHASE2_FORCE_STOP_MS = 135000;
module.exports = {
    CORE_DIR,
    BOT_NODE_DIR,
    WORK_DIR,
    BENCHMARK_LOAD_PATH: path.join(BOT_NODE_DIR, 'benchmark_load.bin'),
    BOT_REPLAY_BINARY: path.join(BOT_NODE_DIR, 'bot_replay'),
    VERIFIER_BINARY: path.resolve(__dirname, 'verifier'),
    BUCKET_NAME: process.env.S3_BUCKET_NAME || 'my-hackathon-submissions',
    ENGINE_READY_LOG_MARKER: '[Wrapper] Listening on Port',
    ENGINE_READY_TIMEOUT_MS: 30000,
    ENGINE_READY_SETTLE_MS: 250,
    DOCKER_HARD_LIMIT_MS: 180000,
    DOCKER_MAX_MEMORY_MB: 4096,
    DOCKER_MAX_CPUS: 4,
    MAX_PHASE1_SHARDS: 32,
    SCALE_INTERVAL_MS: 3000,
    PHASE1_TIMEOUT_MS: 120000,
    PHASE2_POISON_MS: 120000,
    PHASE1_LOAD_FAILURE_MSG: `[Phase 1] Engine could not handle the load (did not finish Phase 1 within ${PHASE1_FORCE_STOP_MS / 1000}s)`,
    PHASE2_LOAD_FAILURE_MSG: `[Phase 2] Engine could not handle the load (did not finish within ${PHASE2_FORCE_STOP_MS / 1000}s after delivery)`,
};
