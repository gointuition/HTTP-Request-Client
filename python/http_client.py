"""
HTTP Client - Python binding via cffi (ABI mode).

Mirrors nodejs/index.js: wraps the native C library (libhttpclient.dylib)
and exposes init() / request() / cleanup().

Uses cffi ABI mode (ffi.dlopen) - no C compiler required at runtime,
all third-party libs are statically linked into the shared library.
"""

import json
import os
import platform

from cffi import FFI


# ── cffi definition: declare the C API from HttpClient.h ──────────────
ffi = FFI()
ffi.cdef("""
    void initialiseEnv(void);
    void cleanupEnv(void);
    intptr_t handleRequest(const char *requestJSONString);
    char* setNonBlocking(const char *requestJSONString, int nonBlocking);
    void free(void *ptr);
    void handleResponse(intptr_t basketHandle, char *dest, int capacity, int *outStatus, int *outLen);
""")


def _add_mingw_dll_path():
    """On Windows, add MinGW bin to DLL search path for runtime dependencies."""
    if platform.system() != "Windows":
        return

    # Common MSYS2 MinGW64 install locations
    candidates = [
        r"C:\msys64\mingw64\bin",
        r"C:\mingw64\bin",
        os.path.join(os.environ.get("MSYS2_ROOT", r"C:\msys64"), "mingw64", "bin"),
    ]
    for d in candidates:
        if os.path.isdir(d) and os.path.exists(os.path.join(d, "libwinpthread-1.dll")):
            if hasattr(os, "add_dll_directory"):
                os.add_dll_directory(d)
            else:
                os.environ["PATH"] = d + os.pathsep + os.environ.get("PATH", "")
            return


def _find_library():
    """Locate the shared library relative to this file."""
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lib_shared = os.path.join(project_root, "lib", "shared")

    system = platform.system()
    if system == "Darwin":
        lib_name = "libhttpclient.dylib"
    elif system == "Linux":
        lib_name = "libhttpclient.so"
    else:
        lib_name = "libhttpclient.dll"

    # 1) project lib/shared
    candidate = os.path.join(lib_shared, lib_name)
    if os.path.exists(candidate):
        return candidate

    # 2) alongside this module (packaged)
    candidate = os.path.join(os.path.dirname(os.path.abspath(__file__)), lib_name)
    if os.path.exists(candidate):
        return candidate

    # 3) inside the package's bundled lib/ directory (wheel install)
    candidate = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib", lib_name)
    if os.path.exists(candidate):
        return candidate

    raise FileNotFoundError(
        f"Cannot find {lib_name}. Please build the project first:\n"
        f"  cd {project_root} && mkdir -p build && cd build && cmake .. && make"
    )


class HttpClient:
    """
    HTTP client backed by the native C library.

    Mirrors the HttpClient class in nodejs/index.js.
    """

    def __init__(self):
        self._initialized = False
        self._lib = None

    def _load_library(self):
        """Load the native library via cffi dlopen."""
        if self._lib is not None:
            return

        _add_mingw_dll_path()
        lib_path = _find_library()
        self._lib = ffi.dlopen(lib_path)

    def init(self):
        """
        Initialize the HTTP client environment.
        Returns self for chaining.
        """
        if not self._initialized:
            self._load_library()
            self._lib.initialiseEnv()
            self._initialized = True
        return self

    def request(self, config):
        """
        Send an HTTP request.

        :param config: Request configuration dict or JSON string.
        :return: Response JSON string from native library.
        :raises RuntimeError: If the request fails.
        :raises TypeError: If config is not a dict or string.
        """
        if not self._initialized:
            self.init()

        # Encode to bytes for cffi
        request_bytes = self._config_to_bytes(config)

        # Force blocking mode: handleRequest picks the mode from "non-blocking"
        blocking_json = self._lib.setNonBlocking(request_bytes, 0)
        if blocking_json == ffi.NULL:
            raise RuntimeError("invalid request config")

        # Run the blocking exchange, then collect the completed result into a
        # caller-owned buffer
        handle = self._lib.handleRequest(blocking_json)
        self._lib.free(blocking_json)
        if handle == 0:
            raise RuntimeError("handleRequest failed")

        capacity = 1024 * 1024
        buffer = ffi.new("char[]", capacity)
        out_status = ffi.new("int *")
        out_len = ffi.new("int *")
        self._lib.handleResponse(handle, buffer, capacity, out_status, out_len)
        if out_status[0] == 2:
            # response bigger than the buffer: grow and re-collect
            capacity = out_len[0] + 1
            buffer = ffi.new("char[]", capacity)
            self._lib.handleResponse(handle, buffer, capacity, out_status, out_len)

        status = out_status[0]
        actual_len = out_len[0]
        if status == 1 and actual_len > 0:
            return ffi.buffer(buffer, actual_len)[:].decode("utf-8")
        raise RuntimeError("handleRequest failed")

    def _config_to_bytes(self, config):
        """Convert a request config (dict or JSON string) to encoded bytes."""
        if isinstance(config, dict):
            json_str = json.dumps(config)
        elif isinstance(config, str):
            json_str = config
        else:
            raise TypeError("config must be a dict or JSON string")
        return json_str.encode("utf-8")

    def start_request(self, config):
        """
        Start a non-blocking request. Sends HEADERS/DATA and returns immediately
        with a request id, without waiting for the response. The calling thread
        is not blocked while the response is in flight, so the number of
        concurrent requests is not bounded by any thread-pool size — poll the id
        from one thread to reap results as they complete.

        :param config: Request configuration dict or JSON string (non-blocking
                       mode is forced by this call, "non-blocking" is pinned
                       to 1 so the socket runs in O_NONBLOCK mode).
        :return: Positive request id, or 0 on failure.
        """
        if not self._initialized:
            self.init()

        request_bytes = self._config_to_bytes(config)
        async_json = self._lib.setNonBlocking(request_bytes, 1)
        if async_json == ffi.NULL:
            return 0
        handle = self._lib.handleRequest(async_json)
        self._lib.free(async_json)
        return handle

    def poll_request(self, request_id):
        """
        Poll a non-blocking request started with start_request.

        :param request_id: The id returned by start_request.
        :return: (status, data) tuple. status is 0 (in flight), 1 (completed) or
                 -1 (failed/timed out); data is the response JSON string when
                 completed, otherwise None.
        :raises RuntimeError: If the id is unknown (already reaped).
        """
        if not self._initialized:
            self.init()

        capacity = 1024 * 1024
        buffer = ffi.new("char[]", capacity)
        out_status = ffi.new("int *")
        out_len = ffi.new("int *")
        self._lib.handleResponse(request_id, buffer, capacity, out_status, out_len)
        if out_status[0] == 2:
            # response bigger than the buffer: grow and re-collect
            capacity = out_len[0] + 1
            buffer = ffi.new("char[]", capacity)
            self._lib.handleResponse(request_id, buffer, capacity, out_status, out_len)

        status = out_status[0]
        actual_len = out_len[0]

        data = None
        if status != 0 and actual_len > 0:
            data = ffi.buffer(buffer, actual_len)[:].decode("utf-8")

        return status, data

    def request_non_blocking(self, config, poll_interval_ms=5):
        """
        Convenience: start a non-blocking request and wait (poll) until it
        completes. Equivalent to start_request() + a poll loop.

        :param config: Request configuration dict or JSON string.
        :param poll_interval_ms: Poll interval in milliseconds.
        :return: Response JSON string.
        :raises RuntimeError: If the request fails to start or times out.
        """
        import time as _time

        request_id = self.start_request(config)
        if not request_id:
            raise RuntimeError("failed to start non-blocking request")

        while True:
            status, data = self.poll_request(request_id)
            if status != 0:
                return data if data is not None else ""
            _time.sleep(poll_interval_ms / 1000.0)

    def cleanup(self):
        """Cleanup resources."""
        if self._initialized:
            self._lib.cleanupEnv()
            self._initialized = False
