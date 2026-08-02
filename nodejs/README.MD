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

For **development from source** (builds the native addon locally via node-gyp):

```bash
cd nodejs
npm install        # installs devDependencies
npm run build      # compiles http2addon.node from source
```

For **consuming the published package**, no compilation is needed — the npm
tarball ships prebuilt addons under `prebuilds/<plat>-x64/` and `load-addon.js`
picks the right one automatically (see "Using the Release Artifacts" below).

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
cd <project_root>
cmake -B build -G "MinGW Makefiles"
cmake --build build -j4
```

**Step 2 — Build the addon** (PowerShell):

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

### Concurrent requests (async)

`requestAsync` returns a Promise. The native call itself is **blocking**, so it
runs on a libuv worker thread; requests issued together execute in parallel up
to the libuv thread-pool size (default **4** — raise it via `UV_THREADPOOL_SIZE`,
see [Concurrency & Multiplexing](#concurrency--multiplexing)). Same-host
requests share **one** HTTP/2 connection and each takes its own stream, so they
are multiplexed rather than serialized.

```javascript
const httpClient = require('./index.js');
httpClient.init();

const config = {
    method: "GET",
    url: "https://www.cloudflare.com/cdn-cgi/trace",
    headers: { "host": "www.cloudflare.com", "user-agent": "Mozilla/5.0 ... Chrome/145.0.0.0 Safari/537.36" },
};

// 8 concurrent requests over a single multiplexed connection
const responses = await Promise.all(
    Array.from({ length: 8 }, () => httpClient.requestAsync(config))
);
responses.forEach((res) => console.log(JSON.parse(res).session.streamId)); // 1, 3, 5, 7, ...

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

### `httpClient.requestAsync(config)`

Send an HTTP/2 request asynchronously. The underlying native call is
**blocking** (it does synchronous socket I/O in C), so `requestAsync` offloads
it to a libuv worker thread to keep the JS event loop responsive. Multiple
pending requests therefore run in parallel only up to the size of the libuv
thread pool — see [Concurrency & Multiplexing](#concurrency--multiplexing) for
how to raise it with `UV_THREADPOOL_SIZE`. Same-host requests are multiplexed
over a single HTTP/2 connection (each on its own stream).

**Parameters:**
- `config` (Object | string): Request configuration object or JSON string

**Returns:**
- `Promise<string>`: Resolves with the response JSON string

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
| `clientHelloId` | `string?` | uTLS-style fingerprint profile to emulate (see below) |

Optional `clientHelloId` pins the TLS/HTTP/2 wire fingerprint. When omitted, it follows the request's `User-Agent`, and an unrecognized `User-Agent` falls back to `hellochrome_auto`. Supported values: `hellochrome_auto`, `hellochrome_150`, `hellocrios_auto`, `hellocrios_150` (`_auto` tracks the latest version, `_<version>` pins a specific one; case-insensitive).

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

## Concurrency & Multiplexing

`requestAsync` makes HTTP/2 multiplexing usable from JavaScript:

- **Shared connection** — concurrent same-host requests reuse one connection
  (session creation is serialized in the C core, so a burst of requests does not
  each open its own connection).
- **One stream per request** — each request is assigned an odd, incrementing
  stream id (`1, 3, 5, …`) on the shared connection.
- **Per-connection reader thread** — a single reader thread owns `SSL_read` and
  demultiplexes inbound frames to the waiting request by stream id, keeping the
  shared HPACK dynamic table in wire-arrival order.
- **Connection keep-alive** — the client acknowledges server `SETTINGS` and
  `PING` frames, so long-lived multiplexed connections stay open.

> **Note:** some servers close the connection after a single response (they send
> `GOAWAY` with `Last-Stream-ID = 1`); against those, requests fall back to one
> connection per request. Servers that keep the connection open (e.g. Cloudflare)
> show the full `1, 3, 5, 7, …` stream sequence on one connection.

### Blocking calls & the libuv thread pool

The native request is **synchronous/blocking** by design. `requestAsync` hides
this by running each call on a libuv worker thread, but libuv's pool has only
**4 threads by default**. That is the real ceiling on concurrency: issuing 8
`requestAsync` calls with the default pool runs at most 4 at a time while the
rest queue for a free worker.

To get true N-way concurrency, raise the pool with the `UV_THREADPOOL_SIZE`
environment variable (valid range 1–1024):

```bash
# run 8 requests genuinely in parallel
UV_THREADPOOL_SIZE=8 node your-app.js
```

**Timing matters:** libuv creates the pool lazily on the first async operation
and its size is fixed from that moment. Setting it from JavaScript only works if
you do so **before any async work** (including the first `requestAsync`):

```javascript
// must be the very first line, before requiring the client or any I/O
process.env.UV_THREADPOOL_SIZE = '8';
const httpClient = require('./index.js');
```

Setting it as an OS-level environment variable before launching Node is the most
reliable approach. Size the pool to your target concurrency (`UV_THREADPOOL_SIZE
= max in-flight requests`); note the pool is shared with Node's own file-system
and DNS operations, so leave a little headroom if the app also does heavy disk
or `dns.lookup` work.

## Using the Release Artifacts

Each [GitHub Release](https://github.com/gointuition/HTTP-Request-Client/releases) ships a standard npm package `http2-client-nodejs-<ver>.tgz` with prebuilt addons under `prebuilds/<plat>-x64/http2addon.node`. Install it directly from the asset (no registry needed):

```bash
npm install ./http2-client-nodejs-1.0.0.tgz
```

`load-addon.js` selects the matching `prebuilds/<plat>-x64/http2addon.node` automatically — **no compilation**. The addon still needs `libhttp2client` next to it (or on the loader path), so also install the C library from the `http2client-<ver>-all.tar.gz` release asset.

The native request is synchronous/blocking; `requestAsync` runs calls on libuv worker threads (default pool size 4). Raise the pool for real concurrency:

```bash
UV_THREADPOOL_SIZE=8 node your-app.js
```

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
