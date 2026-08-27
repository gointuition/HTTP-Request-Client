const httpClient = require('./index.js');

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

console.log('=== HTTP Client Test ===\n');

let passed = 0;
let failed = 0;

// Initialize first
console.log('[Init] Initializing HTTP client...');
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

        console.log(parsed.response.payload)

        const endTime = Date.now();

        // Validate: error must be empty, status 2xx, payload non-empty
        if (parsed.error && parsed.error.code) {
            console.log(`✗ FAILED: error code ${parsed.error.code}`);
            failed++;
        } else if (!parsed.response || !parsed.response.payload) {
            console.log(`✗ FAILED: response.payload is missing or empty`);
            failed++;
        } else {
            passed++;
            console.log(`✓ PASSED in ${endTime - startTime}ms`);
            console.log(`✓ URL: ${parsed.url || 'N/A'}`);
            console.log(`✓ payload bytes: ${parsed.response.payload.length}`);
        }
    } catch (error) {
        failed++;
        console.error(`✗ Failed: ${error.message}`);
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


