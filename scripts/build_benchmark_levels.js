const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const {
    BENCHMARK_BASELINE_OPS,
    BENCHMARK_COARSE_LEVELS,
    BENCHMARK_FINE_LEVELS,
    BENCHMARK_LEVELS_DIR,
    BENCHMARK_LEVEL_TOTAL_MS,
    levelFilePath,
    targetOpsForMultiplier,
} = require('../shared/benchmarkConfig');

const GENERATE_BINARY = path.join(__dirname, 'generate_nasdaq_level');
const ORDER_BYTES = 36;

function eachBenchmarkMultiplier(fn) {
    for (let i = 1; i <= 32; i++) {
        fn(String(i));
    }
}

function buildLevels({ missingOnly = false } = {}) {
    if (!fs.existsSync(GENERATE_BINARY)) {
        console.error(`Missing ${GENERATE_BINARY}. Run build_native.sh first.`);
        process.exit(1);
    }

    fs.mkdirSync(BENCHMARK_LEVELS_DIR, { recursive: true });

    const durationSec = Math.ceil(BENCHMARK_LEVEL_TOTAL_MS / 1000);
    const multipliers = [];
    eachBenchmarkMultiplier((mult) => multipliers.push(mult));
    let startSeq = 1;
    let generated = 0;
    let reused = 0;

    console.log(
        `[BuildLevels] ${missingOnly ? 'Updating missing' : 'Pregenerating'}`
        + ` ${multipliers.length} NASDAQ level binaries...`,
    );

    for (const mult of multipliers) {
        const outPath = levelFilePath(mult);

        if (missingOnly && fs.existsSync(outPath)) {
            const size = fs.statSync(outPath).size;
            if (size > 0 && size % ORDER_BYTES === 0) {
                startSeq += size / ORDER_BYTES;
                reused += 1;
                continue;
            }
        }

        console.log(`[BuildLevels] ${path.basename(outPath)} (${targetOpsForMultiplier(mult)} ops/s)...`);

        const result = spawnSync(GENERATE_BINARY, [
            outPath,
            String(mult),
            String(BENCHMARK_BASELINE_OPS),
            String(durationSec),
            String(startSeq),
        ], { stdio: 'inherit' });

        if (result.status !== 0)
            process.exit(result.status ?? 1);

        startSeq += fs.statSync(outPath).size / ORDER_BYTES;
        generated += 1;
    }

    console.log(`[BuildLevels] Done → ${BENCHMARK_LEVELS_DIR} (${generated} generated, ${reused} reused)`);
}

if (process.argv.includes('--check')) {
    const missing = [];
    eachBenchmarkMultiplier((mult) => {
        const filePath = levelFilePath(mult);
        if (!fs.existsSync(filePath)) {
            missing.push(mult);
            return;
        }
        const size = fs.statSync(filePath).size;
        if (size === 0 || size % ORDER_BYTES !== 0)
            missing.push(mult);
    });

    if (missing.length === 0) {
        let count = 0;
        eachBenchmarkMultiplier(() => { count += 1; });
        console.log(`[BuildLevels] OK — ${count} pregenerated levels`);
        process.exit(0);
    }
    console.error('[BuildLevels] missing or corrupt level binaries');
    for (const mult of missing)
        console.error(`  - ${path.basename(levelFilePath(mult))}`);
    process.exit(1);
}

const missingOnly = !process.argv.includes('--force');
buildLevels({ missingOnly });
