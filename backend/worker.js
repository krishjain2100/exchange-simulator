const redis = require('redis');
const { spawn, exec } = require('child_process');
const fs = require('fs');
const util = require('util');
const path = require('path');
const execPromise = util.promisify(exec);

const redisClient = redis.createClient({ url: 'redis://127.0.0.1:6379' });

// Requires compiled bot_fleet in the core folder
// Generates CSV files in the run_env folder
const CORE_DIR = '../core';
const WORK_DIR = './run_env'; 

async function processJob(job) {
    console.log(`[Worker] Starting Job: ${job.job_id} for Team: ${job.team}`);
    
    if (!fs.existsSync(WORK_DIR)) fs.mkdirSync(WORK_DIR);

    const engineExe = `${WORK_DIR}/${job.job_id}_engine`;

    try {
        // 1. Compile the user's uploaded C++ code with the provided Wrapper.cpp to create the engine executable
        console.log(`[Worker] Compiling C++...`);
        // We compile their uploaded file with Wrapper.cpp
        // Added '-x c++' so g++ knows the extensionless file is C++ code.
        try {
            await execPromise(`g++ -O3 -std=c++17 -I${CORE_DIR} ${CORE_DIR}/Wrapper.cpp -x c++ ${job.filepath} -o ${engineExe}`);
        } catch (compileErr) {
            console.error(`[Worker] Compilation Failed!`, compileErr.stderr);
            // Will save the compilation error back to Redis so the frontend can display it to the user
            return; // Stop processing this job
        }

        // 2. Run the compiled executable, which will execute the load test and generate the latency.csv and correctness.csv files in the run_env folder
        console.log(`[Worker] Executing Load Test...`);
        try {
            await runLoadTest(engineExe);
        } catch (loadTestErr) {
            console.error(`[Worker] Load Test Failed!`, loadTestErr.message);
            // Will save the load test failure reason back to Redis so the frontend can display it to the user
            return; // Stop processing this job
        }

        // 3. Run the golden model Python script to verify correctness and extract latency. The script will read the generated CSVs and print "SUCCESS" if the submission is correct, along with writing the latency in nanoseconds to latency.txt
        console.log(`[Worker] Verifying Correctness...`);
        const { stdout } = await execPromise(`python3 ${CORE_DIR}/golden_model.py ${WORK_DIR}`);
        
        if (stdout.includes("SUCCESS")) {
            const latencyFilePath = path.join(WORK_DIR, 'latency.txt');
            
            if (fs.existsSync(latencyFilePath)) {
                // Read the text file and convert the latency string "42" into an integer 42
                const latencyStr = fs.readFileSync(latencyFilePath, 'utf8');
                const latency = parseInt(latencyStr.trim(), 10);
                
                console.log(`[Worker] Assigning Official Score: ${latency} ns`);
                
                await redisClient.zAdd('leaderboard', { score: latency, value: job.team });
            } else {
                console.error(`[Worker ERROR] latency.txt was not generated!`);
            }
        } else {
            console.log(`[Worker] Correctness FAILED:\n${stdout}`);
            // Will save the failure reason back to Redis so the frontend can display it
        }
    } catch (err) {
        console.error(`[Worker] Job Failed!`, err.message);
    } finally {
        // Clean up the compiled executable and the user's uploaded source file
        if (fs.existsSync(engineExe)) fs.unlinkSync(engineExe);
        if (fs.existsSync(job.filepath)) fs.unlinkSync(job.filepath);
    }
}

// Orchestrator to ensure Bots run AFTER the engine starts
function runLoadTest(engineExe) {
    return new Promise((resolve, reject) => {
        const absoluteExePath = path.resolve(engineExe);
        const engineProcess = spawn(absoluteExePath, [], { cwd: WORK_DIR });

        // Print standard output from C++ and look for the "Awaiting Bot Fleet" message to know when to start the bots
        engineProcess.stdout.on('data', (data) => {
            const output = data.toString();
            console.log(`[C++] ${output.trim()}`); 
            if (output.includes("Awaiting Bot Fleet")) {
                console.log(`[Worker] Engine ready. Firing Bot Fleet...`);
                exec(path.resolve(`${CORE_DIR}/bot_fleet`), (err) => {
                    if (err) console.error("[Worker] Bot fleet error:", err);
                });
            }
        });

        // Print error output from C++
        engineProcess.stderr.on('data', (data) => {
            const output = data.toString();
            console.error(`[C++ ERROR] ${output.trim()}`);
        });

        engineProcess.on('close', (code) => {
            if (code === 0) {
                console.log(`[Worker] Engine shutdown cleanly. CSVs generated.`);
                resolve();
            } else {
                reject(new Error(`Engine crashed with exit code ${code}`));
            }
        });
    });
}

async function startWorker() {
    await redisClient.connect();
    console.log('[Worker] Connected to Redis. Waiting for submissions...');

    while (true) {
        try {

            // brPop blocks the loop until a job appears in 'hackathon:queue'
            const result = await redisClient.brPop('hackathon:queue', 0);
            const job = JSON.parse(result.element);
            
            // Because of the while loop, it processes one submission at a time
            // This prevents TCP port 8080 from colliding if two teams submit simultaneously
            // We will add containerization in the next upgrade to allow true parallel processing

            await processJob(job);
        } catch (err) {
            console.error('Redis pulling error:', err);
        }
    }
}

startWorker();