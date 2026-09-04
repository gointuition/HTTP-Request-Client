const addon = require('./load-addon');

/**
 * Split a streaming callback object into the positional arguments the addon
 * takes. The callbacks are all or nothing: a missing onData would leave the
 * body with neither a consumer nor a place in the buffered response. With none
 * of them the addon keeps the library's own collector, so the response comes
 * back buffered.
 * @param {StreamCallbacks} [callbacks]
 * @returns {Array<Function|undefined>} onHeaders, onData, onComplete
 */
function splitCallbacks(callbacks) {
    const given = callbacks || {};
    const { onHeaders, onData, onComplete } = given;
    const provided = [onHeaders, onData, onComplete].filter(Boolean).length;
    if (provided !== 0 && provided !== 3) {
        throw new Error('onHeaders, onData and onComplete must be given together');
    }
    return [onHeaders, onData, onComplete];
}

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
     *
     * Synchronous: the event loop is blocked until the response is complete, so
     * streaming callbacks cannot be delivered here. Use requestAsync() or
     * requestNonBlocking() for a streamed response.
     *
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
     *
     * Passing callbacks streams the response: the decoded body is handed to
     * onData chunk by chunk on the JS thread, the resolved JSON then reports
     * "streamed": 1 with an empty payload. Without callbacks the library
     * collects the body itself and the JSON carries it in response.payload. The
     * bundle is all or nothing: a partial one rejects the request.
     *
     * @param {Object|string} config - Request configuration object or JSON string
     * @param {StreamCallbacks} [callbacks] - onHeaders / onData / onComplete
     * @returns {Promise<string>} Resolves with the response JSON string
     */
    async requestAsync(config, callbacks) {
        if (!this.initialized) {
            this.init();
        }

        // Convert object to JSON string if needed
        const jsonString = typeof config === 'string' ? config : JSON.stringify(config);

        // Call native addon (returns a Promise resolving to { data })
        return addon.requestAsync(jsonString, ...splitCallbacks(callbacks)).then((result) => result.data);
    }

    /**
     * Send an HTTP request in non-blocking mode. The native call starts the
     * request (send HEADERS/DATA) and returns immediately with a request id; the
     * response is then polled on a timer, so the event loop never blocks and
     * UV_THREADPOOL_SIZE is irrelevant. Non-blocking mode is forced by this
     * wrapper (the C core selects non-blocking socket I/O).
     * @param {Object|string} config - Request configuration
     * @param {number} [pollIntervalMs=5] - Poll interval in milliseconds
     * @param {StreamCallbacks} [callbacks] - onHeaders / onData / onComplete, all
     *                                        three or none
     * @returns {Promise<string>} Resolves with the response JSON string
     */
    requestNonBlocking(config, pollIntervalMs = 5, callbacks) {
        if (!this.initialized) {
            this.init();
        }

        const jsonString = typeof config === 'string' ? config : JSON.stringify(config);

        return new Promise((resolve, reject) => {
            let id;
            try {
                const started = addon.startRequest(jsonString, ...splitCallbacks(callbacks));
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
