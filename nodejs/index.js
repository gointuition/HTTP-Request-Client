const addon = require('./load-addon');

class HttpClient {
    constructor() {
        this.initialized = false;
    }

    /**
     * Initialize the HTTP client environment
     * @returns {HttpClient} this
     */
    init() {
        if (!this.initialized) {
            addon.initEnv();
            this.initialized = true;
        }
        return this;
    }

    /**
     * Send an HTTP request
     * @param {Object|string} config - Request configuration object or JSON string
     * @returns {Object} Parsed response object
     * @throws {Error} If request fails
     */
    request(config) {
        if (!this.initialized) {
            this.init();
        }

        // Convert object to JSON string if needed
        const jsonString = typeof config === 'string' ? config : JSON.stringify(config);

        // Call native addon
        const result = addon.handleRequest(jsonString);

        return result.data;
    }

    /**
     * Send an HTTP request asynchronously. The blocking native call runs on a
     * libuv worker thread, so multiple pending requests execute concurrently
     * (same-host requests are multiplexed over one HTTP connection).
     * @param {Object|string} config - Request configuration object or JSON string
     * @returns {Promise<string>} Resolves with the response JSON string
     */
    requestAsync(config) {
        if (!this.initialized) {
            this.init();
        }

        // Convert object to JSON string if needed
        const jsonString = typeof config === 'string' ? config : JSON.stringify(config);

        // Call native addon (returns a Promise resolving to { data })
        return addon.requestAsync(jsonString).then((result) => result.data);
    }

    /**
     * Send an HTTP request in non-blocking mode. The native call starts the
     * request (send HEADERS/DATA) and returns immediately with a request id; the
     * response is then polled on a timer, so the event loop never blocks and
     * UV_THREADPOOL_SIZE is irrelevant. The request config must carry
     * "non-blocking": 1 (the C core selects non-blocking socket I/O).
     * @param {Object|string} config - Request configuration (should set "non-blocking": 1)
     * @param {number} [pollIntervalMs=5] - Poll interval in milliseconds
     * @returns {Promise<string>} Resolves with the response JSON string
     */
    requestNonBlocking(config, pollIntervalMs = 5) {
        if (!this.initialized) {
            this.init();
        }

        const jsonString = typeof config === 'string' ? config : JSON.stringify(config);

        return new Promise((resolve, reject) => {
            let id;
            try {
                const started = addon.startRequest(jsonString);
                id = started.id;
            } catch (e) {
                reject(e);
                return;
            }

            if (!id) {
                reject(new Error('Failed to start non-blocking request'));
                return;
            }

            const poll = () => {
                let res;
                try {
                    res = addon.pollRequest(id);
                } catch (e) {
                    reject(e);
                    return;
                }
                if (res.status === 0) {
                    setTimeout(poll, pollIntervalMs);
                } else if (res.status === 1) {
                    resolve(res.data);
                } else {
                    // status === -1: completed with error, still carry the JSON.
                    resolve(res.data);
                }
            };

            // Kick off the first poll on the next tick so the caller can attach.
            setTimeout(poll, 0);
        });
    }

    /**
     * Cleanup resources
     */
    cleanup() {
        if (this.initialized) {
            addon.cleanupEnv();
            this.initialized = false;
        }
    }
}

// Create singleton instance WITHOUT auto-initialization
const client = new HttpClient();

// Export both singleton and class
module.exports = client;
module.exports.HttpClient = HttpClient;
module.exports.default = client;
