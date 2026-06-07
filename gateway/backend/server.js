const express = require('express');
const multer = require('multer');
const redis = require('redis');
const path = require('path');
const { S3Client, PutObjectCommand } = require("@aws-sdk/client-s3");
const { STSClient, GetCallerIdentityCommand } = require("@aws-sdk/client-sts");
require('dotenv').config({ path: path.resolve(__dirname, '../../.env') });

const createLogger = require('../../shared/logger');
const log = createLogger('API');
const { jobKey, writeJson, getJob, fetchLeaderboard, connectRedisWithRetry, nextJobId } = require('../../shared/redisStore');

const app = express();
const awsRegion = process.env.AWS_REGION;
const s3 = new S3Client({ region: awsRegion });
const sts = new STSClient({ region: awsRegion });
const BUCKET_NAME = process.env.S3_BUCKET_NAME;

app.use((req, _res, next) => {
    log.info(`Incoming ${req.method} request to ${req.url}`);
    next();
});

const upload = multer({ storage: multer.memoryStorage() });

const redisClient = redis.createClient({
    url: process.env.REDIS_URL,
});

async function validateAwsIdentity() {
    try {
        const identity = await sts.send(new GetCallerIdentityCommand({}));
        log.info(`[API] AWS identity verified for account ${identity.Account} (${identity.Arn})`);
    } catch (err) {
        const detail = err?.name || err?.Code || err?.message || 'Unknown AWS credential error';
        throw new Error(`[API] AWS credential validation failed: ${detail}`);
    }
}

async function startServer() {
    if (!process.env.REDIS_URL) {
        throw new Error('[API] REDIS_URL is not set');
    }

    try {
        await connectRedisWithRetry(redisClient, { label: 'API' });
    } catch (err) {
        console.error('[API] Redis Connection Error after retries:', err.message);
        process.exit(1);
    }

    try {
        await validateAwsIdentity();
    } catch (err) {
        console.error('[API] AWS credential validation failed:', String(err));
        process.exit(1);
    }

    app.post('/api/submit', upload.single('code_file'), async (req, res) => {
        try {
            const teamName = req.body.team_name;
            if (!teamName) {
                return res.status(400).json({ success: false, message: "Team name is required" });
            }

            const jobId = await nextJobId(redisClient);
            const s3Key = `submissions/${jobId}.cpp`;

            if (!req.file) {
                return res.status(400).json({ success: false, message: "No file uploaded" });
            }

            log.info(`[API] Received file from ${teamName}. Streaming to S3...`);

            await s3.send(new PutObjectCommand({
                Bucket: BUCKET_NAME,
                Key: s3Key,
                Body: req.file.buffer,
                ContentType: "text/x-c"
            }));

            log.info(`[API] File uploaded successfully to ${BUCKET_NAME}/${s3Key}`);

            const jobPayload = JSON.stringify({
                job_id: jobId,
                team: teamName,
                s3_key: s3Key,
            });

            try {
                await redisClient.lPush('hackathon:queue', jobPayload);
            } catch (err) {
                console.error('Failed to push job to Redis queue:', err);
                return res.status(500).json({ success: false, message: "Failed to queue the job" });
            }

            try {
                await writeJson(redisClient, jobKey(jobId), {
                    job_id: jobId,
                    team: teamName,
                    status: 'queued',
                    created_at: Date.now(),
                });
                await redisClient.lPush('hackathon:jobs', jobId);
                log.info(`[Redis] Job ${jobId} pushed to queue.`);
            } catch (err) {
                console.error('Failed to set job record in Redis:', err);
                return res.status(500).json({ success: false, message: "Failed to initialize job status" });
            }

            res.status(200).json({ success: true, job_id: jobId, message: "File uploaded to S3 and queued!" });
        } catch (error) {
            console.error("[API] Submit Route Error:", error);
            res.status(500).json({ success: false, message: "Internal Server Error" });
        }
    });

    app.get('/api/leaderboard', async (_req, res) => {
        try {
            res.json(await fetchLeaderboard(redisClient));
        } catch (err) {
            log.error("Leaderboard fetch error:", err);
            res.status(500).json({ error: "Failed to fetch leaderboard" });
        }
    });

    app.get('/api/jobs', async (_req, res) => {
        try {
            const ids = await redisClient.lRange('hackathon:jobs', 0, 199);
            const jobs = (await Promise.all(ids.map((id) => getJob(redisClient, id)))).filter(Boolean);
            res.json(jobs);
        } catch (err) {
            log.error('Failed to fetch jobs:', err);
            res.status(500).json({ error: 'Failed to fetch jobs' });
        }
    });

    app.listen(3000, '127.0.0.1', () => {
        console.log('[API] Gateway listening on http://127.0.0.1:3000');
    });
}

startServer();
