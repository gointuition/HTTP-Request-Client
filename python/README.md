# http2-client

High-performance HTTP/2 client with native C implementation via cffi (ABI mode).

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

Initialize the HTTP/2 client environment. Returns `self` for chaining.

### `httpClient.request(config)`

Send an HTTP/2 request.

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
| `session` | `SessionConfig` | Session settings |

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
  → ffi.dlopen("libhttp2client.dylib")
    → C library (BoringSSL + nghttp2-style HTTP/2)
      → HTTP/2 over TLS to server
```

- **No C compiler required at runtime** — cffi ABI mode (`ffi.dlopen`) loads the pre-built shared library directly
- **Zero-copy buffer access** — cffi buffer protocol avoids unnecessary data copying
- **TLS 1.3 session resumption** — automatic `pre_shared_key` for subsequent connections to the same host

## Packaging

```bash
pip install .
```

Installs as `http2client` package. Requires `libhttp2client.dylib` (or `.so` on Linux) bundled alongside the module.

## License

Apache-2.0
