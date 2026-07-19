# @http2/client

High-performance HTTP/2 client with native C implementation using N-API.

## Prerequisites

Build the shared library first:

```bash
cd <project_root> && mkdir -p build && cd build && cmake .. && make
```

Install dependencies:

```bash
npm install
```

This will automatically compile the native addon via `node-gyp`.

### Windows (MSYS2 / MinGW-w64)

The addon must be built with the **same MinGW-w64 toolchain** that builds `libhttp2client.dll` (native MSVC is not supported). Run everything from the **MSYS2 MinGW64** shell, using its Node.js and toolchain:

```bash
pacman -S --needed mingw-w64-x86_64-nodejs mingw-w64-x86_64-toolchain make
cd nodejs
npm install        # runs build-addon.js
```

`build-addon.js` detects Windows and passes `--format=make` to `node-gyp`, so it uses GCC + make instead of Visual Studio. It also copies `libhttp2client.dll` next to the compiled `http2addon.node` (Windows has no rpath) so the addon loads at runtime.

To rebuild explicitly:

```bash
npm run build
```

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
{
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
