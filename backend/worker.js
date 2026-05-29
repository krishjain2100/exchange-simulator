const redis = require('redis');
const fs = require('fs');
const { spawn } = require('child_process');
const util = require('util');
const path = require('path');

const CORE_DIR = path.resolve('../core');
const WORK_DIR = path.resolve('../run_env');
const createLogger = require('./logger');
const log = createLogger('Worker');

async function processJob(job, workerId, workerClient) {
    const jobStartedAt = Date.now();
    log.info(`\n========================================`);
    log.info(`[Worker ${workerId}] Starting Job: ${job.job_id} for Team: ${job.team}`);

    // 1. Setup the Isolated Directory for this specific job
    const jobDir = path.join(WORK_DIR, job.job_id);
    if (!fs.existsSync(jobDir)) fs.mkdirSync(jobDir, { recursive: true });

    // Move the uploaded file from /uploads into the job's sandbox directory
    const sandboxCodePath = path.join(jobDir, 'submission.cpp');
    fs.renameSync(job.filepath, sandboxCodePath);

    // Update job status: queued → compiling
    try {
        await workerClient.hSet(`job:${job.job_id}`, {
            status: 'compiling',
            started_at: Date.now().toString()
        });
    } catch (e) {
        log.warn(`[Worker ${workerId}] Could not update job status: ${e.message}`);
    }

    // track whether this job completed successfully
    let succeeded = false;

    try {
        log.info(`[Worker ${workerId}] Spinning up Docker Sandbox...`);

        // 2. Run the Docker Container with the uploaded code
        // --rm: Delete container when finished
        // --network none: No internet access
        // -v: Mount the specific job directory to /sandbox
        // --memory="512m": Prevents them from using too much of host's RAM
        // timeoutMs = 120000 (120s): kills the container if it runs longer than the timeout
        try {
            await new Promise((resolve, reject) => {
                const dockerArgs = ['run', '--rm', '--network', 'none', '-v', `${jobDir}:/sandbox`, 'hft-sandbox'];
                const timeoutMs = 120000;
                const dockerProcess = spawn('docker', dockerArgs, { timeout: timeoutMs, killSignal: 'SIGKILL' });

                dockerProcess.stdout.on('data', (data) => process.stdout.write(data.toString()));
                dockerProcess.stderr.on('data', (data) => log.error(`[Docker ERROR] ${data.toString()}`));

                (async () => {
                    try {
                        console.log(`[Worker ${workerId}] Updating job status to running in Redis...`);
                        await workerClient.hSet(`job:${job.job_id}`, {
                            status: 'running',
                            started_at: Date.now().toString()
                        });
                    } catch (e) {
                        log.warn(`[Worker ${workerId}] Could not update job status to running: ${e.message}`);
                    }
                })();

                dockerProcess.on('error', (err) => reject(err));

                dockerProcess.on('close', (code, signal) => {
                    if (code === 0) {
                        return resolve();
                    } else if (signal === 'SIGKILL') {
                        return reject(new Error(`Docker process killed after exceeding time limit of ${timeoutMs / 1000} seconds`));
                    } else if (code !== null) {
                        return reject(new Error(`Docker exited with code ${code}`));
                    } else {
                        return reject(new Error(`Docker terminated by signal ${signal}`));
                    }
                });
            });

        } catch (dockerError) {
            try {
                await workerClient.hSet(`job:${job.job_id}`, {
                    status: 'failed',
                    finished_at: Date.now().toString(),
                    error: `docker:${dockerError.message}`
                });
            } catch (e) {
                log.warn(`[Worker ${workerId}] Could not update job status after Docker failure: ${e.message}`);
            }
            return;
        }

        // 3. Run the Verifier (On the Mac Host)
        log.info(`[Worker ${workerId}] Verifying Correctness...`);

        try {
            await new Promise((resolve, reject) => {
                const verifyProcess = spawn(`${CORE_DIR}/verifier`, [jobDir]);

                verifyProcess.stdout.on('data', (data) => process.stdout.write(data.toString()));
                verifyProcess.stderr.on('data', (data) => log.error(`[Verifier ERROR] ${data.toString()}`));

                verifyProcess.on('close', (code) => {
                    if (code === 0) {
                        resolve();
                    } else {
                        reject(new Error(`Verification failed with exit code: ${code}`));
                    }
                });
            });

            log.info(`[Worker ${workerId}] Correctness Passed!`);

            // 4. Score the submission by reading the latency.txt file generated by the Docker container
            const latencyFilePath = path.join(jobDir, 'latency.txt');
            if (!fs.existsSync(latencyFilePath)) {
                log.error(`[Worker ${workerId}] latency.txt was not generated!`);
                try {
                    await workerClient.hSet(`job:${job.job_id}`, {
                        status: 'failed',
                        finished_at: Date.now().toString(),
                        error: 'missing_latency'
                    });
                } catch (e) {
                    log.warn(`[Worker ${workerId}] Could not update job status after missing latency: ${e.message}`);
                }
                return;
            }

            const latencyStr = fs.readFileSync(latencyFilePath, 'utf8');
            const parts = latencyStr.split(',').map(v => parseInt(v.trim(), 10));
            if (parts.length < 3 || parts.some(v => Number.isNaN(v))) {
                log.error(`[Worker ${workerId}] Invalid latency.txt contents:`, latencyStr);
                try {
                    await workerClient.hSet(`job:${job.job_id}`, {
                        status: 'failed',
                        finished_at: Date.now().toString(),
                        error: 'invalid_latency'
                    });
                } catch (e) {
                    log.warn(`[Worker ${workerId}] Could not update job status after invalid latency: ${e.message}`);
                }
                return;
            }

            const [p50, p90, p99] = parts;
            const compositeScore = Math.floor((p50 * 0.5) + (p90 * 0.3) + (p99 * 0.2));

            console.log(`[Worker ${workerId}] p50: ${p50}ns, p90: ${p90}ns, p99: ${p99}ns`);
            console.log(`[Worker ${workerId}] Official Composite Score: ${compositeScore}`);

            // Add to Redis (Redis ZSets rank from lowest to highest by default)
            try {
                await workerClient.zAdd('leaderboard', { score: compositeScore, value: job.team });
                await workerClient.hSet(`team_metrics:${job.team}`, {
                    p50: p50.toString(),
                    p90: p90.toString(),
                    p99: p99.toString(),
                    score: compositeScore.toString()
                });
            } catch (err) {
                console.error(`[Worker ${workerId}] Failed to update leaderboard in Redis:`, err);
                return;
            }

            succeeded = true;
            try {
                await workerClient.hSet(`job:${job.job_id}`, {
                    status: 'passed',
                    finished_at: Date.now().toString()
                });
            } catch (e) {
                console.warn(`[Worker ${workerId}] Could not update job status to passed: ${e.message}`);
            }

        } catch (err) {
            console.log(`[Worker ${workerId}] Correctness FAILED: ${err.message}`);
            try {
                await workerClient.hSet(`job:${job.job_id}`, {
                    status: 'failed',
                    finished_at: Date.now().toString(),
                    error: `verifier:${err.message}`
                });
            } catch (e) {
                console.warn(`[Worker ${workerId}] Could not update job status to failed: ${e.message}`);
            }
            return;
        }

    } catch (err) {
        console.error(`[Worker ${workerId}] ERROR:`, err.message);
        try {
            await workerClient.hSet(`job:${job.job_id}`, {
                status: 'failed',
                finished_at: Date.now().toString(),
                error: `worker:${err.message}`
            });
        } catch (e) {
            console.warn(`[Worker ${workerId}] Could not update job status to failed: ${e.message}`);
        }
    } finally {
        const durationMs = Date.now() - jobStartedAt;

        log.info(`[Worker ${workerId}] Finished job ${job.job_id} in ${durationMs}ms`);

        if (!succeeded) {
            try {
                const existingStatus = await workerClient.hGet(`job:${job.job_id}`, 'status');
                const existingError = await workerClient.hGet(`job:${job.job_id}`, 'error');

                // If the job already has a terminal status, don't overwrite it.
                if (existingStatus !== 'passed' && existingStatus !== 'failed') {
                    await workerClient.hSet(`job:${job.job_id}`, {
                        status: 'failed',
                        finished_at: Date.now().toString(),
                        error: existingError || 'unknown_failure'
                    });
                } else {
                    log.info(`[Worker ${workerId}] Job ${job.job_id} already has terminal status: ${existingStatus}`);
                }
            } catch (e) {
                console.warn(`[Worker ${workerId}] Could not update job status to failed: ${e.message}`);
            }
        }
        // if (fs.existsSync(jobDir)) fs.rmSync(jobDir, { recursive: true, force: true });
    }
}

// Redis Worker Loop
async function startWorker(workerId) {
    const workerClient = redis.createClient({ url: 'redis://127.0.0.1:6379' });
    try {
        await workerClient.connect();
        console.log(`[Worker ${workerId}] Connected to Redis. Waiting for submissions...`);
    } catch (err) {
        console.error(`[Worker ${workerId}] Redis Connection Error:`, err);
        process.exit(1);
    }

    while (true) {
        try {
            // brPop blocks the loop until a job appears in the queue
            // It is atomic and safe for multiple workers to call simultaneously without conflicts
            const result = await workerClient.brPop('hackathon:queue', 0);
            const job = JSON.parse(result.element);

            await processJob(job, workerId, workerClient);
        } catch (err) {
            console.error(`[Worker ${workerId}] Redis pulling error:`, err);
        }
    }
}

const CONCURRENT_WORKERS = 4;

console.log(`[Fleet] Booting up ${CONCURRENT_WORKERS} concurrent workers...`);

for (let i = 0; i < CONCURRENT_WORKERS; i++) {
    startWorker(i + 1);
}