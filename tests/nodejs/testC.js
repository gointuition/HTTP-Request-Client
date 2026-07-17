const koffi = require("koffi");

const lib = koffi.load("../../lib/shared/libhttp2client.dylib")

const initialiseEnv     = lib.func("void initialiseEnv(void)");
const cleanupEnv        = lib.func("void cleanupEnv(void)");
const handleRequest     = lib.func("int handleRequest(const char *requestJSONString, char *basketJSONString, size_t basketStrLen)");
// const handleRequest     = lib.func("void* handleRequest(const char *requestJSONString)");

const requestJSON = {
  "method": "GET",
  "url": "https://www.wizzair.com/149e9513-01fa-4fb0-aad4-566afd725d1b/2d206a39-8ed7-437e-a3be-862e0f06eea3/fp?x-kpsdk-v=j-1.2.490",
  "connectTimeoutInMilliseconds": 3000,
  "responseReadingTimeoutInMilliseconds": 30000,
  "decompress": 1,
  "headers": {
    "host": "www.wizzair.com",
    "user-agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36",
    "sec-ch-ua": "\"Not:A-Brand\";v=\"99\", \"Google Chrome\";v=\"145\", \"Chromium\";v=\"145\"",
    "sec-ch-ua-mobile": "?0",
    "accept": "*/*",
    "sec-fetch-site": "same-origin",
    "sec-fetch-mode": "cors",
    "sec-fetch-dest": "script",
    "accept-encoding": "gzip, deflate, br, zstd",
    "accept-language": "en-US,en;q=0.9",
    "priority": "u=1"
  },
  "session": {
       "expirationInMilliseconds": 300000
  }
};

try {
    initialiseEnv();

    let basketStrLen = 3041;
    // let basketJSONString = new koffi.Buffer(basketStrLen);
    let basketJSONString = Buffer.alloc(basketStrLen);

    let actualLen = handleRequest(JSON.stringify(requestJSON), basketJSONString, basketStrLen);
    // actualLen + 1 <= basketStrLen
    if (actualLen < basketStrLen) {
        console.log(`[DEBUG] actual length ${actualLen}`)
        console.log(basketJSONString.toString('utf8', 0, actualLen));
    } else {
        console.log(`[Error]: buffer length is ${basketStrLen}, actual length is ${actualLen}`);
    }

    actualLen = handleRequest(JSON.stringify(requestJSON), basketJSONString, basketStrLen);
    if (actualLen < basketStrLen) {
        // console.log(basketJSONString.toString('utf8', 0, actualLen));
        console.log(JSON.parse(basketJSONString.toString('utf8', 0, actualLen)));
    } else {
        console.log(`[Error]: buffer length is ${basketStrLen}, actual length is ${actualLen}`);
    }

} catch (e) {
    console.log(e);
} finally {
    cleanupEnv();
}
