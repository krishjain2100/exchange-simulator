const fs = require('fs');
const path = require('path');
const { S3Client, GetObjectCommand } = require('@aws-sdk/client-s3');
const { pipeline } = require('stream/promises');
const createLogger = require('../shared/logger');
const { patchJob, trySetTeamBest } = require('../shared/redisStore');
const { readPhase1Metrics,readPhase2Metrics,computeScore } = require('../shared/jobMetrics');
const { runChild } = require('./engine');
const { runDockerSandbox } = require('./dockerSandbox');
const { WORK_DIR, BENCHMARK_LOAD_PATH, BOT_REPLAY_BINARY, VERIFIER_BINARY, BUCKET_NAME } = require('./config');

const log = createLogger('Worker');
const s3 = new S3Client({ region: process.env.AWS_REGION || 'us-east-1' });

async function downloadSubmission(job, sandboxCodePath) {
    log.info(`Downloading ${job.s3_key} from S3...`);
    const response = await s3.send(new GetObjectCommand({ Bucket: BUCKET_NAME, Key: job.s3_key }));
    await pipeline(response.Body, fs.createWriteStream(sandboxCodePath));
}

async function runVerifier(jobDir) {
    await runChild(VERIFIER_BINARY, [jobDir], {
        onStderrLine: (line) => log.error(`[Verifier ERROR] ${line}`),
    });
}

async function runPhase1(workerClient, job_id, jobDir) {
    log.info(`[Worker] === Phase 1: Correctness Testing ===`);
    await patchJob(workerClient, job_id, { status: 'phase1_running' });
    await runDockerSandbox({job_id, jobDir, workerClient, phase: 'correctness' });
    const phase1_metrics = readPhase1Metrics(jobDir);
    if (!phase1_metrics) throw new Error('Phase 1 metrics missing');
    await patchJob(workerClient, job_id, { phase1_metrics });
    log.info(`[Worker] Verifying Phase 1 correctness...`);
    await runVerifier(jobDir);
    log.info(`[Worker] Phase 1 correctness passed.`);
    return phase1_metrics;
}

async function runPhase2(workerClient, job_id, jobDir) {
    log.info(`[Worker] === Phase 2: Benchmark Scoring ===`);
    await patchJob(workerClient, job_id, { status: 'phase2_running' });
    await runDockerSandbox({job_id, jobDir, workerClient, phase: 'benchmark' });
    const phase2_metrics = readPhase2Metrics(jobDir);
    if (!phase2_metrics) throw new Error('Phase 2 metrics missing');
    await patchJob(workerClient, job_id, { phase2_metrics });
    log.info(`[Worker] Phase 2 benchmark passed.`);
    return phase2_metrics;
}

async function finalizeSuccess(workerClient, job, phase1_metrics, phase2_metrics) {
    const score_metrics = computeScore(phase1_metrics, phase2_metrics);

    await patchJob(workerClient, job.job_id, {
        status: 'passed',
        finished_at: Date.now(),
        score_metrics,
        phase1_metrics,
        phase2_metrics,
    });

    const bestRun = {
        team: job.team,
        best_job_id: job.job_id,
        best_score_at: Date.now(),
        score_metrics,
        phase1_metrics,
        phase2_metrics,
    };

    try {
        await trySetTeamBest(workerClient, job.team, bestRun);
    } catch (err) {
        log.error(`[Leaderboard] Failed to update best score for ${job.team}:`, err.message);
    }
}

async function setFailedStatus(workerClient, job_id, message, phase1_metrics = {}, phase2_metrics = {}) {
    const patch = {
        status: 'failed',
        finished_at: Date.now(),
        error: message.slice(0, 2000),
    };

    if (phase1_metrics) patch.phase1_metrics = phase1_metrics;
    if (phase2_metrics) patch.phase2_metrics = phase2_metrics;

    await patchJob(workerClient, job_id, patch);
}

async function processJob(job, workerClient) {
    log.info('\n========================================');
    log.info(`[Worker] Starting Job: ${job.job_id} for Team: ${job.team}`);

    const jobDir = path.join(WORK_DIR, job.job_id);
    const sandboxCodePath = path.join(jobDir, 'submission.cpp');

    let phase1_metrics = null;
    let phase2_metrics = null;

    try {
        if (!fs.existsSync(BENCHMARK_LOAD_PATH)) {
            throw new Error(`Missing benchmark load file at ${BENCHMARK_LOAD_PATH}`);
        }
        if (!fs.existsSync(BOT_REPLAY_BINARY)) {
            throw new Error(`Missing bot_replay binary at ${BOT_REPLAY_BINARY}`);
        }
        if (!fs.existsSync(VERIFIER_BINARY)) {
            throw new Error(`Missing verifier binary at ${VERIFIER_BINARY}`);
        }

        fs.mkdirSync(jobDir, { recursive: true });

        await patchJob(workerClient, job.job_id, {
            status: 'compiling',
            started_at: Date.now(),
        });

        await downloadSubmission(job, sandboxCodePath);

        phase1_metrics = await runPhase1(workerClient, job.job_id, jobDir);
        phase2_metrics = await runPhase2(workerClient, job.job_id, jobDir);
        await finalizeSuccess(workerClient, job, phase1_metrics, phase2_metrics);
    } catch (err) {
        const message = err?.message || String(err);
        log.error(`[Worker] Job failed:`, message);
        await setFailedStatus(workerClient, job.job_id, message, phase1_metrics, phase2_metrics).catch((redisErr) => {
            log.error(`[Worker] Failed to persist job error:`, redisErr.message);
        });
    } finally {
        if (fs.existsSync(jobDir)) fs.rmSync(jobDir, { recursive: true, force: true });
    }
}

module.exports = { processJob };
