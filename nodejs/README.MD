# @http2/client

High-performance HTTP/2 client with native C implementation using N-API.

## Prerequisites

### 1. Build the C shared library

```bash
cd <project_root>
cmake -B build && cmake --build build -j$(nproc)
```

This produces `lib/shared/libhttp2client.{dylib,so,dll}`.

### 2. Install the addon

```bash
cd nodejs
npm install   # automatically runs build-addon.js
```

### Windows (MSVC + MinGW dual-toolchain)

On Windows the project uses a **split toolchain**:

| Component | Compiler | Output |
|-----------|----------|--------|
| C library | MinGW-w64 (MSYS2 MINGW64) | `libhttp2client.dll` |
| Node.js addon | MSVC (node-gyp default) | `http2addon.node` |

**Requirements:**

- [MSYS2](https://www.msys2.org/) with MINGW64 toolchain (`pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-tools-git`)
- Visual Studio 2022 with "Desktop development with C++" workload
- NASM, Go (for BoringSSL)
- Node.js >= 14, CMake >= 3.29

**Step 1 — Build the C library** (MSYS2 MINGW64 shell):

```bash
cd /e/Files/HTTP-Request-Client
cmake -B build -G "MinGW Makefiles"
cmake --build build -j4
```

**Step 2 — Build the addon** (any terminal — cmd / PowerShell / MSYS2):

```bash
cd nodejs
npm run build
```

`build-addon.js` automates the entire process:
1. Generates `http2client.lib` (MSVC import library) from `libhttp2client.dll` using `gendef` + `lib.exe`
2. Runs `node-gyp rebuild` (MSVC compiles the addon)
3. Copies runtime DLLs to `build/Release/`:
   - `libhttp2client.dll`
   - `libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll` (MinGW runtime)

> **Note:** zlib is statically linked into `libhttp2client.dll`, so no external zlib DLL is needed.

## Usage

```javascript
const httpClient = require('./index.js');

// Initialize
httpClient.init();

// Send a request
const result = httpClient.request({
    "method": "GET",
    "url": "https://tls.peet.ws/api/all",
    "connectTimeoutInMilliseconds": 3000,
    "responseReadingTimeoutInMilliseconds": 30000,
    "decompress": 0,
    "log": 1,
    "headers": {
        "host": "tls.peet.ws",
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
    "proxy": {
        "scheme": "https",
        "host": "127.0.0.1",
        "port": "24801",
        "authorization": "Basic dXNlcm5hbWU6cGFzc3dvcmQ="
    },
    "session": {
        "expirationInMilliseconds": 300000
    }
});

console.log(result);

// Cleanup when done
httpClient.cleanup();
```

## API

### `httpClient.init()`

Initialize the HTTP/2 client environment. Returns `this` for chaining.

### `httpClient.request(config)`

Send an HTTP/2 request.

**Parameters:**
- `config` (Object | string): Request configuration object or JSON string

**Returns:**
- `Object`: Parsed response data

**Throws:**
- `Error`: If request fails

### `httpClient.cleanup()`

Cleanup resources and release memory.

## Request Configuration

| Field | Type | Description |
|-------|------|-------------|
| `url` | `string` | Target URL (required) |
| `method` | `string` | HTTP method: GET, POST |
| `headers` | `Record<string, string>` | Request headers |
| `payload` | `object` | Request body (for POST) |
| `connectTimeoutInMilliseconds` | `number` | TCP + TLS connect timeout |
| `responseReadingTimeoutInMilliseconds` | `number` | Response reading timeout |
| `decompress` | `number` | Decompression flags: 0 (none), 1 (gzip), 2 (deflate), 4 (br), 8 (zstd), or combinations (e.g. 15 = all) |
| `log` | `number` | Enable logging: 0 (off), 1 (on) |
| `proxy` | `ProxyConfig` | Proxy settings |
| `session` | `SessionConfig` | Session settings |

### ProxyConfig

| Field | Type | Description |
|-------|------|-------------|
| `scheme` | `string` | Proxy scheme (e.g. `https`) |
| `host` | `string` | Proxy host |
| `port` | `string` | Proxy port |
| `authorization` | `string?` | Proxy auth header (e.g. `Basic ...`) |

### SessionConfig

| Field | Type | Description |
|-------|------|-------------|
| `expirationInMilliseconds` | `number` | Session expiration timeout |

## Response Structure

```typescript
interface HttpResult {
    url: string;
    method: string;
    request: {
        headers: string[];
        payload?: string;
    };
    response: {
        statusCode?: number;
        headers?: string[];
        payload?: string;
        contentEncoding?: string;
        payloadEncoding?: string;
        payloadSize?: number;
    };
    error: {
        code?: string;
        message?: string;
    };
    session: {
        creationTime?: number;
        streamId?: number;
        expirationInMilliseconds?: number;
    };
}
```

## Running Tests

```bash
# Quick test
node test.js

# Full example
node example.js

# Or use npm
npm test
```

## How It Works

```
Node.js (N-API)
  → ./build/Release/http2addon.node
    → C library (BoringSSL + HTTP/2)
      → HTTP/2 over TLS to server
```

- **No FFI overhead** — direct native binding via N-API
- **Direct memory access** — zero-copy buffer sharing between C and JS
- **Automatic buffer management** — V8 garbage collection handles JS-side cleanup
- **TLS 1.3 session resumption** — automatic `pre_shared_key` for subsequent connections to the same host

## Building from Source

```bash
npm run build
```

## TypeScript Support

Type definitions are included (`index.d.ts`):

```typescript
import httpClient, { HttpRequestConfig, HttpResult } from '@http2/client';

const config: HttpRequestConfig = {
    method: "GET",
    url: "https://example.com",
};

const result: HttpResult = httpClient.request(config);
```

## License

Apache-2.0
