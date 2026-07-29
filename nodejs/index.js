const addon = require('./build/Release/http2addon.node');

class HttpClient {
    constructor() {
        this.initialized = false;
    }

    /**
     * Initialize the HTTP/2 client environment
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
     * Send an HTTP/2 request
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
     * Send an HTTP/2 request asynchronously. The blocking native call runs on a
     * libuv worker thread, so multiple pending requests execute concurrently
     * (same-host requests are multiplexed over one HTTP/2 connection).
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
