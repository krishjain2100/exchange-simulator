function createLogger(tag) {
    const prefix = `[${tag}]`;
    return {
        info: (...args) => console.log(`[${new Date().toISOString()}] ${prefix} INFO:`, ...args),
        warn: (...args) => console.warn(`[${new Date().toISOString()}] ${prefix} WARN:`, ...args),
        error: (...args) => console.error(`[${new Date().toISOString()}] ${prefix} ERROR:`, ...args),
    };
}

module.exports = createLogger;
