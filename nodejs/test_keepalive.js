//
// Node.js keep-alive (long connection) verification.
//
// Mirrors the C-level test_SessionExpiry probe: session.streamId is a
// per-connection counter that starts at 1 and grows by 2 (1, 3, 5, ...).
//   - reusing a pooled (keep-alive) connection -> streamId keeps incrementing
//   - a brand new connection                   -> streamId resets to 1
//
// The session pool lives in the C library and is process-global, so issuing
// several httpClient.request() calls from one Node process exercises real
// connection reuse across the JS <-> native boundary.
//
// Scenarios:
//   1. sequential reuse  : N back-to-back requests must share ONE connection
//                          (streamId 1, 3, 5, ...).
//   2. idle reaping      : after idling past expirationInMilliseconds the next
//                          request opens a fresh connection (streamId back to 1).
//   3. concurrent burst  : Promise.all of requestAsync (informational) — on the
//                          serial HTTP/1.1 transport each in-flight request gets
//                          its own connection, so the pool grows instead of one
//                          connection carrying them all.
//

const httpClient = require('./index.js');

const EXPIRATION_MS = 4000;
const SEQUENTIAL = 3;

function http11Config() {
    return {
        method: 'GET',
        url: 'https://www.cloudflare.com/cdn-cgi/trace',
        connectTimeoutInMilliseconds: 3000,
        responseReadingTimeoutInMilliseconds: 30000,
        decompress: 15,
        log: 0,
        headers: {
            host: 'www.cloudflare.com',
            'Connection': 'keep-alive',
            'user-agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36',
            accept: '*/*',
            'accept-language': 'en-US,en;q=0.9'
        },
        session: {
            protocol: 'http/1.1',        // force the serial HTTP/1.1 transport
            clientHelloId: 'hellochrome_auto',
            expirationInMilliseconds: EXPIRATION_MS
        }
    };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Pull the per-connection streamId out of a response, or throw on error.
function streamIdOf(result) {
    const parsed = typeof result === 'string' ? JSON.parse(result) : result;
    if (parsed.error && parsed.error.code) {
        throw new Error(`request error ${parsed.error.code}: ${parsed.error.message}`);
    }
    return parsed.session && parsed.session.streamId;
}

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log(`  ✓ ${msg}`); }
    else { failed++; console.log(`  ✗ ${msg}`); }
}

async function main() {
    console.log('=== Node.js HTTP/1.1 keep-alive test ===\n');
    httpClient.init();
    const config = http11Config();

    // 1. sequential reuse: N requests on ONE connection -> 1, 3, 5, ...
    console.log(`[1] sequential reuse over ${SEQUENTIAL} requests (expect streamId 1,3,5)`);
    const seqIds = [];
    try {
        for (let i = 0; i < SEQUENTIAL; i++) {
            const id = streamIdOf(httpClient.request(config));
            seqIds.push(id);
            console.log(`  request #${i + 1}: streamId=${id}`);
        }
        const expected = Array.from({ length: SEQUENTIAL }, (_, i) => 1 + i * 2);
        check(JSON.stringify(seqIds) === JSON.stringify(expected),
            `sequential requests reused one connection (got [${seqIds}], expected [${expected}])`);
    } catch (e) {
        check(false, `sequential phase threw: ${e.message}`);
    }

    // 2. idle reaping: idle past the expiration window -> fresh connection (streamId 1)
    console.log(`\n[2] idle ${Math.round(EXPIRATION_MS / 1000) + 2}s (> ${EXPIRATION_MS}ms window), expect a NEW connection`);
    await sleep(EXPIRATION_MS + 2000);
    try {
        const id = streamIdOf(httpClient.request(config));
        console.log(`  request after idle: streamId=${id}`);
        check(id === 1, `expired session was reaped, connection re-established (got streamId=${id}, expected 1)`);
    } catch (e) {
        check(false, `idle phase threw: ${e.message}`);
    }

    // 3. concurrent burst (informational)
    const CONC = 4;
    console.log(`\n[3] concurrent burst of ${CONC} requestAsync (informational)`);
    try {
        const results = await Promise.all(
            Array.from({ length: CONC }, () => httpClient.requestAsync(config))
        );
        const ids = results.map(streamIdOf);
        const newConnections = ids.filter((x) => x === 1).length;
        console.log(`  streamIds=[${ids}] -> ${newConnections} fresh connection(s), ` +
            `${ids.length - newConnections} reused`);
        console.log('  note: on the serial HTTP/1.1 transport, concurrent in-flight requests');
        console.log('        each take their own connection (pool grows); reuse applies to idle windows.');
        check(ids.length === CONC, `all ${CONC} concurrent requests completed`);
    } catch (e) {
        check(false, `concurrent phase threw: ${e.message}`);
    }

    httpClient.cleanup();

    console.log('\n========================================');
    console.log(`Results: ${passed} passed, ${failed} failed`);
    if (failed > 0) {
        console.log('SOME TESTS FAILED');
        process.exit(1);
    }
    console.log('✓ keep-alive behaves as expected\n');
}

main().catch((e) => {
    console.error(e);
    try { httpClient.cleanup(); } catch (_) {}
    process.exit(1);
});
