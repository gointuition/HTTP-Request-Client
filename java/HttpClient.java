/*
 * HttpClient.java
 *
 * HTTP Client - Java binding via JNI.
 *
 * Mirrors python/http_client.py and nodejs/http-addon.cc:
 * wraps the native C library (libhttpclient.dylib) and exposes
 * init() / request() / cleanup().
 *
 * Uses JNI (Java Native Interface) - requires a compiled JNI bridge
 * library (libhttpjni.dylib) that links against libhttpclient.
 */

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Map;

public class HttpClient {

    private static volatile boolean initialized = false;
    private static final int BUFFER_SIZE = 1024 * 1024; // 1 MB, same as nodejs/python

    /**
     * Streaming response callbacks.
     *
     * Passing a listener to request() or startRequest() streams the response:
     * the decoded body is handed to onData chunk by chunk, and the response JSON
     * then reports "streamed": 1 with an empty payload. Without a listener the
     * library collects the body itself and the JSON carries it in
     * response.payload.
     *
     * The callbacks run on the thread that receives the bytes (the connection
     * reader), not on the calling thread, so implementations must be thread safe
     * and must not call back into this library.
     *
     * All three are mandatory: a listener that skipped onData would leave the
     * body with neither a consumer nor a place in the buffered response, so the
     * compiler rejects an implementation that does not provide them.
     */
    public interface ResponseListener {

        /**
         * Response headers, ":status" first, delivered once before the body.
         *
         * @param headers Header name to value, in wire order.
         */
        void onHeaders(Map<String, String> headers);

        /**
         * One decoded body chunk.
         *
         * @param chunk The decoded bytes, owned by the caller (copied per call).
         * @return true to stop the response, false to keep reading.
         */
        boolean onData(byte[] chunk);

        /**
         * Delivered once per attempt, after the last chunk.
         *
         * @param errorCode null when the body ended cleanly, otherwise the code.
         * @param errorMessage null when the body ended cleanly, otherwise the text.
         */
        void onComplete(String errorCode, String errorMessage);
    }

    // ── Native method declarations (implemented in HttpClient.c) ──────────

    /**
     * Initialize the native HTTP client environment.
     * Corresponds to C: initialiseEnv()
     */
    private static native void nativeInit();

    /**
     * Send an HTTP request.
     * Corresponds to C: handleRequest(const char*, NULL) in blocking mode
     * ("non-blocking" is forced to 0 by the native bridge).
     *
     * @param requestJson JSON string describing the request config.
     * @return Response JSON string, or null on failure.
     */
    private static native String nativeRequest(String requestJson);

    /**
     * Cleanup native resources.
     * Corresponds to C: cleanupEnv()
     */
    private static native void nativeCleanup();

    /**
     * Start a non-blocking request (send HEADERS/DATA, return immediately).
     * Corresponds to C: handleRequest(const char*, NULL) with "non-blocking" forced
     * to 1; the returned basket handle is the id to poll via
     * nativePollRequest.
     *
     * @param requestJson JSON string describing the request config.
     * @return Positive request id to poll, or 0 on failure.
     */
    private static native long nativeStartRequest(String requestJson);

    /**
     * Poll a non-blocking request started with nativeStartRequest.
     * Corresponds to C: handleResponse(long, int*, int*)
     *
     * @param requestId The id returned by nativeStartRequest.
     * @return Object[]{ Integer status (0 in-flight / 1 done / -1 failed),
     *                    String data (response JSON when done) }.
     */
    private static native Object[] nativePollRequest(long requestId);

    /**
     * Send a blocking request whose body is streamed to the listener.
     * Corresponds to C: handleRequest(const char*, const ResponseStream*).
     *
     * @param requestJson JSON string describing the request config.
     * @param listener Streaming callbacks, kept alive for the whole exchange.
     * @return Response JSON string, or null on failure.
     */
    private static native String nativeRequestStreaming(String requestJson, ResponseListener listener);

    /**
     * Start a non-blocking request whose body is streamed to the listener.
     * The listener outlives this call: it runs on the connection reader thread
     * until the id is reaped by nativePollRequest.
     *
     * @param requestJson JSON string describing the request config.
     * @param listener Streaming callbacks.
     * @return Positive request id to poll, or 0 on failure.
     */
    private static native long nativeStartRequestStreaming(String requestJson, ResponseListener listener);

    // ── Library loading ──────────────────────────────────────────────────

    static {
        // Load libhttpclient first (provides initialiseEnv/handleRequest/etc.),
        // then load the JNI bridge (libhttpjni) which links against it.
        loadNativeLibrary("libhttpclient");
        loadNativeLibrary("libhttpjni");
    }

    /**
     * Map the current OS to the platform sub-directory baked into the
     * cross-platform fat JAR: linux | macos | win.
     */
    private static String platformDir() {
        String os = System.getProperty("os.name").toLowerCase();
        if (os.contains("linux")) {
            return "linux";
        } else if (os.contains("mac") || os.contains("darwin")) {
            return "macos";
        } else if (os.contains("windows") || os.contains("win")) {
            return "win";
        }
        throw new UnsatisfiedLinkError("Unsupported OS for native loading: " + os);
    }

    /**
     * Locate and load a native library by base name (e.g. "libhttpjni").
     *
     * Search order:
     *   1. JAR resource:  /native/<plat>/<baseName><ext>  (cross-platform fat JAR)
     *   2. Filesystem:    build/, ../, ../../lib/shared/  (development)
     *   3. Fallback:      System.loadLibrary()
     */
    private static void loadNativeLibrary(String baseName) {
        String ext = ".dylib";
        if (System.getProperty("os.name").toLowerCase().contains("linux")) {
            ext = ".so";
        } else if (System.getProperty("os.name").toLowerCase().contains("windows")) {
            ext = ".dll";
        }

        String fileName = baseName + ext;

        // 1) Try loading from JAR resource (packaged mode).
        //    Cross-platform JARs nest natives under /native/<plat>/; single-platform
        //    (or locally-built) JARs keep them directly under /native/. Try both.
        String resourcePath = "/native/" + platformDir() + "/" + fileName;
        URL resourceUrl = HttpClient.class.getResource(resourcePath);
        if (resourceUrl == null) {
            resourcePath = "/native/" + fileName;
            resourceUrl = HttpClient.class.getResource(resourcePath);
        }
        if (resourceUrl != null) {
            try {
                File tempFile = extractToTemp(resourcePath, fileName);
                System.load(tempFile.getAbsolutePath());
                return;
            } catch (IOException e) {
                throw new UnsatisfiedLinkError(
                    "Failed to extract native library from JAR: " + resourcePath +
                    " - " + e.getMessage());
            }
        }

        // 2) Search filesystem (development mode)
        String codeSource = HttpClient.class.getProtectionDomain()
                .getCodeSource().getLocation().getPath();
        // URL-decode the path (handles spaces etc.)
        codeSource = java.net.URLDecoder.decode(codeSource, java.nio.charset.StandardCharsets.UTF_8);
        File codeSourceFile = new File(codeSource);
        String classDir = codeSourceFile.getAbsolutePath();

        String[] searchPaths;
        if (codeSourceFile.isDirectory()) {
            // Running from a directory (e.g. java/build/)
            String projectRoot = codeSourceFile.getParentFile().getParent();
            searchPaths = new String[] {
                classDir,                                              // java/build/
                classDir + File.separator + ".." + File.separator,     // java/
                projectRoot + File.separator + "lib" + File.separator + "shared", // lib/shared/
            };
        } else {
            // Running from a JAR but native lib not bundled inside
            String jarDir = codeSourceFile.getParent();
            String projectRoot = new File(jarDir).getParentFile().getParent();
            searchPaths = new String[] {
                jarDir,                                                // alongside JAR
                jarDir + File.separator + ".." + File.separator + "native", // ../native/
                projectRoot + File.separator + "lib" + File.separator + "shared", // lib/shared/
            };
        }

        for (String dir : searchPaths) {
            File candidate = new File(dir, fileName).getAbsoluteFile();
            if (candidate.exists()) {
                System.load(candidate.getAbsolutePath());
                return;
            }
        }

        // 3) Fallback: System.loadLibrary (searches java.library.path)
        try {
            System.loadLibrary(baseName);
        } catch (UnsatisfiedLinkError e) {
            throw new UnsatisfiedLinkError(
                "Cannot find " + fileName + ". Searched:\n" +
                "  JAR resource: " + resourcePath + "\n" +
                String.join("\n  ", java.util.Arrays.stream(searchPaths)
                    .map(d -> new File(d, fileName).getAbsolutePath())
                    .toArray(String[]::new)) + "\n" +
                "Please run build.sh first to build and package the native libraries.\n" +
                "Original error: " + e.getMessage());
        }
    }

    /**
     * Extract a JAR resource to a temporary file.
     */
    private static File extractToTemp(String resourcePath, String fileName) throws IOException {
        // Extract into a per-version subdirectory but keep the REAL library
        // filename. This lets a co-located dependent library (libhttpjni ->
        // libhttpclient) be resolved via rpath ($ORIGIN on Linux, @loader_path
        // on macOS) at load time. A version-suffixed filename would break the
        // name-based lookup on Linux and cause "undefined symbol" errors.
        File tempDir = new File(System.getProperty("java.io.tmpdir"),
                "httpclient-native" + File.separator + getVersion());
        tempDir.mkdirs();
        File tempFile = new File(tempDir, fileName);
        tempFile.deleteOnExit();

        try (InputStream in = HttpClient.class.getResourceAsStream(resourcePath)) {
            if (in == null) {
                throw new IOException("Resource not found: " + resourcePath);
            }
            Files.copy(in, tempFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        return tempFile;
    }

    // ── Public API (mirrors nodejs/python bindings) ──────────────────────

    /**
     * Initialize the HTTP client environment.
     * Returns true for consistency (mirrors nodejs InitEnv).
     */
    public static boolean init() {
        if (!initialized) {
            nativeInit();
            initialized = true;
        }
        return true;
    }

    /**
     * Send an HTTP request.
     *
     * @param requestJson JSON string describing the request config.
     * @return Response JSON string from native library.
     * @throws RuntimeException if the request fails.
     */
    public static String request(String requestJson) {
        if (!initialized) {
            init();
        }

        String result = nativeRequest(requestJson);
        if (result == null) {
            throw new RuntimeException("handleRequest failed (native returned null)");
        }
        return result;
    }

    /**
     * Send an HTTP request with a streamed response.
     *
     * The body goes to {@code listener.onData} chunk by chunk instead of being
     * buffered, so the returned JSON reports "streamed": 1 with an empty
     * payload. The callbacks run on the connection reader thread.
     *
     * @param requestJson JSON string describing the request config.
     * @param listener Streaming callbacks, all three of them (see
     *                 {@link ResponseListener}); null behaves like request(String).
     * @return Response JSON string from native library.
     * @throws RuntimeException if the request fails.
     */
    public static String request(String requestJson, ResponseListener listener) {
        if (!initialized) {
            init();
        }
        if (listener == null) {
            return request(requestJson);
        }

        String result = nativeRequestStreaming(requestJson, listener);
        if (result == null) {
            throw new RuntimeException("handleRequest failed (native returned null)");
        }
        return result;
    }

    /**
     * Poll result for a non-blocking request.
     */
    public static final class PollResult {
        /** 0 = in flight, 1 = completed, -1 = failed/timed out. */
        public final int status;
        /** Response JSON string when completed; otherwise null. */
        public final String data;

        public PollResult(int status, String data) {
            this.status = status;
            this.data = data;
        }

        public boolean isDone() {
            return status != 0;
        }
    }

    /**
     * Start a non-blocking request. Sends HEADERS/DATA and returns immediately
     * without waiting for the response. The calling thread is not blocked while
     * the response is in flight, so the number of concurrent requests is not
     * bounded by any thread-pool size — poll the returned id from one thread to
     * reap results as they complete.
     *
     * @param requestJson JSON string describing the request config (non-blocking
     *                    mode is forced by the native bridge, the socket runs
     *                    in O_NONBLOCK mode).
     * @return Positive request id, or 0 on failure.
     */
    public static long startRequest(String requestJson) {
        if (!initialized) {
            init();
        }
        return nativeStartRequest(requestJson);
    }

    /**
     * Start a non-blocking request with a streamed response. The listener runs
     * on the connection reader thread until pollRequest() reaps the id.
     *
     * @param requestJson JSON string describing the request config.
     * @param listener Streaming callbacks; null behaves like startRequest(String).
     * @return Positive request id, or 0 on failure.
     */
    public static long startRequest(String requestJson, ResponseListener listener) {
        if (!initialized) {
            init();
        }
        if (listener == null) {
            return startRequest(requestJson);
        }
        return nativeStartRequestStreaming(requestJson, listener);
    }

    /**
     * Poll a non-blocking request started with startRequest.
     *
     * @param requestId The id returned by startRequest.
     * @return PollResult carrying the status and (on completion) the response JSON.
     * @throws IllegalStateException if the id is unknown (already reaped).
     */
    public static PollResult pollRequest(long requestId) {
        if (!initialized) {
            init();
        }
        Object[] arr = nativePollRequest(requestId);
        if (arr == null || arr.length < 1 || arr[0] == null) {
            throw new IllegalStateException("pollRequest returned no result");
        }
        int status = ((Integer) arr[0]).intValue();
        String data = arr.length >= 2 ? (String) arr[1] : null;
        return new PollResult(status, data);
    }

    /**
     * Convenience: start a non-blocking request and wait (poll) until it
     * completes. Equivalent to startRequest() + a poll loop. Returns the response
     * JSON string.
     *
     * @param requestJson JSON string describing the request config.
     * @param pollIntervalMs Poll interval in milliseconds.
     * @return Response JSON string.
     * @throws RuntimeException if the request fails to start or times out.
     */
    public static String requestNonBlocking(String requestJson, long pollIntervalMs) {
        return requestNonBlocking(requestJson, pollIntervalMs, null);
    }

    /**
     * Convenience: start a non-blocking request with a streamed response and
     * wait (poll) until it completes.
     *
     * @param requestJson JSON string describing the request config.
     * @param pollIntervalMs Poll interval in milliseconds.
     * @param listener Streaming callbacks, or null for a buffered response.
     * @return Response JSON string.
     * @throws RuntimeException if the request fails to start or times out.
     */
    public static String requestNonBlocking(String requestJson, long pollIntervalMs,
                                           ResponseListener listener) {
        long id = startRequest(requestJson, listener);
        if (id == 0) {
            throw new RuntimeException("failed to start non-blocking request");
        }
        while (true) {
            PollResult r = pollRequest(id);
            if (r.isDone()) {
                return r.data != null ? r.data : "";
            }
            try {
                Thread.sleep(pollIntervalMs);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new RuntimeException("interrupted while polling non-blocking request", e);
            }
        }
    }

    /**
     * Cleanup resources.
     */
    public static void cleanup() {
        if (initialized) {
            nativeCleanup();
            initialized = false;
        }
    }

    /**
     * Get version.
     */
    public static String getVersion() {
        return "1.0.0";
    }
}
