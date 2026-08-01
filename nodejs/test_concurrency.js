// libuv's worker thread pool defaults to 4, so 8 blocking native calls would run
// in two batches of 4 (watch the per-request ms: the last 4 wait for a free worker).
// Bump the pool to at least the concurrency BEFORE anything uses libuv, so all 8
// requestAsync() calls get their own worker and run truly in parallel.
process.env.UV_THREADPOOL_SIZE = process.env.UV_THREADPOOL_SIZE || '8';

const httpClient = require('./index.js');

const fs = require("fs");
const readline = require("readline");

async function readLinesWithoutComments(filePath) {
    const fileStream = fs.createReadStream(filePath);
    const rl = readline.createInterface({
        input: fileStream,
        crlfDelay: Infinity
    });

    const lines = [];
    for await (const line of rl) {
        const trimmed = line.trim();
        if (trimmed && !isCommentLine(trimmed)) {
            lines.push(line);
        }
    }
    return lines.join('\n');
}

function isCommentLine(line) {
    return line.startsWith('#') ||
        line.startsWith('//') ||
        line.startsWith('--') ||
        line.startsWith(';');
}

console.log('=== HTTP/2 Client — Concurrency Test ===\n');

let passed = 0;
let failed = 0;
const CONCURRENCY = 8;

// Initialize first
console.log('[Init] Initializing HTTP/2 client...');
httpClient.init();
console.log('[Init] ✓ Initialized\n');

async function runTests() {
    // Concurrent requests via Promise.all (HTTP/2 multiplexing).
    // The async path runs each blocking native call on a libuv worker thread, so
    // these fire in parallel; same-host requests share ONE multiplexed connection
    // and each takes its own stream (odd ids 1,3,5,...).
    console.log(`[Test] concurrent requests (async, multiplexed)...`);
    try {
        const concurrentStr = await readLinesWithoutComments("../bin/request_Concurrency.json");
        const config = JSON.parse(concurrentStr);
        const concurrency = config.concurrency || CONCURRENCY;
        delete config.concurrency; // test-only field; strip before the native call

        console.log(`  firing ${concurrency} requests to ${config.url}`);
        const startTime = Date.now();
        const results = await Promise.all(
            Array.from({ length: concurrency }, (_, i) => {
                const t0 = Date.now();
                return httpClient.requestAsync(config).then((res) => {
                    const parsed = JSON.parse(res);
                    return {
                        i,
                        ms: Date.now() - t0,
                        error: parsed.error && parsed.error.code ? parsed.error.code : null,
                        bytes: parsed.response && parsed.response.payload ? parsed.response.payload.length : 0,
                        streamId: parsed.session && parsed.session.streamId,
                    };
                });
            })
        );
        const totalMs = Date.now() - startTime;

        let ok = 0;
        const ids = [];
        for (const r of results) {
            if (r.error) {
                failed++;
                console.log(`  #${r.i} ✗ ${r.error} (${r.ms}ms)`);
            } else {
                ok++;
                passed++;
                ids.push(r.streamId);
                console.log(`  #${r.i} ✓ ${r.bytes} bytes, stream ${r.streamId} (${r.ms}ms)`);
            }
        }
        const distinct = [...new Set(ids)].sort((a, b) => a - b);
        console.log(`✓ ${ok}/${concurrency} succeeded, total wall time ${totalMs}ms`);
        console.log(`✓ distinct stream ids on the shared connection: ${distinct.join(', ')}`);
    } catch (error) {
        failed++;
        console.error(`✗ Concurrent test failed: ${error.message}`);
        console.error(error.stack);
    }

    console.log('\nCleaning up...');
    httpClient.cleanup();

    const total = passed + failed || 1;
    console.log('\n========================================');
    console.log(`Results: ${passed}/${total} passed, ${failed}/${total} failed`);
    if (failed > 0) {
        console.log('SOME TESTS FAILED');
        process.exit(1);
    }
    console.log('✓ All tests passed!\n');
}

runTests().catch(console.error);
