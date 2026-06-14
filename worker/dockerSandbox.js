const { spawn, execSync } = require('child_process');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function getContainerIP(containerName) {
    try {
        const ip = execSync(`docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' ${containerName}`)
            .toString()
            .trim();
        return ip;
    } catch (err) {
        return null;
    }
}
const getPort = require('get-port');
const createLogger = require('../shared/logger');
const { waitForEngineReady, waitForNextProbe, attachStdoutBuffer } = require('./engine');
const { startPhase1Load, stopLoadTimers } = require('./phaseLoad');
const { searchBenchmarkBrackets } = require('./benchmarkSearch');
const {
    CORE_DIR,
    DOCKER_HARD_LIMIT_MS,
    DOCKER_MAX_MEMORY_MB,
    DOCKER_MAX_CPUS,
    PHASE1_LOAD_FAILURE_MSG,
} = require('./config');

const log = createLogger('Worker');

function extractDockerFailureMessage(code, signal, dockerStderr, forcedReason) {
    if (forcedReason) return forcedReason;
    if (signal === 'SIGKILL') return 'Docker process killed after exceeding hard limit';

    const lines = dockerStderr.split('\n').map((line) => line.trim()).filter(Boolean);
    const dockerError = lines.find((line) => line.includes('[Docker ERROR]'));
    if (dockerError) return dockerError.slice(0, 2000);

    const compilerError = lines.find((line) => /error:/i.test(line));
    if (compilerError) return compilerError.slice(0, 2000);

    return lines.slice(-6).join(' | ') || `Docker exited with code ${code}`;
}

function removeContainer(containerName) {
    return new Promise((resolve) => {
        spawn('docker', ['rm', '-f', containerName], { stdio: 'ignore' })
            .on('close', () => resolve())
            .on('error', () => resolve());
    });
}

function forceStopContainer(containerName, dockerProcess) {
    spawn('docker', ['rm', '-f', containerName], { stdio: 'ignore' }).on('error', () => { });
    if (dockerProcess && !dockerProcess.killed)
        dockerProcess.kill('SIGKILL');
}

function translateToHostPath(containerPath) {
    const hostProjectPath = process.env.HOST_PROJECT_PATH;
    if (!hostProjectPath) return containerPath;
    if (containerPath.startsWith('/workspace')) {
        return containerPath.replace('/workspace', hostProjectPath);
    }
    return containerPath;
}

function buildDockerArgs({ containerName, port, jobDir, env }) {
    const envArgs = Object.entries(env).flatMap(([k, v]) => ['-e', `${k}=${v}`]);
    const hostJobDir = translateToHostPath(jobDir);
    const hostCoreDir = translateToHostPath(CORE_DIR);
    const args = [
        'run', '--rm', '--name', containerName,
        '-p', `${port}:8080`,
        `--cpus=${DOCKER_MAX_CPUS}`, `--memory=${DOCKER_MAX_MEMORY_MB}m`,
        ...envArgs,
        '-v', `${hostJobDir}:/sandbox`,
        '-v', `${hostCoreDir}:/core:ro`,
    ];
    if (process.env.DOCKER_NET) {
        args.push('--network', process.env.DOCKER_NET);
    }
    args.push('hft-sandbox');
    return args;
}

async function runContainer({
    containerName,
    jobDir,
    env,
    timeoutMs,
    timeoutReason,
    onReady,
    acceptCompletedAsSuccess = false,
}) {
    const port = await getPort();
    await removeContainer(containerName);

    return new Promise((resolve, reject) => {
        const dockerProcess = spawn('docker', buildDockerArgs({ containerName, port, jobDir, env }), {
            timeout: timeoutMs,
            killSignal: 'SIGKILL',
        });

        let settled = false;
        let stderr = '';
        let forcedReason = null;
        let loadCleanup = null;
        let completed = false;

        let targetHost = '127.0.0.1';
        let targetPort = port;

        const controls = {
            port: targetPort,
            host: targetHost,
            dockerProcess,
            setForcedReason: (reason) => { forcedReason = reason; },
            onCleanup: (fn) => { loadCleanup = fn; },
            forceStop: () => forceStopContainer(containerName, dockerProcess),
        };

        const hardTimer = setTimeout(() => {
            forcedReason = forcedReason || timeoutReason || `Container ${containerName} exceeded ${timeoutMs / 1000}s`;
            controls.forceStop();
        }, timeoutMs);

        const cleanup = () => {
            clearTimeout(hardTimer);
            if (loadCleanup) loadCleanup();
        };

        const settle = (fn) => {
            if (settled) return;
            settled = true;
            cleanup();
            fn();
        };

        attachStdoutBuffer(dockerProcess);
        dockerProcess.stderr.on('data', (d) => {
            stderr += d.toString();
        });

        (async () => {
            try {
                if (process.env.DOCKER_NET) {
                    let ip = null;
                    for (let i = 0; i < 50; i++) {
                        ip = getContainerIP(containerName);
                        if (ip) break;
                        await sleep(100);
                    }
                    if (!ip) throw new Error(`Failed to resolve container IP for ${containerName}`);
                    targetHost = ip;
                    targetPort = 8080;
                    controls.host = ip;
                    controls.port = 8080;
                }

                await waitForEngineReady(dockerProcess, targetPort, { host: targetHost });
                await onReady(controls);
                completed = true;
                if (acceptCompletedAsSuccess) {
                    settle(() => resolve());
                }
            } catch (err) {
                controls.forceStop();
                settle(() => reject(err));
            }
        })();

        dockerProcess.on('error', (err) => settle(() => reject(err)));

        dockerProcess.on('close', (code, signal) => {
            const ok = code === 0 || (acceptCompletedAsSuccess && completed);
            if (ok) {
                if (completed || !acceptCompletedAsSuccess) {
                    settle(() => resolve());
                }
            } else {
                settle(() => reject(new Error(extractDockerFailureMessage(code, signal, stderr, forcedReason))));
            }
        });
    });
}

async function driveCorrectness(controls, { workerClient, job_id }) {
    const timers = await startPhase1Load(workerClient, job_id, controls.port, {
        host: controls.host,
        onForceStop: () => {
            controls.setForcedReason(PHASE1_LOAD_FAILURE_MSG);
            controls.forceStop();
        },
    });
    controls.onCleanup(() => stopLoadTimers(timers));
}

async function driveBenchmark(controls, { workerClient, job_id, jobDir }) {
    const { port, host } = controls;
    const peakPassResult = await searchBenchmarkBrackets({
        jobDir,
        port,
        host,
        workerClient,
        job_id,
        dockerProcess: controls.dockerProcess,
        waitForNextProbe: (fromIndex) => waitForNextProbe(controls.dockerProcess, port, host, fromIndex),
    });
    controls.forceStop();
    return peakPassResult;
}

const PHASE_CONFIG = {
    correctness: {
        timeoutMs: DOCKER_HARD_LIMIT_MS,
        acceptCompletedAsSuccess: false,
        env: () => ({ HFT_RUN_MODE: 'correctness' }),
        drive: driveCorrectness,
    },
    benchmark: {
        timeoutMs: DOCKER_HARD_LIMIT_MS,
        acceptCompletedAsSuccess: true,
        env: () => ({
            HFT_RUN_MODE: 'benchmark',
        }),
        drive: driveBenchmark,
    },
};

async function runSandbox({ job_id, jobDir, phase, workerClient }) {
    const config = PHASE_CONFIG[phase];
    let peakPassResult;

    await runContainer({
        containerName: `hft-${job_id}-${phase}`,
        jobDir,
        env: config.env(),
        timeoutMs: config.timeoutMs,
        timeoutReason: `${phase} exceeded ${config.timeoutMs / 1000}s`,
        acceptCompletedAsSuccess: config.acceptCompletedAsSuccess,
        onReady: async (controls) => {
            peakPassResult = await config.drive(controls, { workerClient, job_id, jobDir });
        },
    });

    if (phase !== 'benchmark') return;

    return peakPassResult;
}

module.exports = { runSandbox };
