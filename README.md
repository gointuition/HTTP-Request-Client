# HTTP Request Client

A high-performance HTTP/2 client library written in C, with TLS 1.3 session resumption, HPACK header compression, and Browser-like TLS fingerprinting.

## Features

- **HTTP/2** — full implementation: concurrent stream multiplexing over a shared connection (per-connection reader thread), HPACK dynamic table, flow control, SETTINGS/PING ACK, GOAWAY
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
│   ├── Compat.h        # POSIX <-> Winsock2 networking shim
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
- `lib/shared/libhttp2client.dylib` (macOS) / `.so` (Linux) / `.dll` (Windows) — shared library for language bindings
- `test_GET` / `test_POST` — C test executables

### Build Output

| Artifact | Path |
|----------|------|
| Shared library | `lib/shared/libhttp2client.{dylib,so,dll}` |
| Static library | `lib/static/libhttp2client.a` |
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
#include "Http2Client.h"

// 1. Initialize (call once at startup)
void initialiseEnv(void);

// 2. Send request (two-step, thread-safe)
//    handleRequest is a BLOCKING/synchronous call: it does not return until the
//    response arrives, the timeout elapses, or an error occurs. Concurrency is
//    achieved by calling it from multiple threads -- it may be called
//    concurrently, and same-host requests share one HTTP/2 connection and are
//    multiplexed on separate streams.
//    Step 1: get result pointer and length
//    requestJSONString: JSON config (see format below)
//    outLen: output parameter for response JSON length
//    Returns: malloc'd response JSON string, or NULL on error
char* handleRequest(const char *requestJSONString, int *outLen);

//    Step 2: copy content to your buffer (frees the source pointer)
//    basketStr: pointer returned by handleRequest
//    dest:      caller-allocated buffer (at least outLen + 1 bytes)
void getBasketContent(char *basketStr, char *dest);

// 3. Cleanup (call once at shutdown)
void cleanupEnv(void);
```

> **Blocking by design.** `handleRequest` is synchronous — the calling thread
> blocks until the response is fully read, the timeout fires, or an error is
> returned. There is no async/callback variant in the C core. To issue requests
> concurrently, call `handleRequest` from multiple threads (it is thread-safe);
> same-host requests are then multiplexed over a single shared HTTP/2 connection.
> The language bindings build their concurrency on top of this: e.g. the Node.js
> binding runs each blocking call on a libuv worker thread (raise
> `UV_THREADPOOL_SIZE` for high concurrency — see [nodejs/README.MD](nodejs/README.MD)),
> and Python/Java use their own thread pools.

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

    // Step 1: get result pointer and length
    int len = 0;
    char *result = handleRequest(request, &len);
    if (result != NULL && len > 0) {
        // Step 2: allocate exact buffer, copy content (frees result internally)
        char *buf = malloc(len + 1);
        getBasketContent(result, buf);
        printf("%s\n", buf);
        free(buf);
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
| `log` | `number` | Enable logging: 0 (off), 1 (on) |
| `proxy` | `object` | Proxy: `{ scheme, host, port, authorization? }` |
| `session` | `object` | Session: `{ expirationInMilliseconds, clientHelloId? }` |

### `session.clientHelloId`

Optional uTLS-style identifier that pins the TLS/HTTP/2 wire fingerprint to emulate. When omitted, the fingerprint follows the request's `User-Agent`, and an unrecognized `User-Agent` falls back to `hellochrome_auto`.

| clientHelloId | Emulated profile |
|---------------|------------------|
| `hellochrome_auto` | Desktop Chrome — currently emulated version |
| `hellochrome_150` | Desktop Chrome 150 (version-pinned) |
| `hellocrios_auto` | Chrome on iOS (CriOS) — currently emulated version |
| `hellocrios_150` | Chrome on iOS (CriOS) 150 (version-pinned) |

`_auto` always tracks the latest emulated version, while `_<version>` pins that specific profile. Matching is case-insensitive.

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
| JSON | [Jansson](https://github.com/akheron/jansson) | Request/response JSON serialization |
| Brotli | [Brotli](https://github.com/google/brotli) | Response decompression + TLS cert compression |
| Zstd | [Zstd](https://github.com/facebook/zstd) | Response decompression |
| zlib | [zlib](https://www.zlib.net/) | gzip / deflate decompression |

## Using the Release Artifacts

Every [GitHub Release](https://github.com/your-org/Http2/releases) ships prebuilt binaries so you do **not** need to compile the C library (BoringSSL, etc.) yourself. Pick the assets for your platform:

| Artifact | Contents | Platforms |
|----------|----------|-----------|
| `http2client-<ver>-<plat>.tar.gz` | C shared library (`libhttp2client.{so,dylib,dll}`) + public `include/*.h` | `linux`, `macos`, `win` |
| `http2client-nodejs-<ver>-<plat>.tar.gz` | Node.js N-API addon (`nodejs/`) | `linux`, `macos`, `win` |
| `http2client-python-<ver>-<plat>.tar.gz` | Python cffi binding (`python/`) | `linux`, `macos`, `win` |
| `http2-client-java-<ver>.jar` | Cross-platform fat JAR (native libs inside) | all |

`<plat>` is `linux` / `macos` / `win`. Download the matching set (the binding archives do **not** include the C library — extract the C archive too, or `npm install` / `pip install cffi` will fail to find the native lib).

```bash
# Example: Linux, version 1.0.0
VER=1.0.0
curl -sSL -O "https://github.com/your-org/Http2/releases/download/v${VER}/http2client-${VER}-linux.tar.gz"
curl -sSL -O "https://github.com/your-org/Http2/releases/download/v${VER}/http2client-nodejs-${VER}-linux.tar.gz"
tar -xzf http2client-${VER}-linux.tar.gz        # -> libhttp2client.so + include/
tar -xzf http2client-nodejs-${VER}-linux.tar.gz # -> nodejs/
```

The C library must be loadable at runtime (see the per-binding notes below). The simplest approach is to keep the shared library next to the binding, or add its directory to the loader path:

| Platform | Loader path env / flag |
|----------|------------------------|
| Linux | `LD_LIBRARY_PATH=/path/to/libdir` |
| macOS | `DYLD_LIBRARY_PATH=/path/to/libdir` (or `install_name_tool` the rpath) |
| Windows | add the directory to `PATH` |

A `checksums-<ver>.sha256` file is published alongside the assets — verify before use:

```bash
sha256sum -c "checksums-${VER}.sha256"
```

### Node.js

The `nodejs/` archive contains the prebuilt `http2addon.node` plus its runtime DLLs (Windows) and `node_modules`. The addon expects `libhttp2client` beside it (or on the loader path).

```bash
cd nodejs
# make the C shared library discoverable, e.g. copy it next to the addon:
cp ../libhttp2client.so ./build/Release/    # Linux (macOS: .dylib, Windows: .dll)
# Windows: ensure MinGW runtime DLLs (libwinpthread-1.dll, libgcc_s_seh-1.dll,
#          libstdc++-6.dll) and libhttp2client.dll are on PATH

node -e "const c=require('./index.js'); c.init(); console.log(c.request({method:'GET',url:'https://tls.peet.ws/api/all'})); c.cleanup();"
```

For concurrency, raise the libuv pool (see [nodejs/README.MD](nodejs/README.MD)):

```bash
UV_THREADPOOL_SIZE=8 node your-app.js
```

### Python

The `python/` archive contains `http2_client.py` (cffi ABI mode) which `dlopen`s `libhttp2client` directly — no C compiler needed. Install cffi, then point the binding at the shared library:

```bash
pip install cffi
cd python
# place libhttp2client.{so,dylib,dll} next to http2_client.py, or:
export LD_LIBRARY_PATH=/path/to/libdir   # macOS: DYLD_LIBRARY_PATH, Windows: add to PATH

python -c "import httpClient; httpClient.init(); print(httpClient.request({'method':'GET','url':'https://tls.peet.ws/api/all'})); httpClient.cleanup()"
```

### Java

The fat JAR is cross-platform: the native libraries (`libhttp2client` + JNI bridge) are bundled inside `/native/` and extracted to a temp dir at runtime, so you only need a JDK — no separate C library download.

```bash
# Linux / macOS / Windows (JDK >= 16 needs --enable-native-access; <16 must omit it)
java --enable-native-access=ALL-UNNAMED -cp http2-client-java-1.0.0.jar Test
java --enable-native-access=ALL-UNNAMED -cp http2-client-java-1.0.0.jar Example
```

Embed it as a normal dependency (`Http2Client` lives in the unnamed module, call directly without import):

```java
Http2Client.init();
String result = Http2Client.request(requestJsonString);
Http2Client.cleanup();
```

> **Windows note:** the JNI bridge depends on MinGW runtime DLLs (`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`). Ensure `C:\msys64\mingw64\bin` is on `PATH`, or copy those DLLs next to the JAR.

## License

Apache-2.0
