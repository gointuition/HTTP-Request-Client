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
    // Test 1: Single request
    console.log('[Test 1] Single request...');
    try {
        const startTime = Date.now();

        let requestStr = await readLinesWithoutComments("../bin/request_GET.json");
        let result = httpClient.request(JSON.parse(requestStr));

        result = httpClient.request(JSON.parse(requestStr));

        const endTime = Date.now();

        console.log(`✓ Completed in ${endTime - startTime}ms`);
        console.log(`✓ URL: ${result.url || 'N/A'}`);

        console.log(JSON.parse(result));
    } catch (error) {
        console.error(`✗ Failed: ${error.message}`);
        console.error(error.stack);
    }

    console.log('\nCleaning up...');
    httpClient.cleanup();
    console.log('✓ All tests completed!\n');
}

runTests().catch(console.error);


