// Streaming response test. Mirrors tests/test_Streaming.c and
// python/test_streaming.py: the decoded body is handed to JS callbacks chunk by
// chunk instead of being buffered, and every case asserts both what the callbacks
// saw and what the response JSON reports ("streamed": 1, empty payload).
//
//   1. request_Streaming.json over HTTP/2, whole body delivered chunk by chunk
//   2. same, stopped by onData after 64 KB
//   3. same over HTTP/1.1
//   4. same through the non-blocking surface (start + poll)
//   5. request_Streaming_gzip.json: gzip body decoded incrementally
//   6. control: no callbacks at all keeps the buffered response intact
//   7. half a contract is refused before the request goes out
//
// The callbacks are posted to the JS thread independently of the Promise, so a
// case waits for onComplete before judging instead of trusting the resolve order.

const httpClient = require('./index.js');

const fs = require('fs');
const path = require('path');

const BIN_DIR = path.join(__dirname, '..', 'bin');
const ABORT_AFTER_BYTES = 64 * 1024;
const ABORTED_BY_CONSUMER_CODE = '3-0014';

// label, request file, transport, bytes before onData stops, expected body prefix
const CASES = [
    ['requestAsync, http/2, whole body', 'request_Streaming.json', 'http2', 0, null],
    ['requestAsync, http/2, stopped by onData', 'request_Streaming.json', 'http2', ABORT_AFTER_BYTES, null],
    ['requestAsync, http/1.1, whole body', 'request_Streaming.json', 'http11', 0, null],
    ['requestNonBlocking, callbacks on the reader thread', 'request_Streaming.json', 'async', 0, null],
    ['gzip body decoded incrementally', 'request_Streaming_gzip.json', 'http2', 0, '{'],
];

function isCommentLine(line) {
    return line.startsWith('#') || line.startsWith('//') ||
        line.startsWith('--') || line.startsWith(';');
}

/** Load a request file (comments stripped) and rewrite its transport knobs. */
function readConfig(fileName, transport) {
    const text = fs.readFileSync(path.join(BIN_DIR, fileName), 'utf8');
    const stripped = text.split('\n')
        .filter((line) => !isCommentLine(line.trim()))
        .join('\n');
    const config = JSON.parse(stripped);

    if (transport === 'http11') {
        config.session = config.session || {};
        config.session.protocol = 'http/1.1';
    }
    return config;
}

/** What the callbacks saw; onData stops the response once `abortAfter` is met. */
class Recorder {
    constructor(abortAfter) {
        this.headers = null;
        this.error = 'unset';
        this.completes = 0;
        this.bytes = 0;
        this.chunks = 0;
        this.head = Buffer.alloc(0);
        this.abortAfter = abortAfter || 0;
        this.completed = new Promise((resolve) => {
            this.markCompleted = resolve;
        });
    }

    get callbacks() {
        return {
            onHeaders: (headers) => {
                this.headers = headers;
            },
            onData: (chunk) => {
                this.bytes += chunk.length;
                this.chunks += 1;
                if (this.head.length < 8) {
                    this.head = Buffer.concat([this.head, chunk.subarray(0, 8 - this.head.length)]);
                }
                // returning true asks the library to tear the stream down, which
                // the next chunk honours
                return this.abortAfter > 0 && this.bytes >= this.abortAfter;
            },
            onComplete: (error) => {
                this.completes += 1;
                this.error = error;
                this.markCompleted();
            },
        };
    }

    /** Resolves on onComplete, or after the timeout when no callback ever came. */
    whenComplete(timeoutMs = 15000) {
        return Promise.race([
            this.completed,
            new Promise((resolve) => setTimeout(resolve, timeoutMs)),
        ]);
    }
}

/** Compare what the callbacks saw with what the response JSON reports. */
function validate(recorder, resultJson, abortAfter, expectedPrefix) {
    const problems = [];
    const aborted = abortAfter > 0;
    const result = resultJson ? JSON.parse(resultJson) : {};
    const response = result.response || {};
    const errorCode = (result.error || {}).code;
    const headers = recorder.headers || {};

    if (response.streamed !== 1) {
        problems.push(`response.streamed is ${response.streamed}, expected 1`);
    }
    if (response.payload !== '') {
        problems.push(`response.payload must be empty, got ${(response.payload || '').length} bytes`);
    }
    if (aborted && errorCode !== ABORTED_BY_CONSUMER_CODE) {
        problems.push(`error.code is ${errorCode}, expected ${ABORTED_BY_CONSUMER_CODE}`);
    }
    if (!aborted && errorCode) {
        problems.push(`unexpected error.code ${errorCode}`);
    }

    if (recorder.completes !== 1) {
        problems.push(`onComplete ran ${recorder.completes} times, expected 1`);
    }
    if (recorder.bytes === 0) {
        problems.push('no body byte delivered');
    }
    if (aborted && recorder.bytes < abortAfter) {
        problems.push(`stopped after ${recorder.bytes} bytes, expected at least ${abortAfter}`);
    }

    const completedCode = recorder.error ? recorder.error.code : undefined;
    if (!aborted && recorder.error !== null) {
        problems.push(`onComplete reported ${JSON.stringify(recorder.error)}`);
    }
    if (aborted && completedCode && completedCode !== ABORTED_BY_CONSUMER_CODE) {
        problems.push(`onComplete reported ${completedCode}, expected ${ABORTED_BY_CONSUMER_CODE}`);
    }

    if (!aborted && headers[':status'] !== '200') {
        problems.push(`:status is ${headers[':status']}, expected 200`);
    }

    // an uncompressed body must arrive complete
    const contentLength = headers['content-length'];
    const encoded = (headers['content-encoding'] || 'identity').toLowerCase() !== 'identity';
    if (!aborted && !encoded && contentLength && Number(recorder.bytes) !== Number(contentLength)) {
        problems.push(`delivered ${recorder.bytes} of ${contentLength} advertised bytes`);
    }

    if (expectedPrefix !== null && !recorder.head.toString('utf8').startsWith(expectedPrefix)) {
        problems.push(`body starts with ${recorder.head.toString('utf8')}, expected ${expectedPrefix}`);
    }
    return problems;
}

/** Print one case verdict. */
function report(label, problems, byteCount, chunkCount) {
    if (problems.length > 0) {
        console.log(`    FAILED [${label}]`);
        problems.forEach((problem) => console.log(`      - ${problem}`));
        return false;
    }
    console.log(`    passed [${label}]: ${byteCount} bytes in ${chunkCount} chunks`);
    return true;
}

async function runCase(label, fileName, transport, abortAfter, expectedPrefix) {
    const config = readConfig(fileName, transport);
    const recorder = new Recorder(abortAfter);

    const resultJson = transport === 'async'
        ? await httpClient.requestNonBlocking(config, 5, recorder.callbacks)
        : await httpClient.requestAsync(config, recorder.callbacks);
    await recorder.whenComplete();

    return report(label, validate(recorder, resultJson, abortAfter, expectedPrefix),
        recorder.bytes, recorder.chunks);
}

/** Control: without callbacks the response stays a complete buffered one. */
async function runBufferedCase() {
    const label = 'no callbacks, buffered control';
    const config = readConfig('request_Streaming.json', 'http2');
    const result = JSON.parse(await httpClient.requestAsync(config));
    const response = result.response || {};
    const payload = response.payload || '';

    const problems = [];
    if (response.streamed !== 0) {
        problems.push(`response.streamed is ${response.streamed}, expected 0`);
    }
    if (payload.length === 0) {
        problems.push('response.payload is empty');
    }
    return report(label, problems, payload.length, 1);
}

/** Half a contract is refused up front: a missing onData would leave the body
 *  with neither a consumer nor a place in the buffered response. */
async function runPartialContractCase() {
    const label = 'partial callbacks refused';
    const config = readConfig('request_Streaming.json', 'http2');

    const partials = [
        { onHeaders: () => {} },
        { onData: () => false },
        { onComplete: () => {} },
    ];

    const problems = [];
    for (const callbacks of partials) {
        const given = Object.keys(callbacks).join('/');
        try {
            await httpClient.requestAsync(config, callbacks);
            problems.push(`${given} was accepted`);
        } catch (e) {
            if (!/must be given together/.test(e.message)) {
                problems.push(`${given} threw ${e.message}`);
            }
        }
    }
    return report(label, problems, 0, 0);
}

async function main() {
    console.log('=== HTTP Client — Streaming Response Test ===\n');

    console.log('[Init] Initializing HTTP client...');
    httpClient.init();
    console.log('[Init] Initialized\n');

    let passed = 0;
    let failed = 0;

    for (const [label, fileName, transport, abortAfter, expectedPrefix] of CASES) {
        let ok;
        try {
            ok = await runCase(label, fileName, transport, abortAfter, expectedPrefix);
        } catch (e) {
            console.log(`    FAILED [${label}]: ${e.message}`);
            ok = false;
        }
        ok ? passed++ : failed++;
    }

    try {
        await runBufferedCase() ? passed++ : failed++;
    } catch (e) {
        console.log(`    FAILED [no callbacks, buffered control]: ${e.message}`);
        failed++;
    }

    try {
        await runPartialContractCase() ? passed++ : failed++;
    } catch (e) {
        console.log(`    FAILED [partial callbacks refused]: ${e.message}`);
        failed++;
    }

    console.log('\nCleaning up...');
    httpClient.cleanup();

    const total = passed + failed;
    console.log(`\n${'='.repeat(40)}`);
    console.log(`Results: ${passed}/${total} passed, ${failed}/${total} failed`);
    if (failed > 0) {
        console.log('SOME TESTS FAILED');
        process.exit(1);
    }
    console.log('All tests passed!');
}

main();
