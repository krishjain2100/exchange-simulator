const express = require('express');
const multer = require('multer');
const redis = require('redis');
const path = require('path');
const fs = require('fs');

const app = express();
const createLogger = require('./logger');
const log = createLogger('API');

app.use((req, res, next) => {
    log.info(`Incoming ${req.method} request to ${req.url}`);
    next();
});

// 1. Configure Multer to save uploaded files to a local directory called 'uploads'
const UPLOADS_DIR = path.join(__dirname, 'uploads');
if (!fs.existsSync(UPLOADS_DIR)) {
    fs.mkdirSync(UPLOADS_DIR);
    // Note: In production, you would want to handle file storage more robustly (e.g., S3, GCS)
    //  and clean up old files regularly
}

const upload = multer({ dest: UPLOADS_DIR });

// 2. Setup Redis client
const redisClient = redis.createClient({
    url: 'redis://127.0.0.1:6379'
});

async function connectRedisWithRetry(maxRetries = 3, delayMs = 3000) {
    for (let attempt = 1; attempt <= maxRetries; attempt++) {
        try {
            await redisClient.connect();
            console.log('Redis Connected');
            return;
        } catch (err) {
            console.error(`Redis connection attempt ${attempt} failed:`, err.message || err);
            if (attempt < maxRetries) {
                await new Promise((r) => setTimeout(r, delayMs));
            } else {
                throw err;
            }
        }
    }
}

async function startServer() {
    try {
        await connectRedisWithRetry(3, 5000);
    } catch (err) {
        console.error('Redis Connection Error after retries:', err);
        process.exit(1);
    }

    // 3. Submit Endpoint: Receives the uploaded file and team name, saves the file, and pushes a job ticket to Redis
    app.post('/api/submit', upload.single('code_file'), async (req, res) => {
        try {
            const teamName = req.body.team_name;
            const filePath = req.file.path; // Multer handled the disk save automatically
            const jobId = "job_" + Date.now();

            log.info(`[API] Received file from ${teamName}. Saved locally at ${filePath}`);

            // 4. Create the JSON ticket for the worker
            const jobPayload = JSON.stringify({
                job_id: jobId,
                team: teamName,
                filepath: filePath,
                status: "queued"
            });

            // 5. Push the ticket onto the Redis Queue
            try {
                await redisClient.lPush('hackathon:queue', jobPayload);
            }
            catch (err) {
                console.error('Failed to push job to Redis queue:', err);
                return res.status(500).json({ success: false, message: "Failed to queue the job" });
            }

            const jobKey = `job:${jobId}`;

            try {
                await redisClient.hSet(jobKey, {
                    job_id: jobId,
                    team: teamName,
                    status: 'queued',
                    created_at: Date.now().toString()
                });
            } catch (err) {
                console.error('Failed to set job hash in Redis:', err);
                return res.status(500).json({ success: false, message: "Failed to initialize job status" });
            }
            try {
                await redisClient.lPush('hackathon:jobs', jobId);
                await redisClient.lTrim('hackathon:jobs', 0, 199); // Refine later
                log.info(`[Redis] Job ${jobId} pushed to queue.`);
            } catch (err) {
                console.error('Failed to push job to Redis jobs list:', err);
                return res.status(500).json({ success: false, message: "Failed to initialize job status" });
            }

            res.status(200).json({ success: true, job_id: jobId, message: "File saved and queued!" });

        } catch (error) {
            console.error(error);
            res.status(500).json({ success: false, message: "Internal Server Error" });
        }
    });

    // Leaderboard Endpoint: Retrieves the sorted leaderboard from Redis and sends it to the frontend
    app.get('/api/leaderboard', async (req, res) => {
        try {
            // 1. Get the top teams ranked by their composite score (lowest to highest)
            const rankedTeams = await redisClient.zRangeWithScores('leaderboard', 0, -1);

            // 2. Fetch the  metrics (p50, p90, p99) for each team
            const fullLeaderboard = await Promise.all(rankedTeams.map(async (entry) => {
                const teamName = entry.value;
                const compositeScore = entry.score;

                const metrics = await redisClient.hGetAll(`team_metrics:${teamName}`);

                return {
                    team: teamName,
                    composite: compositeScore,
                    p50: parseInt(metrics.p50 || 0, 10),
                    p90: parseInt(metrics.p90 || 0, 10),
                    p99: parseInt(metrics.p99 || 0, 10)
                };
            }));

            res.json(fullLeaderboard);
        } catch (err) {
            log.error("Leaderboard fetch error:", err);
            res.status(500).json({ error: "Failed to fetch leaderboard" });
        }
    }); // cache or something if this becomes a bottleneck


    app.get('/api/jobs', async (req, res) => {
        try {
            const ids = await redisClient.lRange('hackathon:jobs', 0, 199);
            const jobs = await Promise.all(ids.map(async (id) => {
                const data = await redisClient.hGetAll(`job:${id}`);
                return {
                    job_id: data.job_id || id,
                    team: data.team || 'unknown',
                    status: data.status || 'unknown',
                    created_at: data.created_at ? parseInt(data.created_at, 10) : null,
                    started_at: data.started_at ? parseInt(data.started_at, 10) : null,
                    finished_at: data.finished_at ? parseInt(data.finished_at, 10) : null,
                    error: data.error || null
                };
            })); // refine later.

            res.json(jobs);
        } catch (err) {
            log.error('Failed to fetch jobs:', err);
            res.status(500).json({ error: 'Failed to fetch jobs' });
        }
    });

    // Bind to IPv4
    app.listen(3000, '127.0.0.1', () => {
        console.log('[API] Gateway listening on http://127.0.0.1:3000');
    });
}

startServer();