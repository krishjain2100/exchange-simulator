const redis = require('redis');
const { spawn } = require('child_process');
const path = require('path');
require('dotenv').config({ path: path.resolve(__dirname, '../.env') });
const { connectRedisWithRetry } = require('../shared/redisStore');

const BOT_FLEET = process.env.BOT_FLEET_BINARY || path.join(__dirname, 'bot_fleet');

function runBotFleet(ticket) {
    return new Promise((resolve, reject) => {
        const args = [
            ticket.target_ip,
            String(ticket.target_port),
        ];

        const proc = spawn(BOT_FLEET, args);
        proc.stdout.on('data', (d) => process.stdout.write(`[Fleet ${ticket.job_id}] ${d}`));
        proc.stderr.on('data', (d) => process.stderr.write(`[Fleet ${ticket.job_id}] ${d}`));
        proc.on('error', reject);
        proc.on('close', (code) => {
            if (code === 0) resolve();
            else reject(new Error(`bot_fleet exited with code ${code}`));
        });
    });
}

async function startBotWorker() {
    const redisClient = redis.createClient({ url: process.env.REDIS_URL });
    if (!process.env.REDIS_URL) {
        console.error('[Bot Node] REDIS_URL is not set');
        process.exit(1);
    }

    await connectRedisWithRetry(redisClient, { label: 'Bot Node' });
    console.log('[Bot Node] Ready');

    while (true) {
        try {
            const { element } = await redisClient.brPop('hackathon:bot_queue', 0);
            runBotFleet(JSON.parse(element)).catch((err) => {
                console.error('[Bot Node] Fleet error:', err.message);
            });
        } catch (err) {
            console.error('[Bot Node] Queue error:', err.message);
        }
    }
}

startBotWorker();
