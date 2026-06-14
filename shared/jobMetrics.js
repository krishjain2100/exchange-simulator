const fs = require('fs');
const path = require('path');

const SLA_THRESHOLD_NS = 1_000_000_000;
const PHASE1_FILE = 'phase1_throughput.txt';
const PHASE2_FILE = 'phase2_metrics.txt';

// orders,trades,duration_ns,max_ops,shutdown_reason,p50,p90,p99
function parseEngineCsv(raw) {
    const parts = raw.trim().split(',');
    if (parts.length < 8) return null;

    return {
        orders_processed: parseInt(parts[0], 10) || 0,
        trades_executed: parseInt(parts[1], 10) || 0,
        processing_duration_ns: parseInt(parts[2], 10) || 0,
        max_ops: parseInt(parts[3], 10) || 0,
        shutdown_reason: (parts[4] || '').trim(),
        p50: parseInt(parts[5], 10) || 0,
        p90: parseInt(parts[6], 10) || 0,
        p99: parseInt(parts[7], 10) || 0,
    };
}

function readPhase1Metrics(jobDir) {
    const file = path.join(jobDir, PHASE1_FILE);
    if (!fs.existsSync(file)) return null;
    return parseEngineCsv(fs.readFileSync(file, 'utf8'));
}

function readPhase2Metrics(jobDir) {
    const file = path.join(jobDir, PHASE2_FILE);
    if (!fs.existsSync(file)) return null;
    return parseEngineCsv(fs.readFileSync(file, 'utf8'));
}

function computeScore(phase2_metrics) {
    const maxOps = phase2_metrics.max_ops ?? 0;
    const p99 = phase2_metrics.p99 ?? 0;
    const p99_penalty = 1 + (p99 / SLA_THRESHOLD_NS);

    return {
        final_score: Math.floor(maxOps / p99_penalty),
        max_ops: maxOps,
        p99_penalty,
    };
}

module.exports = {
    readPhase1Metrics,
    readPhase2Metrics,
    parseEngineCsv,
    computeScore,
};
