const redis = require('redis');
const { spawn } = require('child_process');
const path = require('path');
require('dotenv').config({ path: path.resolve(__dirname, '../.env') });
const { connectRedisWithRetry } = require('../shared/redisStore');

const BOT_FLEET_BINARY = path.join(__dirname, 'bot_fleet');
const MAX_CONCURRENT_SHARDS = 32;

function runBotFleet(ticket) {
    return new Promise((resolve, reject) => {
        const args = [
            ticket.target_ip,
            ticket.target_port.toString(),
            ticket.bot_group_id.toString(),
        ];
        if (ticket.test_starts_at) {
            args.push(ticket.test_starts_at.toString());
        }

        console.log(
            `[Bot Node] Launching correctness shard ${ticket.bot_group_id} → `
            + `${ticket.target_ip}:${ticket.target_port}`
        );

        const botProcess = spawn(BOT_FLEET_BINARY, args);
        botProcess.stdout.on('data', (data) => process.stdout.write(`[Fleet ${ticket.job_id}] ${data}`));
        botProcess.stderr.on('data', (data) => process.stderr.write(`[Fleet ERROR ${ticket.job_id}] ${data}`));
        botProcess.on('error', reject);
        botProcess.on('close', (code) => {
            if (code === 0) resolve();
            else reject(new Error(`bot_fleet exited with code ${code}`));
        });
    });
}

let activeShards = 0;

async function launchShard(ticket) {
    activeShards++;
    console.log(`[Bot Node] Active shards: ${activeShards}/${MAX_CONCURRENT_SHARDS}`);

    try {
        await runBotFleet(ticket);
        console.log(`[Bot Node] Shard ${ticket.bot_group_id} finished OK`);
    } catch (err) {
        console.error(`[Bot Node] Shard ${ticket.bot_group_id} failed:`, err.message);
    } finally {
        activeShards--;
    }
}

async function startBotWorker() {
    const redisClient = redis.createClient({ url: process.env.REDIS_URL });

    if (!process.env.REDIS_URL) {
        console.error('[Bot Node] REDIS_URL is not set');
        process.exit(1);
    }

    try {
        await connectRedisWithRetry(redisClient, { label: 'Bot Node' });
    } catch (err) {
        console.error('[Bot Node] Redis connection error after retries:', err.message);
        process.exit(1);
    }

    console.log(`[Bot Node] Concurrent shard cap: ${MAX_CONCURRENT_SHARDS}`);

    while (true) {
        try {
            const result = await redisClient.brPop('hackathon:bot_queue', 0);
            const ticket = JSON.parse(result.element);

            while (activeShards >= MAX_CONCURRENT_SHARDS) {
                await new Promise((resolve) => setTimeout(resolve, 250));
            }

            launchShard(ticket);
        } catch (err) {
            console.error('[Bot Node] Execution error:', err);
        }
    }
}

startBotWorker();
