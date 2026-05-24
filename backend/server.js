const express = require('express');
const multer = require('multer');
const redis = require('redis');
const path = require('path');
const fs = require('fs');

const app = express();

app.use((req, res, next) => {
    console.log(`[Express Network] Incoming ${req.method} request to ${req.url}`);
    next();
});

// 1. Configure Multer to save uploaded files to a local directory called 'uploads'
const UPLOADS_DIR = path.join(__dirname, 'uploads');
if (!fs.existsSync(UPLOADS_DIR)) {
    fs.mkdirSync(UPLOADS_DIR);
    // Note: In production, you would want to handle file storage more robustly (e.g., S3, GCS) and clean up old files regularly
}

const upload = multer({ dest: UPLOADS_DIR });

// 2. Setup Redis client
const redisClient = redis.createClient({
    url: 'redis://127.0.0.1:6379' 
});

async function startServer() {
    await redisClient.connect();
    console.log('Redis Connected');

    // 3. Submit Endpoint: Receives the uploaded file and team name, saves the file, and pushes a job ticket to Redis
    app.post('/api/submit', upload.single('code_file'), async (req, res) => {
        try {
            const teamName = req.body.team_name;
            const filePath = req.file.path; // Multer handled the disk save automatically
            const jobId = "job_" + Date.now();

            console.log(`[API] Received file from ${teamName}. Saved locally at ${filePath}`);

            // 4. Create the JSON ticket for the worker
            const jobPayload = JSON.stringify({
                job_id: jobId,
                team: teamName,
                filepath: filePath,
                status: "queued"
            });

            // 5. Push the ticket onto the Redis Queue
            await redisClient.lPush('hackathon:queue', jobPayload);
            console.log(`[Redis] Job ${jobId} pushed to queue.`);

            // 6. Respond to the client
            res.status(200).json({ success: true, job_id: jobId, message: "File saved and queued!" });

        } catch (error) {
            console.error(error);
            res.status(500).json({ success: false, message: "Internal Server Error" });
        }
    });

    // 7. Leaderboard Endpoint: Retrieves the sorted leaderboard from Redis and sends it to the frontend
    app.get('/api/leaderboard', async (req, res) => {
        try {
            const results = await redisClient.zRangeWithScores('leaderboard', 0, -1);
            const leaderboard = results.map((entry, index) => ({
                rank: index + 1,
                team: entry.value,
                latency_ns: entry.score
            }));

            res.status(200).json({ success: true, leaderboard });
        } catch (error) {
            console.error(error);
            res.status(500).json({ success: false });
        }
    });

    // Bind to IPv4
    app.listen(3000, '127.0.0.1', () => {
        console.log('[API] Gateway listening on http://127.0.0.1:3000');
    });
}

startServer();