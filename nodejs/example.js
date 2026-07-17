// nodejs/example.js
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


console.log('HTTP/2 Client Example\n');


(async () => {
    // Example 1: Simple GET request
    console.log('Example 1: GET request');
    try {
        let requestStr = await readLinesWithoutComments("../bin/request_GET.json");
        // console.log(requestStr);
        const result = httpClient.request(JSON.parse(requestStr));
        console.log(JSON.parse(result));
    } catch (error) {
        console.error('Error:', error.message);
    }

    // Example 2: POST request with JSON
    console.log('Example 2: POST request');
    try {
        let requestStr = await readLinesWithoutComments("../bin/request_POST.json");
        let requestJson = JSON.parse(requestStr);
        requestJson.headers['content-length'] = JSON.stringify(requestJson.payload).length + "";
        const result = httpClient.request(requestJson);
        console.log(JSON.parse(result));
    } catch (error) {
        console.error('Error:', error.message);
    }

    // Example 3: Custom timeouts
    console.log('Example 3: Custom timeouts');
    try {
        let requestStr = await readLinesWithoutComments("../bin/request_GET.json");
        let requestJson = JSON.parse(requestStr);
        requestJson.url = "https://httpbin.org/delay/10";
        requestJson.connectTimeoutInMilliseconds = 5000;
        requestJson.responseReadingTimeoutInMilliseconds = 5000;
        // console.log(requestStr);
        const result = httpClient.request(requestJson);
        console.log(JSON.parse(result));

    } catch (error) {
        console.error('Error:', error.message);
    }

    // Cleanup
    httpClient.cleanup();
    console.log('Done!');
})();
