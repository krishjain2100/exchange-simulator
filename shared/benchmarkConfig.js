const path = require('path');

const BENCHMARK_BASELINE_OPS = 20000;
const BENCHMARK_COARSE_LEVELS = ['10', '20', '30', '40', '50', '60', '80', '100', '120', '140', '160', '180', '200', '220', '240', '260', '280', '300'];
const BENCHMARK_FINE_LEVELS = [];
const BENCHMARK_LEVELS_DIR = path.join(__dirname, '../worker/levels');
const BENCHMARK_LEVEL_SETTLE_MS = 6000;
const BENCHMARK_LEVEL_MEASURE_MS = 4000;
const BENCHMARK_LEVEL_TOTAL_MS = BENCHMARK_LEVEL_SETTLE_MS + BENCHMARK_LEVEL_MEASURE_MS;
const BENCHMARK_BORDERLINE_RETRIES = 3;
const BENCHMARK_FAILURE_QUEUE_DEPTH = 10000;
const BENCHMARK_FAILURE_PROCESS_TIME_NS = 100000; // 100 microseconds SLA
const BENCHMARK_FAILURE_DEBOUNCE = 3;

// Engine must process at least this fraction of target ops during the measure window.
const BENCHMARK_MIN_THROUGHPUT_FRACTION = 0.75;

function targetOpsForMultiplier(mult) {
    return Math.round(BENCHMARK_BASELINE_OPS * Number(mult));
}

function levelFilename(mult) {
    return `nasdaq_${mult}x.bin`;
}

function levelFilePath(mult) {
    return path.join(BENCHMARK_LEVELS_DIR, levelFilename(mult));
}

module.exports = {
    BENCHMARK_BASELINE_OPS,
    BENCHMARK_COARSE_LEVELS,
    BENCHMARK_FINE_LEVELS,
    BENCHMARK_LEVELS_DIR,
    BENCHMARK_LEVEL_SETTLE_MS,
    BENCHMARK_LEVEL_MEASURE_MS,
    BENCHMARK_LEVEL_TOTAL_MS,
    BENCHMARK_BORDERLINE_RETRIES,
    BENCHMARK_FAILURE_QUEUE_DEPTH,
    BENCHMARK_FAILURE_PROCESS_TIME_NS,
    BENCHMARK_FAILURE_DEBOUNCE,
    BENCHMARK_MIN_THROUGHPUT_FRACTION,
    targetOpsForMultiplier,
    levelFilename,
    levelFilePath,
};
