# HTTP Request Client

A high-performance HTTP/2 client library written in C, with TLS 1.3 session resumption, HPACK header compression, and Browser-like TLS fingerprinting.

## Features

- **HTTP/2** — full implementation: multiplexed streams, HPACK dynamic table, flow control, SETTINGS/GOAWAY
- **TLS 1.3** — session resumption with `pre_shared_key` (NewSessionTicket callback)
- **TLS fingerprint** — GREASE, ECH, ALPS, cert compression (Brotli), signature algorithms alignment
- **Compression** — gzip, deflate, Brotli, Zstd response decompression
- **Proxy** — HTTPS CONNECT tunnel with authorization
- **Session pool** — connection reuse with configurable expiration (up to 1024 concurrent sessions)
- **Cross-language** — native bindings for [Node.js](nodejs/) (N-API), [Python](python/) (cffi), [Java](java/) (JNI)

## Architecture

```
┌─────────────────────────────────────────────────┐
│                 Language Bindings               │
│     Node.js (N-API) │ Python (cffi) │ Java (JNI)│
├─────────────────────────────────────────────────┤
│             libhttp2client (shared lib)         │
├─────────────────────────────────────────────────┤
│ Http2Client → Basket → Session → RequestHandler │
│                        → ResponseHandler        │
│                        → SocketHandler          │
├─────────────────────────────────────────────────┤
│ SSLHandler (BoringSSL) │ CompressHandler        │
│ BrowserHandler (Chrome)│ UrlParser / File       │
├─────────────────────────────────────────────────┤
│ BoringSSL │ Brotli │ Zstd │ Jansson │ zlib      │
└─────────────────────────────────────────────────┘
```

## Project Structure

```
├── include/            # Public headers
│   ├── Http2Client.h   # C API entry point
│   ├── Basket.h        # Request/Response/Session data structures
│   ├── SSLHandler.h    # TLS layer
│   ├── Session.h       # Connection session pool
│   └── ...
├── src/                # C source
│   ├── Http2Client.c   # init / request / cleanup
│   ├── Session.c       # Session pool + TLS session cache
│   ├── SSLHandler.c    # Browser-like TLS configuration
│   ├── RequestHandler.c # HTTP/2 HEADERS + DATA frames
│   ├── ResponseHandler.c # Frame parsing, HPACK decoding
│   ├── CompressHandler.c # gzip/deflate/brotli/zstd
│   └── ...
├── tests/              # C test programs
├── third_party/        # Git submodules
│   ├── boringssl/      # TLS 1.3
│   ├── brotli/         # Brotli compression
│   ├── zstd/           # Zstandard compression
│   ├── jansson/        # JSON parsing
│   └── zlog/           # Logging (optional)
├── nodejs/             # Node.js N-API binding
├── python/             # Python cffi binding
└── java/               # Java JNI binding
```

## Prerequisites

- CMake >= 3.29
- C17 compiler (Clang / GCC)
- macOS or Linux

## Building

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

This produces:
- `lib/shared/libhttp2client.dylib` (macOS) / `.so` (Linux) — shared library for language bindings
- `test_GET` / `test_POST` — C test executables

### Build Output

| Artifact | Path |
|----------|------|
| Shared library | `lib/shared/libhttp2client.{dylib,so}` |
| Static library | `lib/static/libhttp2client.a` |
| Test executables | `bin/test_GET`, `bin/test_POST` |

## C API

```c
#include "Http2Client.h"

// 1. Initialize (call once at startup)
void initialiseEnv(void);

// 2. Send request
//    requestJSONString: JSON config (see format below)
//    basketJSONString: output buffer for response JSON
//    basketStrLen:     buffer size
//    Returns: actual response length, or <= 0 on error
int handleRequest(const char *requestJSONString,
                  char *basketJSONString,
                  size_t basketStrLen);

// 3. Cleanup (call once at shutdown)
void cleanupEnv(void);
```

### Example

```c
#include "Http2Client.h"
#include <stdlib.h>

int main() {
    initialiseEnv();

    const char *request = "{"\n"
        "  \"method\": \"GET\","\n"
        "  \"url\": \"https://tls.peet.ws/api/all\","\n"
        "  \"connectTimeoutInMilliseconds\": 3000,"\n"
        "  \"responseReadingTimeoutInMilliseconds\": 30000,"\n"
        "  \"decompress\": 0,"\n"
        "  \"log\": 1,"\n"
        "  \"headers\": {"\n"
        "    \"host\": \"tls.peet.ws\","\n"
        "    \"user-agent\": \"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36\","\n"
        "    \"sec-ch-ua\": \"\\\"Not:A-Brand\\\";v=\\\"99\\\", \\\"Google Chrome\\\";v=\\\"145\\\", \\\"Chromium\\\";v=\\\"145\\\"\","\n"
        "    \"sec-ch-ua-mobile\": \"?0\","\n"
        "    \"accept\": \"*/*\","\n"
        "    \"sec-fetch-site\": \"same-origin\","\n"
        "    \"sec-fetch-mode\": \"cors\","\n"
        "    \"sec-fetch-dest\": \"script\","\n"
        "    \"accept-encoding\": \"gzip, deflate, br, zstd\","\n"
        "    \"accept-language\": \"en-US,en;q=0.9\","\n"
        "    \"priority\": \"u=1\""\n"
        "  },"\n"
        "  \"proxy\": {"\n"
        "    \"scheme\": \"https\","\n"
        "    \"host\": \"127.0.0.1\","\n"
        "    \"port\": \"24801\","\n"
        "    \"authorization\": \"Basic dXNlcm5hbWU6cGFzc3dvcmQ=\""\n"
        "  },"\n"
        "  \"session\": { \"expirationInMilliseconds\": 300000 }"\n"
        "}";

    size_t bufLen = 1024 * 1024; // 1 MB
    char *buf = malloc(bufLen);

    int len = handleRequest(request, buf, bufLen);
    if (len > 0) {
        printf("%s\n", buf);
    }

    free(buf);
    cleanupEnv();
    return 0;
}
```

## Request JSON Format

| Field | Type | Description |
|-------|------|-------------|
| `url` | `string` | Target URL (required) |
| `method` | `string` | HTTP method: GET, POST |
| `headers` | `object` | Request headers |
| `payload` | `object` | Request body (for POST) |
| `connectTimeoutInMilliseconds` | `number` | TCP + TLS connect timeout |
| `responseReadingTimeoutInMilliseconds` | `number` | Response reading timeout |
| `decompress` | `number` | Decompression flags: 0 (none), 1 (gzip), 2 (deflate), 4 (br), 8 (zstd), or combinations (e.g. 15 = all) |
| `log` | `number` | Enable logging: 0 (off), 1 (on) |
| `proxy` | `object` | Proxy: `{ scheme, host, port, authorization? }` |
| `session` | `object` | Session: `{ expirationInMilliseconds }` |

## Testing

```bash
# C tests (from build directory)
./bin/test_GET
./bin/test_POST

# Node.js
cd nodejs && npm install && npm test

# Python
cd python && bash build.sh

# Java
cd java && bash build.sh
```

## Tech Stack

| Component | Library | Purpose |
|-----------|---------|---------|
| TLS 1.3 | [BoringSSL](https://github.com/google/boringssl) | TLS handshake, session resumption, cert compression |
| HTTP/2 | Custom implementation | Frames, HPACK, stream multiplexing, flow control |
| JSON | [Jansson](https://github.com/akheron/jansson) | Request/response JSON serialization |
| Brotli | [Brotli](https://github.com/google/brotli) | Response decompression + TLS cert compression |
| Zstd | [Zstd](https://github.com/facebook/zstd) | Response decompression |
| zlib | [zlib](https://www.zlib.net/) | gzip / deflate decompression |

## License

Apache-2.0
