const redis = require('redis');
const path = require('path');
require('dotenv').config({ path: path.resolve(__dirname, '../.env') });
const { processJob } = require('./jobRunner');
const { connectRedisWithRetry } = require('../shared/redisStore');

const CONCURRENT_WORKERS = 1;

async function startWorker() {
    const workerClient = redis.createClient({ url: process.env.REDIS_URL });

    try {
        await connectRedisWithRetry(workerClient, { label: 'Worker' });
    } catch (err) {
        console.error('[Worker] Redis connection error after retries:', err.message);
        process.exit(1);
    }

    console.log('[Worker] Waiting for submissions...');

    while (true) {
        try {
            const result = await workerClient.brPop('hackathon:queue', 0);
            await processJob(JSON.parse(result.element), workerClient);
        } catch (err) {
            console.error('[Worker] Queue polling error:', err.message);
            await new Promise((resolve) => setTimeout(resolve, 1000));
        }
    }
}

async function boot() {
    if (!process.env.REDIS_URL) {
        console.error('[Worker] REDIS_URL is not set');
        process.exit(1);
    }

    console.log(`[Worker] Booting up ${CONCURRENT_WORKERS} concurrent worker...`);
    await startWorker();
}

boot();
