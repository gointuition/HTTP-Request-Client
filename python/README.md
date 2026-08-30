# http-client

High-performance HTTP client with native C implementation via cffi (ABI mode).

## Prerequisites

Build the shared library first:

```bash
cd <project_root> && mkdir -p build && cd build && cmake .. && make
```

Install Python dependency:

```bash
pip install cffi
```

## Usage

```python
from python import httpClient

# Initialize
httpClient.init()

# Send a request
result = httpClient.request({
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
})

print(result)

# Cleanup when done
httpClient.cleanup()
```

## API

### `httpClient.init()`

Initialize the HTTP client environment. Returns `self` for chaining.

### `httpClient.request(config)`

Send an HTTP request.

**Parameters:**
- `config` (dict | str): Request configuration dict or JSON string

**Returns:**
- `str`: Response JSON string from native library

**Throws:**
- `RuntimeError`: If the native call fails
- `TypeError`: If config is not a dict or string

### `httpClient.cleanup()`

Cleanup resources and release memory.

## Request Configuration

| Field | Type | Description |
|-------|------|-------------|
| `url` | `str` | Target URL (required) |
| `method` | `str` | HTTP method: GET, POST |
| `headers` | `dict[str, str]` | Request headers |
| `payload` | `dict` | Request body (for POST/PUT/PATCH) |
| `connectTimeoutInMilliseconds` | `int` | TCP + TLS connect timeout |
| `responseReadingTimeoutInMilliseconds` | `int` | Response reading timeout |
| `decompress` | `int` | Decompression flags: 0 (none), 1 (gzip), 2 (deflate), 4 (br), 8 (zstd), or combinations (e.g. 15 = all) |
| `log` | `int` | Enable logging: 0 (off), 1 (on) |
| `proxy` | `ProxyConfig` | Proxy settings |
| `session` | `SessionConfig` | Session settings: `{ expirationInMilliseconds, clientHelloId? }` |

### `session.clientHelloId`

Optional uTLS-style identifier that pins the TLS/HTTP wire fingerprint. When omitted, it follows the request's `User-Agent`, and an unrecognized `User-Agent` falls back to `hellochrome_auto`. Supported values: `hellochrome_auto`, `hellochrome_152`, `hellochrome_150`, `hellocrios_auto`, `hellocrios_150` (`_auto` follows the browser major version declared by the `User-Agent` when a profile exists for it, else the latest emulated version; `_<version>` pins a specific one; case-insensitive).

## Running Tests

```bash
# Quick test
python python/test.py

# Full example
python python/example.py

# Or use the build script
bash python/build.sh
```

## How It Works

```
Python (cffi ABI mode)
  → ffi.dlopen("libhttpclient.dylib")
    → C library (BoringSSL + nghttp2-style HTTP)
      → HTTP over TLS to server
```

- **No C compiler required at runtime** — cffi ABI mode (`ffi.dlopen`) loads the pre-built shared library directly
- **Zero-copy buffer access** — cffi buffer protocol avoids unnecessary data copying
- **TLS 1.3 session resumption** — automatic `pre_shared_key` for subsequent connections to the same host

## Packaging

```bash
pip install .
```

Installs as `http-client` package. Requires `libhttpclient.dylib` (or `.so` on Linux) bundled alongside the module.

## Using the Release Artifacts

Each [GitHub Release](https://github.com/gointuition/HTTP-Request-Client/releases) ships per-platform wheels that bundle `libhttpclient` inside the package, so `pip install` needs no compiler. A source sdist (`http_client-<ver>.tar.gz`) is also published for platforms without a prebuilt wheel (you build `libhttpclient` locally before installing).

| Platform | File |
| --- | --- |
| Linux (x86_64, manylinux) | `http_client-<ver>-py3-none-manylinux_2_17_x86_64.whl` |
| macOS (x86_64) | `http_client-<ver>-py3-none-macosx_11_0_x86_64.whl` |
| Windows (x86_64) | `http_client-<ver>-py3-none-win_amd64.whl` |
| Source (any platform) | `http_client-<ver>.tar.gz` (sdist) |

Install the wheel matching **your current platform**:

```bash
# Linux
pip install ./http_client-1.0.0-py3-none-manylinux_2_17_x86_64.whl
# macOS
pip install ./http_client-1.0.0-py3-none-macosx_11_0_x86_64.whl
# Windows
pip install ./http_client-1.0.0-py3-none-win_amd64.whl
```

```python
import http_client
client = http_client.HttpClient()
# ...
```

Installing the wheel also places `libhttpclient` inside the package, so no loader-path setup is needed. (`cffi` is pulled in automatically via `install_requires`.)

> **Why a separate package per platform?** The wheel bundles a platform-specific
> native library (`libhttpclient.so` / `.dylib` / `.dll`). cffi loads it via
> `dlopen` at runtime, so the wheel must be built on and tagged for the exact
> target platform — the same constraint as numpy, grpcio, etc.

### Cross-platform deployment (important)

Your Python **source code** is platform-independent, but `http_client` depends on
a native library that is **not**. The rule of thumb (same as numpy, grpcio, …):

> **Source is portable; dependencies are not. Re-install the package on the
> target machine instead of copying it over.**

So if you write the program on macOS and run it on a Linux server:

```bash
# 1. Copy your .py source to the Linux server (e.g. scp / git clone)
# 2. On the Linux server — install the MATCHING wheel, NOT the macOS one:
pip install ./http_client-1.0.0-py3-none-manylinux_2_17_x86_64.whl
python your_app.py
```

Do **not** copy the macOS virtualenv or the macOS `.whl` to Linux — the `.dylib`
inside it cannot be loaded by Linux.

**Alternatives:**

- **Containerize (recommended for servers):** put your code in a Docker image and
  `pip install http_client` inside the **Linux** image, so code + dependency +
  platform are locked together and runnable anywhere Docker exists.
- **No prebuilt wheel for your platform?** Install the source distribution and
  build the native library yourself on the target machine:
  ```bash
  # build libhttpclient first (see project root README "Building"), then:
  pip install ./http_client-1.0.0.tar.gz
  ```

## License

Apache-2.0
