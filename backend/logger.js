function pad(value, width = 2) {
    return String(value).padStart(width, '0');
}

function timestamp(date = new Date()) {
    const year = date.getFullYear();
    const month = pad(date.getMonth() + 1);
    const day = pad(date.getDate());
    const hours = pad(date.getHours());
    const minutes = pad(date.getMinutes());
    const seconds = pad(date.getSeconds());
    const milliseconds = pad(date.getMilliseconds(), 3);

    return `${year}-${month}-${day} ${hours}:${minutes}:${seconds}.${milliseconds}`;
}

function createLogger(component = 'app') {
    return {
        info: (...args) => console.log(`[${timestamp()}] [${component}] INFO:`, ...args),
        warn: (...args) => console.warn(`[${timestamp()}] [${component}] WARN:`, ...args),
        error: (...args) => console.error(`[${timestamp()}] [${component}] ERROR:`, ...args),
        debug: (...args) => console.debug(`[${timestamp()}] [${component}] DEBUG:`, ...args)
    };
}

module.exports = createLogger;
