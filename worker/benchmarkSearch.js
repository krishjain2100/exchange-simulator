const createLogger = require('../shared/logger');
const { executeBenchmarkProbe } = require('./benchmarkProbe');
const { patchJob } = require('../shared/redisStore');
const {
    BENCHMARK_COARSE_LEVELS,
    BENCHMARK_BORDERLINE_RETRIES,
} = require('./config');

const log = createLogger('Worker');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function formatProbeLog(prefix, result) {
    return (
        `[Worker] ${prefix} ${result.multiplier}x (${result.target_ops.toLocaleString()} ops/s):`
        + ` ${result.failed ? 'FAIL' : 'PASS'}`
        + ` | max_ops: ${result.max_ops.toLocaleString()} ops/s`
        + (result.fail_reason ? ` (${result.fail_reason})` : '')
    );
}

async function searchBenchmarkBrackets({ jobDir, port, host, waitForNextProbe, listener, workerClient, job_id, dockerProcess }) {
    const search = {
        lastPass: null,
        firstFail: null,
        peakPassResult: null,
    };

    const runProbe = async (multiplier, { retries = 1 } = {}) => {
        for (let attempt = 0; attempt < retries; attempt++) {

            if (workerClient && job_id) {
                const suffix = retries > 1 ? ` - attempt ${attempt + 1}` : '';
                await patchJob(workerClient, job_id, { status: `phase2_running (${multiplier}x${suffix})` })
                     .catch((err) => log.error(`[Worker] Failed to update live multiplier status: ${err.message}`));
            }
            const result = await executeBenchmarkProbe({
                port, host, jobDir, multiplier, listener, waitForNextProbe, dockerProcess,
            });
            if (!result.failed || attempt === retries - 1)
                return result;
        }
    };

    for (let i = 0; i < BENCHMARK_COARSE_LEVELS.length; i++) {
        const mult = BENCHMARK_COARSE_LEVELS[i];
        const result = await runProbe(mult, { retries: BENCHMARK_BORDERLINE_RETRIES });
        log.info(formatProbeLog('Coarse', result));

        if (result.failed) {
            search.firstFail = mult;
            search.firstFailReason = result.fail_reason;
            break;
        }

        search.lastPass = mult;
        search.peakPassResult = result;


    }

    if (search.lastPass == null) {
        throw new Error(`Engine failed at minimum multiplier ${BENCHMARK_COARSE_LEVELS[0]}×`);
    }

    if (search.firstFail != null && Number(search.firstFail) > Number(search.lastPass)) {
        const fineLevels = [];
        const start = Number(search.lastPass);
        const end = Number(search.firstFail);
        for (let val = start + 1; val < end; val++) {
            fineLevels.push(String(val));
        }



        for (let i = 0; i < fineLevels.length; i++) {
            const mult = fineLevels[i];
            const result = await runProbe(mult, { retries: BENCHMARK_BORDERLINE_RETRIES });
            log.info(formatProbeLog('Fine', result));

            if (result.failed) {
                search.firstFailReason = result.fail_reason;
                break;
            }

            search.peakPassResult = result;


        }
    }

    if (search.peakPassResult && search.firstFailReason) {
        search.peakPassResult.shutdown_reason = search.firstFailReason;
    }
    return search.peakPassResult;
}

module.exports = { searchBenchmarkBrackets };
