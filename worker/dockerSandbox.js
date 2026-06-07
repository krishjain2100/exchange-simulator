const { spawn } = require('child_process');
const getPort = require('get-port');
const createLogger = require('../shared/logger');
const { waitForEngineReady } = require('./engine');
const { startPhase1Load, startPhase2Load, stopLoadTimers } = require('./phaseLoad');
const { CORE_DIR, DOCKER_HARD_LIMIT_MS, DOCKER_MAX_MEMORY_MB, DOCKER_MAX_CPUS, PHASE1_LOAD_FAILURE_MSG, PHASE2_LOAD_FAILURE_MSG } = require('./config');


const log = createLogger('Worker');

function extractDockerFailureMessage(code, signal, dockerStderr, forcedReason) {
    if (forcedReason) return forcedReason;

    if (signal === 'SIGKILL') {
        return `Docker process killed after exceeding ${DOCKER_HARD_LIMIT_MS / 1000}s hard limit`;
    }

    const lines = dockerStderr.split('\n').map((line) => line.trim()).filter(Boolean);
    const dockerError = lines.find((line) => line.includes('[Docker ERROR]'));
    if (dockerError) return dockerError.slice(0, 2000);

    const compilerError = lines.find((line) => /error:/i.test(line));
    if (compilerError) return compilerError.slice(0, 2000);

    const tail = lines.slice(-6).join(' | ');
    return tail || `Docker exited with code ${code}`;
}

function forceStopContainer(containerName, dockerProcess) {
    spawn('docker', ['kill', containerName], { stdio: 'ignore' }).on('error', () => {});
    if (dockerProcess && !dockerProcess.killed) {
        dockerProcess.kill('SIGKILL');
    }
}

async function runDockerSandbox({ job_id, jobDir, workerClient, phase }) {
    const dynamicPort = await getPort();
    const containerName = `hft-${job_id}-${phase}`;
    let forcedStopReason = null;

    return new Promise((resolve, reject) => {
        const dockerProcess = spawn('docker', [
            'run', '--rm', '--name', containerName, '-p', `${dynamicPort}:8080`,
            `--cpus=${DOCKER_MAX_CPUS}`, `--memory=${DOCKER_MAX_MEMORY_MB}m`,
            '-e', `HFT_RUN_MODE=${phase}`,
            '-v', `${jobDir}:/sandbox`,
            '-v', `${CORE_DIR}:/core:ro`,
            'hft-sandbox',
        ], { timeout: DOCKER_HARD_LIMIT_MS, killSignal: 'SIGKILL' });

        let dockerCrashed = false;
        let dockerStderr = '';
        let loadTimers = null;

        dockerProcess.stdout.on('data', (data) => process.stdout.write(data.toString()));
        dockerProcess.stderr.on('data', (data) => {
            dockerStderr += data.toString();
            process.stderr.write(data.toString());
        });

        (async () => {
            try {
                await waitForEngineReady(dockerProcess, dynamicPort);
                if (dockerCrashed) return;

                loadTimers = phase === 'correctness'
                    ? await startPhase1Load(workerClient, job_id, dynamicPort, {
                        onForceStop: () => {
                            forcedStopReason = PHASE1_LOAD_FAILURE_MSG;
                            forceStopContainer(containerName, dockerProcess);
                        },
                    })
                    : await startPhase2Load(dynamicPort, {
                        onForceStop: () => {
                            forcedStopReason = PHASE2_LOAD_FAILURE_MSG;
                            forceStopContainer(containerName, dockerProcess);
                        },
                    });
            } catch (err) {
                dockerCrashed = true;
                stopLoadTimers(loadTimers);
                reject(err);
            }
        })();

        dockerProcess.on('error', (err) => {
            dockerCrashed = true;
            reject(err);
        });

        dockerProcess.on('close', (code, signal) => {
            stopLoadTimers(loadTimers);
            if (code === 0) resolve();
            else reject(new Error(extractDockerFailureMessage(code, signal, dockerStderr, forcedStopReason)));
        });
    });
}

module.exports = { runDockerSandbox };
