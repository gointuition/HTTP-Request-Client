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

## License

Apache-2.0
