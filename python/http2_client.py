"""
HTTP/2 Client - Python binding via cffi (ABI mode).

Mirrors nodejs/index.js: wraps the native C library (libhttp2client.dylib)
and exposes init() / request() / cleanup().

Uses cffi ABI mode (ffi.dlopen) - no C compiler required at runtime,
all third-party libs are statically linked into the shared library.
"""

import json
import os
import platform

from cffi import FFI


# ── cffi definition: declare the C API from Http2Client.h ──────────────
ffi = FFI()
ffi.cdef("""
    void initialiseEnv(void);
    void cleanupEnv(void);
    int handleRequest(const char *requestJSONString, char *basketStr, size_t basketStrLen);
    void freeBasketString(char *basketStr);
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
        lib_name = "libhttp2client.dylib"
    elif system == "Linux":
        lib_name = "libhttp2client.so"
    else:
        lib_name = "libhttp2client.dll"

    # 1) project lib/shared
    candidate = os.path.join(lib_shared, lib_name)
    if os.path.exists(candidate):
        return candidate

    # 2) alongside this module (packaged)
    candidate = os.path.join(os.path.dirname(os.path.abspath(__file__)), lib_name)
    if os.path.exists(candidate):
        return candidate

    raise FileNotFoundError(
        f"Cannot find {lib_name}. Please build the project first:\n"
        f"  cd {project_root} && mkdir -p build && cd build && cmake .. && make"
    )


class HttpClient:
    """
    HTTP/2 client backed by the native C library.

    Mirrors the HttpClient class in nodejs/index.js.
    """

    def __init__(self):
        self._initialized = False
        self._lib = None
        self._buffer_size = 1024 * 1024  # 1 MB, same as nodejs addon

    def _load_library(self):
        """Load the native library via cffi dlopen."""
        if self._lib is not None:
            return

        _add_mingw_dll_path()
        lib_path = _find_library()
        self._lib = ffi.dlopen(lib_path)

    def init(self):
        """
        Initialize the HTTP/2 client environment.
        Returns self for chaining.
        """
        if not self._initialized:
            self._load_library()
            self._lib.initialiseEnv()
            self._initialized = True
        return self

    def request(self, config):
        """
        Send an HTTP/2 request.

        :param config: Request configuration dict or JSON string.
        :return: Response JSON string from native library.
        :raises RuntimeError: If the request fails.
        :raises TypeError: If config is not a dict or string.
        """
        if not self._initialized:
            self.init()

        # Convert dict to JSON string if needed
        if isinstance(config, dict):
            json_str = json.dumps(config)
        elif isinstance(config, str):
            json_str = config
        else:
            raise TypeError("config must be a dict or JSON string")

        # Encode to bytes for cffi
        request_bytes = json_str.encode("utf-8")

        # Allocate result buffer via cffi
        buffer = ffi.new("char[]", self._buffer_size)

        # Call native handleRequest
        actual_len = self._lib.handleRequest(request_bytes, buffer, self._buffer_size)

        if actual_len > 0:
            # ffi.string reads up to NUL or specified length
            result_bytes = ffi.buffer(buffer, actual_len)[:]
            return result_bytes.decode("utf-8")
        else:
            raise RuntimeError(f"handleRequest failed with return code: {actual_len}")

    def cleanup(self):
        """Cleanup resources."""
        if self._initialized:
            self._lib.cleanupEnv()
            self._initialized = False
