// libuv's worker thread pool defaults to 4, so 8 blocking native calls would run
// in two batches of 4 (watch the per-request ms: the last 4 wait for a free worker).
// Bump the pool to at least the concurrency BEFORE anything uses libuv, so all 8
// requestAsync() calls get their own worker and run truly in parallel.
// Equivalent to launching with: UV_THREADPOOL_SIZE=8 node test.js
process.env.UV_THREADPOOL_SIZE = process.env.UV_THREADPOOL_SIZE || '8';

const httpClient = require('./index.js');
// const httpClient = require('');

const fs = require("fs")
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

console.log('=== HTTP/2 Client Test ===\n');

// Initialize first
console.log('[Init] Initializing HTTP/2 client...');
httpClient.init();
console.log('[Init] ✓ Initialized\n');

async function runTests() {
    // Test 1: Single request (regression for the synchronous path)
    console.log('[Test 1] Single request (sync)...');
    try {
        const startTime = Date.now();

        const requestStr = await readLinesWithoutComments("../bin/request_GET.json");
        const result = httpClient.request(JSON.parse(requestStr));
        const parsed = JSON.parse(result);

        const endTime = Date.now();
        console.log(`✓ Completed in ${endTime - startTime}ms`);
        console.log(`✓ URL: ${parsed.url || 'N/A'}`);
        console.log(`✓ payload bytes: ${parsed.response && parsed.response.payload ? parsed.response.payload.length : 0}`);
        if (parsed.error && parsed.error.code) {
            console.log(`✗ error: ${parsed.error.code} ${parsed.error.message || ''}`);
        }
    } catch (error) {
        console.error(`✗ Failed: ${error.message}`);
        console.error(error.stack);
    }

    // Test 2: Concurrent requests via Promise.all (HTTP/2 multiplexing).
    // The async path runs each blocking native call on a libuv worker thread, so
    // these fire in parallel; same-host requests share ONE multiplexed connection
    // and each takes its own stream (odd ids 1,3,5,...). Cloudflare keeps the
    // connection open for many streams, so it demonstrates real multiplexing
    // (unlike tls.peet.ws, which sends GOAWAY after a single stream).
    const CONCURRENCY = 8;
    console.log(`\n[Test 2] concurrent requests (async, multiplexed)...`);
    try {
        const concurrentStr = await readLinesWithoutComments("../tests/test_Concurrency.json");
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
                console.log(`  #${r.i} ✗ ${r.error} (${r.ms}ms)`);
            } else {
                ok++;
                ids.push(r.streamId);
                console.log(`  #${r.i} ✓ ${r.bytes} bytes, stream ${r.streamId} (${r.ms}ms)`);
            }
        }
        const distinct = [...new Set(ids)].sort((a, b) => a - b);
        console.log(`✓ ${ok}/${concurrency} succeeded, total wall time ${totalMs}ms`);
        console.log(`✓ distinct stream ids on the shared connection: ${distinct.join(', ')}`);
    } catch (error) {
        console.error(`✗ Concurrent test failed: ${error.message}`);
        console.error(error.stack);
    }

    console.log('\nCleaning up...');
    httpClient.cleanup();
    console.log('✓ All tests completed!\n');
}

runTests().catch(console.error);


