const fs = require('fs');
const path = require('path');

const SCORE_K = 1;
const SLA_THRESHOLD_NS = 1_000_000_000;
const SCORE_W_ORDERS = 0.01;

const PHASE1_THROUGHPUT_FILE = 'phase1_throughput.txt';
const PHASE2_METRICS_FILE = 'phase2_metrics.txt';

function parsePhase1Line(parts) {
    if (parts.length < 6) return null;
    return {
        orders_processed: parseInt(parts[0], 10) || 0,
        trades_executed: parseInt(parts[1], 10) || 0,
        processing_duration_ns: parseInt(parts[2], 10) || 0,
        order_tps: parseInt(parts[3], 10) || 0,
        trade_tps: parseInt(parts[4], 10) || 0,
        shutdown_reason: parts[5] || 'unknown',
    };
}

function parsePhase2Line(parts) {
    if (parts.length < 9) return null;
    return {
        orders_processed: parseInt(parts[0], 10) || 0,
        trades_executed: parseInt(parts[1], 10) || 0,
        processing_duration_ns: parseInt(parts[2], 10) || 0,
        order_tps: parseInt(parts[3], 10) || 0,
        trade_tps: parseInt(parts[4], 10) || 0,
        shutdown_reason: parts[5] || 'unknown',
        p50: parseInt(parts[6], 10) || 0,
        p90: parseInt(parts[7], 10) || 0,
        p99: parseInt(parts[8], 10) || 0,
    };
}

function readPhase1Metrics(jobDir) {
    const filePath = path.join(jobDir, PHASE1_THROUGHPUT_FILE);
    if (!fs.existsSync(filePath)) return null;
    return parsePhase1Line(fs.readFileSync(filePath, 'utf8').trim().split(','));
}

function readPhase2Metrics(jobDir) {
    const filePath = path.join(jobDir, PHASE2_METRICS_FILE);
    if (!fs.existsSync(filePath)) return null;
    return parsePhase2Line(fs.readFileSync(filePath, 'utf8').trim().split(','));
}

function computeScore(phase1_metrics, phase2_metrics) {
    const trade_tps = phase2_metrics.trade_tps;
    const p99 = phase2_metrics.p99;
    const phase1_orders = phase1_metrics.orders_processed;
    const latency_ratio = p99 / SLA_THRESHOLD_NS;
    const p99_penalty = Math.max(0.1, 1 + (latency_ratio ** SCORE_K));
    const benchmark_score = Math.floor(trade_tps * p99_penalty);
    const throughput_score = Math.floor(SCORE_W_ORDERS * phase1_orders);
    const final_score = benchmark_score + throughput_score;

    return {
        final_score,
        throughput_score,
        benchmark_score,
        p99_penalty,
        latency_ratio,
    };
}

module.exports = {
    readPhase1Metrics,
    readPhase2Metrics,
    computeScore,
};
