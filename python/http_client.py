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
    typedef struct {
        const char *code;
        const char *msg;
    } Error;

    typedef struct {
        char *name;
        char *value;
        int freeName;
        int freeValue;
    } ResponseHeader;

    // Mirrors ResponseStream in include/ResponseStream.h. A NULL stream makes
    // the library collect the body itself; these callbacks run on the thread
    // that receives the bytes, and onData returning non-zero stops the response.
    typedef struct {
        void (*onHeaders)(void *userData, const ResponseHeader *headers, size_t numHeaders);
        int (*onData)(void *userData, const unsigned char *data, size_t len);
        void (*onComplete)(void *userData, Error error);
        void *userData;
    } ResponseStream;

    void initialiseEnv(void);
    void cleanupEnv(void);
    intptr_t handleRequest(const char *requestJSONString, const ResponseStream *stream);
    char* setNonBlocking(const char *requestJSONString, int nonBlocking);
    void freeJson(char *json);
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


def _cstring(pointer):
    """Decode a NUL-terminated C string; None when the pointer is NULL."""
    if pointer == ffi.NULL:
        return None
    return ffi.string(pointer).decode("utf-8", "replace")


def _build_bridge(on_headers, on_data, on_complete):
    """
    A bridge only when the caller wants the body chunk by chunk; None keeps the
    library's own collector, which answers with a complete buffered response.
    The callbacks are all or nothing: a missing on_data would leave the body
    with neither a consumer nor a place in the buffered response.
    """
    callbacks = (on_headers, on_data, on_complete)
    if all(callback is None for callback in callbacks):
        return None
    if not all(callback is not None for callback in callbacks):
        raise ValueError("on_headers, on_data and on_complete must be given together")
    return _StreamBridge(on_headers, on_data, on_complete)


class _StreamBridge:
    """
    cffi trampolines for one streaming request.

    The library calls back on the thread that receives the bytes (the HTTP/2
    connection reader, or the one running an HTTP/1.1 exchange), so the bridge
    must stay referenced until the request is reaped: cffi frees a trampoline as
    soon as its handle is collected. An exception raised by a user callback is
    captured here and re-raised on the calling thread, and stops the response so
    the library does not keep feeding a consumer that already gave up.
    """

    def __init__(self, on_headers, on_data, on_complete):
        self.headers = None       # headers dict handed to on_headers
        self.bytes_received = 0   # decoded body bytes handed to on_data
        self.chunks = 0
        self.error = None         # {"code", "message"} handed to on_complete
        self.exception = None
        self._on_headers = on_headers
        self._on_data = on_data
        self._on_complete = on_complete

        headers_trampoline = ffi.callback(
            "void(*)(void *, const ResponseHeader *, size_t)", self._headers)
        data_trampoline = ffi.callback(
            "int(*)(void *, const unsigned char *, size_t)", self._data)
        complete_trampoline = ffi.callback("void(*)(void *, Error)", self._complete)
        # a trampoline lives exactly as long as the handle returned here
        self._keepalive = (headers_trampoline, data_trampoline, complete_trampoline)

        stream = ffi.new("ResponseStream *")
        stream.onHeaders = headers_trampoline
        stream.onData = data_trampoline
        stream.onComplete = complete_trampoline
        # the trampolines close over this bridge, so no userData is needed
        stream.userData = ffi.NULL
        self.stream = stream

    def _headers(self, user_data, headers, num_headers):
        collected = {}
        for index in range(num_headers):
            collected[_cstring(headers[index].name)] = _cstring(headers[index].value)
        self.headers = collected
        self._invoke(self._on_headers, collected)

    def _data(self, user_data, data, length):
        # the library reuses its decode buffer, so the chunk is copied first
        chunk = ffi.buffer(data, length)[:]
        self.bytes_received += length
        self.chunks += 1
        try:
            stop = self._on_data(chunk)
        except Exception as exc:
            self.exception = exc
            return 1
        return 1 if stop else 0

    def _complete(self, user_data, error):
        code = _cstring(error.code)
        self.error = None if code is None else {"code": code, "message": _cstring(error.msg)}
        self._invoke(self._on_complete, self.error)

    def _invoke(self, callback, argument):
        try:
            callback(argument)
        except Exception as exc:
            self.exception = exc


class HttpClient:
    """
    HTTP client backed by the native C library.

    Mirrors the HttpClient class in nodejs/index.js.
    """

    def __init__(self):
        self._initialized = False
        self._lib = None
        # request id -> _StreamBridge, kept alive for the reader thread until
        # poll_request() reaps the id
        self._streams = {}

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

    def request(self, config, on_headers=None, on_data=None, on_complete=None):
        """
        Send an HTTP request.

        Without callbacks the library collects the decoded body itself and the
        returned JSON carries it in response.payload. Giving all three callbacks
        streams the response instead: the body goes to on_data chunk by chunk,
        response.payload stays empty and "streamed" is 1. A partial set is
        refused, since a missing on_data would leave the body nowhere.

        :param config: Request configuration dict or JSON string.
        :param on_headers: Called once with the response headers as a dict
                           (":status" first), before the body.
        :param on_data: Called with each decoded body chunk (bytes). Return True
                        to stop the response early.
        :param on_complete: Called once with None when the body ended cleanly,
                            otherwise a {"code", "message"} dict.
        :return: Response JSON string from native library.
        :raises RuntimeError: If the request fails.
        :raises TypeError: If config is not a dict or string.
        :raises ValueError: If only some of the callbacks are given.
        """
        if not self._initialized:
            self.init()

        # Encode to bytes for cffi
        request_bytes = self._config_to_bytes(config)

        # Force blocking mode: handleRequest picks the mode from "non-blocking"
        blocking_json = self._lib.setNonBlocking(request_bytes, 0)
        if blocking_json == ffi.NULL:
            raise RuntimeError("invalid request config")

        # The bridge must outlive the call: the callbacks run on the reader thread
        bridge = _build_bridge(on_headers, on_data, on_complete)
        stream = bridge.stream if bridge is not None else ffi.NULL

        # Run the blocking exchange, then collect the completed result into a
        # caller-owned buffer
        handle = self._lib.handleRequest(blocking_json, stream)
        self._lib.freeJson(blocking_json)
        if handle == 0:
            raise RuntimeError("handleRequest failed")

        status, data = self._collect(handle)
        if bridge is not None and bridge.exception is not None:
            raise bridge.exception
        if status == 1 and data is not None:
            return data
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

    def _collect(self, handle):
        """
        Copy the response JSON out of `handle`, growing the buffer when the
        library reports the first attempt was truncated (status 2).

        :param handle: A basket handle from handleRequest.
        :return: (status, data). status is 0 while in flight, 1 when completed,
                 -1 on failure; data is the JSON string, None while in flight.
        """
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
        data = None
        if status != 0 and out_len[0] > 0:
            data = ffi.buffer(buffer, out_len[0])[:].decode("utf-8")
        return status, data

    def start_request(self, config, on_headers=None, on_data=None, on_complete=None):
        """
        Start a non-blocking request. Sends HEADERS/DATA and returns immediately
        with a request id, without waiting for the response. The calling thread
        is not blocked while the response is in flight, so the number of
        concurrent requests is not bounded by any thread-pool size — poll the id
        from one thread to reap results as they complete.

        The callbacks, when given, fire on the connection reader thread while the
        response is in flight; they are released once poll_request() reaps the id.

        :param config: Request configuration dict or JSON string (non-blocking
                       mode is forced by this call, "non-blocking" is pinned
                       to 1 so the socket runs in O_NONBLOCK mode).
        :param on_headers: See request().
        :param on_data: See request().
        :param on_complete: See request().
        :return: Positive request id, or 0 on failure.
        """
        if not self._initialized:
            self.init()

        request_bytes = self._config_to_bytes(config)
        async_json = self._lib.setNonBlocking(request_bytes, 1)
        if async_json == ffi.NULL:
            return 0

        bridge = _build_bridge(on_headers, on_data, on_complete)
        stream = bridge.stream if bridge is not None else ffi.NULL
        handle = self._lib.handleRequest(async_json, stream)
        self._lib.freeJson(async_json)
        if handle == 0:
            return 0

        if bridge is not None:
            self._streams[handle] = bridge
        return handle

    def poll_request(self, request_id):
        """
        Poll a non-blocking request started with start_request.

        :param request_id: The id returned by start_request.
        :return: (status, data) tuple. status is 0 (in flight), 1 (completed) or
                 -1 (failed/timed out); data is the response JSON string when
                 completed, otherwise None.
        :raises RuntimeError: If the id is unknown (already reaped), or if a
                              streaming callback raised while in flight.
        """
        if not self._initialized:
            self.init()

        status, data = self._collect(request_id)
        if status == 0:
            return status, data

        # the exchange is over, so the reader thread no longer calls back
        bridge = self._streams.pop(request_id, None)
        if bridge is not None and bridge.exception is not None:
            raise bridge.exception
        return status, data

    def request_non_blocking(self, config, poll_interval_ms=5, on_headers=None,
                            on_data=None, on_complete=None):
        """
        Convenience: start a non-blocking request and wait (poll) until it
        completes. Equivalent to start_request() + a poll loop.

        :param config: Request configuration dict or JSON string.
        :param poll_interval_ms: Poll interval in milliseconds.
        :param on_headers: See request().
        :param on_data: See request().
        :param on_complete: See request().
        :return: Response JSON string.
        :raises RuntimeError: If the request fails to start or times out.
        """
        import time as _time

        request_id = self.start_request(config, on_headers, on_data, on_complete)
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
            self._streams.clear()
            self._lib.cleanupEnv()
            self._initialized = False
