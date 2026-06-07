const TEAMS_SET = 'hackathon:teams';
const JOB_SEQ_KEY = 'hackathon:job_seq';

function jobKey(jobId) {
    return `job:${jobId}`;
}

function teamBestKey(team) {
    return `team_best:${team}`;
}

async function readJson(client, key) {
    const raw = await client.get(key);
    if (!raw) return null;
    return JSON.parse(raw);
}

async function writeJson(client, key, value) {
    await client.set(key, JSON.stringify(value));
}

async function patchJob(client, jobId, patch) {
    const key = jobKey(jobId);
    const current = await readJson(client, key);
    if (!current) {
        throw new Error(`Job ${jobId} not found`);
    }
    await writeJson(client, key, { ...current, ...patch });
}

async function getJob(client, jobId) {
    return readJson(client, jobKey(jobId));
}

async function trySetTeamBest(client, team, bestRun) {
    const key = teamBestKey(team);
    const current = await readJson(client, key);
    if (current && current.score_metrics.final_score >= bestRun.score_metrics.final_score) {
        return;
    }

    await writeJson(client, key, bestRun);
    await client.sAdd(TEAMS_SET, team);
}

async function fetchLeaderboard(client) {
    const teams = await client.sMembers(TEAMS_SET);
    const entries = await Promise.all(teams.map((team) => readJson(client, teamBestKey(team))));
    return entries.filter(Boolean).sort(
        (a, b) => b.score_metrics.final_score - a.score_metrics.final_score
    );
}

async function nextJobId(client) {
    const seq = await client.incr(JOB_SEQ_KEY);
    return `job_${seq}`;
}

async function connectRedisWithRetry(client, { label = 'Redis', maxRetries = 3, delayMs = 5000 } = {}) {
    for (let attempt = 1; attempt <= maxRetries; attempt++) {
        try {
            await client.connect();
            console.log(`[${label}] Redis connected`);
            return;
        } catch (err) {
            if (attempt < maxRetries) {
                await new Promise((r) => setTimeout(r, delayMs));
            } else {
                throw new Error(`[${label}] Redis connection attempt ${attempt} failed: ${String(err)}`);
            }
        }
    }
}

module.exports = {
    TEAMS_SET,
    JOB_SEQ_KEY,
    jobKey,
    teamBestKey,
    readJson,
    writeJson,
    patchJob,
    getJob,
    nextJobId,
    trySetTeamBest,
    fetchLeaderboard,
    connectRedisWithRetry,
};
