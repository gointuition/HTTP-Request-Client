# HTTP Request Client

A high-performance HTTP/2 client library written in C, with TLS 1.3 session resumption, HPACK header compression, and Browser-like TLS fingerprinting.

## Features

- **HTTP/2** — full implementation: concurrent stream multiplexing over a shared connection (per-connection reader thread), HPACK dynamic table, flow control, SETTINGS/PING ACK, GOAWAY
- **HTTP/1.1** — downgrade / fallback transport: ALPN downgrade, forced protocol via `"session": {"protocol": "http/1.1"}`, plain `http://` URLs, automatic retry on `HTTP_1_1_REQUIRED`; keep-alive session reuse and `Connection: close` handling; header order preserved on the wire; async requests queue serially on the shared connection
- **TLS 1.3** — session resumption with `pre_shared_key` (NewSessionTicket callback)
- **TLS fingerprint** — GREASE, ECH, ALPS, cert compression (Brotli), signature algorithms alignment
- **Compression** — gzip, deflate, Brotli, Zstd response decompression
- **Proxy** — HTTPS CONNECT tunnel with authorization
- **Session pool** — thread-safe connection reuse with configurable expiration (up to 1024 concurrent sessions); concurrent same-host requests share one multiplexed connection
- **Cross-language** — native bindings for [Node.js](nodejs/) (N-API), [Python](python/) (cffi), [Java](java/) (JNI)

## Architecture

```
┌─────────────────────────────────────────────────┐
│                 Language Bindings               │
│     Node.js (N-API) │ Python (cffi) │ Java (JNI)│
├─────────────────────────────────────────────────┤
│             libhttpclient (shared lib)          │
├─────────────────────────────────────────────────┤
│ HttpClient  → Basket → Session → Http2RequestHandler │
│                        → Http2ResponseHandler        │
│                        → Http11RequestHandler        │
│                        → Http11ResponseHandler       │
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
│   ├── HttpClient.h    # C API entry point
│   ├── Basket.h        # Request/Response/Session data structures
│   ├── SSLHandler.h    # TLS layer
│   ├── Session.h       # Connection session pool
│   ├── Compat.h        # POSIX <-> Winsock2 networking shim
│   └── ...
├── src/                # C source
│   ├── HttpClient.c    # init / request / cleanup + request registry (handleRequest / handleResponse)
│   ├── Session.c       # Session pool + TLS session cache
│   ├── SSLHandler.c    # Browser-like TLS configuration
│   ├── Http2RequestHandler.c # HTTP/2 HEADERS + DATA frames
│   ├── Http2ResponseHandler.c # Frame parsing, HPACK decoding
│   ├── Http11RequestHandler.c # HTTP/1.1 request building/sending + exchange orchestration
│   ├── Http11ResponseHandler.c # HTTP/1.1 response parsing (chunked / content-length / until-close)
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
- Git (third-party dependencies are cloned automatically at configure time)
- **Windows:** [MSYS2](https://www.msys2.org/) with the MinGW-w64 toolchain (see below). This codebase relies on POSIX APIs (`pthread`, `<stdatomic.h>`, BSD sockets), which MinGW-w64 provides; native MSVC is not supported.

zlib is used for gzip/deflate. On macOS/Linux the system zlib is used automatically; if it is missing it is downloaded and built from source.

## Building

### macOS

```bash
cmake -B build
cmake --build build -j$(sysctl -n hw.ncpu)
```

### Linux

```bash
cmake -B build
cmake --build build -j$(nproc)
```

### Windows (MSYS2 / MinGW-w64)

Native MSVC is not supported. Build inside the **MSYS2 MinGW64** shell, whose GCC ships the POSIX headers (`pthread.h`, `stdatomic.h`, sockets) this project needs.

1. Install [MSYS2](https://www.msys2.org/), then open the **"MSYS2 MinGW64"** shell and install the toolchain (NASM is required by BoringSSL):
   ```bash
   pacman -S --needed \
     mingw-w64-x86_64-toolchain \
     mingw-w64-x86_64-cmake \
     mingw-w64-x86_64-nasm \
     git
   ```
2. Configure and build:
   ```bash
   cmake -B build -G "Ninja"
   cmake --build build -j$(nproc)
   ```
   (Use `-G "MinGW Makefiles"` if Ninja is not installed.)

> If configuration fails with `No CMAKE_ASM_NASM_COMPILER could be found`, install NASM (`pacman -S mingw-w64-x86_64-nasm`) or pass `-DOPENSSL_NO_ASM=1`. See [Troubleshooting](#troubleshooting).

#### Networking compatibility layer

The networking code is written against the POSIX BSD socket API. On Windows this is bridged to Winsock2 by [`include/Compat.h`](include/Compat.h), so no source changes are needed per platform:

| POSIX | Windows (via `Compat.h`) |
|-------|--------------------------|
| `<sys/socket.h>`, `<netdb.h>`, `<arpa/inet.h>` | `<winsock2.h>`, `<ws2tcpip.h>` |
| `close(fd)` | `closeSocket(fd)` → `closesocket` |
| `fcntl(fd, ..., O_NONBLOCK)` | `setSocketNonBlocking` / `setSocketBlocking` → `ioctlsocket(FIONBIO)` |
| `errno` / `EINPROGRESS` / `ECONNREFUSED` | `SOCKET_LAST_ERROR` / `SOCKET_EINPROGRESS` / `SOCKET_ECONNREFUSED` |
| `usleep` | `sleepMicroseconds` → `Sleep` |

`WSAStartup` / `WSACleanup` are invoked automatically inside `initialiseEnv()` / `cleanupEnv()`, and the Winsock library (`ws2_32`) is linked automatically on Windows. No extra setup is required.

This produces:
- `lib/shared/libhttpclient.dylib` (macOS) / `.so` (Linux) / `.dll` (Windows) — shared library for language bindings
- `test_GET` / `test_POST` — C test executables

### Build Output

| Artifact | Path |
|----------|------|
| Shared library | `lib/shared/libhttpclient.{dylib,so,dll}` |
| Static library | `lib/static/libhttpclient.a` |
| Test executables | `bin/test_GET`, `bin/test_POST` |

## BoringSSL Patches

Matching a real browser's TLS ClientHello byte-for-byte requires a few capabilities that stock BoringSSL does not expose. Rather than fork the submodule, the project keeps small, self-contained patches under [`patches/boringssl/`](patches/boringssl/) and applies them to the pristine checkout at build time.

**How they are applied** — the `apply_third_party_patches()` function in [`CMakeLists.txt`](CMakeLists.txt) globs `patches/boringssl/*.patch`, applies them in filename order (`0001-`, `0002-`, …) with `git apply`, and skips any patch that `git apply --reverse --check` reports as already applied. This runs automatically at `cmake` configure time, is idempotent (re-running never double-applies), and needs no manual step.

| Patch | Purpose |
|-------|---------|
| `0001-tls13-configurable-cipher-order.patch` | Adds `SSL_set_tls13_cipher_prefs(ssl, str)`, letting the caller advertise the TLS 1.3 cipher suites in an explicit order. Stock BoringSSL fixes the TLS 1.3 cipher order based on AES-hardware detection, which leaks the host's CPU capabilities into the fingerprint. The patch also re-enables three legacy 3DES suites (`ECDHE-ECDSA-DES-CBC3-SHA`, `ECDHE-RSA-DES-CBC3-SHA`, `DES-CBC3-SHA`) so the advertised cipher list can match the reference browser exactly. |
| `0002-allow-duplicate-verify-sigalgs.patch` | Lets the client-advertised `signature_algorithms` list contain duplicate entries (e.g. `rsa_pss_rsae_sha384` twice), which some browsers emit and which BoringSSL otherwise rejects. Only the verify (ClientHello) preferences allow duplicates; signing preferences still reject them, since a repeated entry there is a genuine configuration error. |

The browser-specific values these patches consume (cipher list, signature algorithms, extension toggles) live in the per-profile `BrowserFingerprint` structs in [`src/BrowserHandler.c`](src/BrowserHandler.c) and are applied in [`src/SSLHandler.c`](src/SSLHandler.c).

## C API

```c
#include "HttpClient.h"

// 1. Initialize (call once at startup)
void initialiseEnv(void);

// 2. Send request (unified entry, thread-safe): builds the basket and starts
//    the request, returning the basket pointer as an intptr_t handle (0 on
//    failure). The "non-blocking" field (default 1) selects the mode; either
//    way the response is collected by passing the handle and a caller-owned
//    buffer to handleResponse():
//      non-blocking = 1 (DEFAULT): fire-and-forget — the call returns
//      immediately; the response is collected in the background and
//      handleResponse() polls it. The registry owns the basket until the
//      result is fully copied out — only pass the handle to handleResponse().
//      non-blocking = 0: BLOCKING — the call does not return until the
//      response arrives, the timeout elapses, or an error occurs; the basket
//      already holds the result, and handleResponse() copies it into the
//      buffer. Concurrency is achieved by calling it from
//      multiple threads -- it may be called concurrently, and same-host
//      requests share one HTTP/2 connection and are multiplexed on separate
//      streams.
intptr_t handleRequest(const char *requestJSONString);

// 3. Cleanup (call once at shutdown)
void cleanupEnv(void);

// ─── Response retrieval (unified) ───
// Collect the response for a handle returned by handleRequest. The result is
// copied into the caller-owned buffer (dest/capacity); the caller allocates
// and frees it (who allocates, frees). Blocking baskets are already complete
// and are copied out immediately; non-blocking requests are polled until the
// exchange finishes (HTTP/2: the connection's reader thread; HTTP/1.1: a
// per-request exchange thread serialized on the shared connection).
// If the buffer is too small the full result stays cached in the basket,
// *outLen carries the complete length, and the same handle must be passed
// again with a buffer of at least outLen + 1 bytes.
//   outStatus: 0 = still in flight, 1 = fully copied, 2 = complete but
//              truncated (re-call with a bigger buffer), -1 = failed/timed out
//   outLen:    full length of the response JSON (valid for status 1, 2, -1)
// Once status is 1 (or -1 with the result copied) the handle is reclaimed.
void handleResponse(intptr_t basketHandle, char *dest, int capacity, int *outStatus, int *outLen);

// Reap all in-flight requests (called automatically by cleanupEnv).
void cleanupAsyncRequests(void);
```

> **Two concurrency models.** The core offers both a non-blocking and a
> blocking surface:
>
> * **`handleRequest` non-blocking mode (default)** — `handleRequest` sends the
>   request and returns immediately with a basket handle; the caller passes it
>   to `handleResponse` later (e.g. on an event loop timer) and reaps
>   the finished result. This path does not occupy a thread while waiting, so
>   it scales independently of any thread-pool size, and the underlying socket
>   runs in `O_NONBLOCK` mode. Both transports support this mode: HTTP/2
>   multiplexes all in-flight requests on one connection; HTTP/1.1 runs one
>   exchange at a time per connection, so queued non-blocking requests on the
>   same host are serialized automatically.
>
> * **`handleRequest` blocking mode** — set `"non-blocking": 0` in the request
>   config: the call blocks until the response is fully read, the timeout
>   fires, or an error is returned; passing the handle and a buffer to
>   `handleResponse` then copies the finished result out immediately.
>   Concurrency is achieved by calling from multiple threads (thread-safe);
>   same-host requests are multiplexed over one shared HTTP/2 connection.
>
> The language bindings build on both surfaces:
>
> * **Node.js** — `requestAsync` runs the blocking `handleRequest` on a libuv
>   worker (raise `UV_THREADPOOL_SIZE` for high concurrency — see
>   [nodejs/README.MD](nodejs/README.MD)); `requestNonBlocking` uses the async
>   surface and never blocks the event loop.
> * **Python** — `request` (blocking, call from threads / a `ThreadPoolExecutor`;
>   cffi releases the GIL so these run in parallel); `start_request` +
>   `poll_request` (or the `request_non_blocking` convenience) for the async
>   surface.
> * **Java** — `request` (blocking, call from threads / an `ExecutorService`);
>   `executeRequest` + `pollRequest` (or the `requestNonBlocking` convenience) for
>   the async surface.
>
> The bindings' blocking wrappers force `"non-blocking": 0` and the async
> wrappers force `"non-blocking": 1`, so each surface keeps its semantics
> regardless of the caller's config.
>
> The async surface matters when a thread pool is bounded (e.g. fewer worker
> threads than in-flight requests): a single thread can drive arbitrarily many
> in-flight requests by firing them all and polling, instead of holding one
> thread per blocking call.

### Example

```c
#include "HttpClient.h"
#include <stdlib.h>

int main() {
    initialiseEnv();

    const char *request = "{"\n"
        "  \"method\": \"GET\","\n"
        "  \"url\": \"https://tls.peet.ws/api/all\","\n"
        "  \"connectTimeoutInMilliseconds\": 3000,"\n"
        "  \"responseReadingTimeoutInMilliseconds\": 30000,"\n"
        "  \"decompress\": 0,\n"
        "  \"log\": 1,\n"
        "  \"non-blocking\": 0,\n"
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

    // Step 1: run the request in blocking mode; the returned basket handle
    // already points at the finished result
    intptr_t handle = handleRequest(request);
    if (handle != 0) {
        // Step 2: collect the result into a caller-owned buffer
        int capacity = 1024 * 1024;
        char *buf = malloc(capacity);
        if (buf != NULL) {
            int status = 0;
            int len = 0;
            handleResponse(handle, buf, capacity, &status, &len);
            if (status == 2) {
                // buffer too small: grow to the full length and re-collect
                char *bigger = realloc(buf, len + 1);
                if (bigger != NULL) {
                    buf = bigger;
                    capacity = len + 1;
                    handleResponse(handle, buf, capacity, &status, &len);
                }
            }
            if (status == 1) {
                printf("%s\n", buf);
            }
            free(buf);
        }
    }

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
| `non-blocking` | `number` | Request mode: 1 (default) = fire-and-forget — `handleRequest` returns a basket handle immediately and the socket runs in `O_NONBLOCK` (`select`/`SSL_ERROR_WANT_*` retry); the response is reaped by passing the handle to `handleResponse`. 0 = blocking — `handleRequest` waits for the whole exchange and returns a handle to the completed basket |
| `log` | `number` | Enable logging: 0 (off), 1 (on) |
| `proxy` | `object` | Proxy: `{ scheme, host, port, authorization? }` |
| `session` | `object` | Session: `{ expirationInMilliseconds, clientHelloId?, protocol? }` |

### `session.clientHelloId`

Optional uTLS-style identifier that pins the TLS/HTTP/2 wire fingerprint to emulate. When omitted, the fingerprint follows the request's `User-Agent`, and an unrecognized `User-Agent` falls back to `hellochrome_auto`.

| clientHelloId | Emulated profile |
|---------------|------------------|
| `hellochrome_auto` | Desktop Chrome — currently emulated version |
| `hellochrome_150` | Desktop Chrome 150 (version-pinned) |
| `hellocrios_auto` | Chrome on iOS (CriOS) — currently emulated version |
| `hellocrios_150` | Chrome on iOS (CriOS) 150 (version-pinned) |

`_auto` always tracks the latest emulated version, while `_<version>` pins that specific profile. Matching is case-insensitive.

### `session.protocol`

Optional transport override. Setting `"session": { "protocol": "http/1.1" }` forces the whole exchange onto the HTTP/1.1 transport (ALPN advertises `http/1.1` only), regardless of server support. Plain `http://` URLs always run HTTP/1.1. When omitted, the transport follows ALPN negotiation (HTTP/2 preferred).

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

## Troubleshooting

### Windows: `No CMAKE_ASM_NASM_COMPILER could be found`

BoringSSL compiles its optimized crypto routines from `.asm` sources, which requires the [NASM](https://www.nasm.us/) assembler. CMake aborts when `nasm` is not on your `PATH`. Since Windows builds use the MSYS2 / MinGW-w64 toolchain (see [Building](#building)), choose one of the following fixes:

**Option A — Install NASM (recommended, keeps assembly optimizations)**

1. In the **MSYS2 MinGW64** shell, install NASM:
   ```bash
   pacman -S --needed mingw-w64-x86_64-nasm
   ```
2. Verify it is on your `PATH`:
   ```bash
   nasm --version
   ```
3. Delete the CMake cache and re-configure so the compiler is re-detected:
   ```bash
   rm -rf build
   cmake -B build -G "Ninja"
   cmake --build build
   ```

If CMake still cannot find it, point it explicitly:
```bash
cmake -B build -G "Ninja" -DCMAKE_ASM_NASM_COMPILER="$(which nasm)"
```

**Option B — Disable assembly optimizations (no NASM needed)**

Build BoringSSL in pure-C mode. This is slightly slower but avoids the assembler dependency entirely:
```bash
cmake -B build -G "Ninja" -DOPENSSL_NO_ASM=1
cmake --build build
```

## Tech Stack

| Component | Library | Purpose |
|-----------|---------|---------|
| TLS 1.3 | [BoringSSL](https://github.com/google/boringssl) | TLS handshake, session resumption, cert compression |
| HTTP/2 | Custom implementation | Frames, HPACK, stream multiplexing, flow control |
| HTTP/1.1 | Custom implementation | Downgrade/fallback transport: keep-alive reuse, chunked/content-length/until-close framing |
| JSON | [Jansson](https://github.com/akheron/jansson) | Request/response JSON serialization |
| Brotli | [Brotli](https://github.com/google/brotli) | Response decompression + TLS cert compression |
| Zstd | [Zstd](https://github.com/facebook/zstd) | Response decompression |
| zlib | [zlib](https://www.zlib.net/) | gzip / deflate decompression |

## Using the Release Artifacts

Every [GitHub Release](https://github.com/gointuition/HTTP-Request-Client/releases) ships prebuilt binaries so you do **not** need to compile the C library (BoringSSL, etc.) yourself. Each binding is published following its language's **standard package-manager layout**, so you install/use it the way you would any other package.

| Component | Release artifact(s) | Standard form |
|-----------|--------------------|---------------|
| C library | `httpclient-<ver>-all.tar.gz` | tarball with `linux/` `macos/` `win/` subdirs (lib + `include/`) |
| Node.js | `http-client-nodejs-<ver>.tgz` | npm package with `prebuilds/<plat>-x64/httpaddon.node` |
| Python | `http_client-<ver>-<plat>.whl` (×3) | platform wheels, self-contained native lib inside |
| Java | `http-client-java-<ver>.jar` | single cross-platform fat JAR (classes + `native/linux\|macos\|win`) |

`<plat>` is `linux` / `macos` / `win`. A `checksums-<ver>.sha256` file is published alongside — verify before use:

```bash
sha256sum -c "checksums-${VER}.sha256"
```

The C library (`libhttpclient`) must be loadable at runtime (see per-binding notes). The simplest approach is to keep it next to the binding, or add its directory to the loader path:

| Platform | Loader path env / flag |
|----------|------------------------|
| Linux | `LD_LIBRARY_PATH=/path/to/libdir` |
| macOS | `DYLD_LIBRARY_PATH=/path/to/libdir` (or `install_name_tool` the rpath) |
| Windows | add the directory to `PATH` |

The per-binding install/use instructions live in each language's own README:

- **Node.js** — [nodejs/README.MD](nodejs/README.MD)
- **Python** — [python/README.md](python/README.md)
- **Java** — [java/README.md](java/README.md)

## License

Apache-2.0
