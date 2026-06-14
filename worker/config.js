const path = require('path');
const benchmark = require('../shared/benchmarkConfig');

const CORE_DIR = path.resolve(__dirname, '../core');
const WORK_DIR = path.resolve(__dirname, '../run_env');

module.exports = {
    ...benchmark,
    CORE_DIR,
    WORK_DIR,
    BOT_REPLAY_BINARY: path.join(__dirname, 'bot_replay'),
    VERIFIER_BINARY: path.resolve(__dirname, 'verifier'),
    BUCKET_NAME: process.env.S3_BUCKET_NAME || 'my-hackathon-submissions',
    PROBE_READY_LOG_MARKER: '[Wrapper] Probe ready',
    ENGINE_READY_TIMEOUT_MS: 30000,
    PROBE_READY_TIMEOUT_MS: 30000,
    ENGINE_READY_SETTLE_MS: 250,
    DOCKER_HARD_LIMIT_MS: 600000,
    DOCKER_MAX_MEMORY_MB: 4096,
    DOCKER_MAX_CPUS: 3,
    PHASE1_TIMEOUT_MS: 20000,
    PHASE1_FORCE_STOP_MS: 30000,
    PHASE1_LOAD_FAILURE_MSG: 'Engine could not finish Phase 1 within 30s',
};
