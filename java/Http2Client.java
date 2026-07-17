/*
 * Http2Client.java
 *
 * HTTP/2 Client - Java binding via JNI.
 *
 * Mirrors python/http2_client.py and nodejs/http2-addon.cc:
 * wraps the native C library (libhttp2client.dylib) and exposes
 * init() / request() / cleanup().
 *
 * Uses JNI (Java Native Interface) - requires a compiled JNI bridge
 * library (libhttp2jni.dylib) that links against libhttp2client.
 */

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

public class Http2Client {

    private static volatile boolean initialized = false;
    private static final int BUFFER_SIZE = 1024 * 1024; // 1 MB, same as nodejs/python

    // ── Native method declarations (implemented in Http2Client.c) ──────────

    /**
     * Initialize the native HTTP/2 client environment.
     * Corresponds to C: initialiseEnv()
     */
    private static native void nativeInit();

    /**
     * Send an HTTP/2 request.
     * Corresponds to C: handleRequest(const char*, char*, size_t)
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

    // ── Library loading ──────────────────────────────────────────────────

    static {
        // Load libhttp2client first (provides initialiseEnv/handleRequest/etc.),
        // then load the JNI bridge (libhttp2jni) which links against it.
        loadNativeLibrary("libhttp2client");
        loadNativeLibrary("libhttp2jni");
    }

    /**
     * Locate and load a native library by base name (e.g. "libhttp2jni").
     *
     * Search order:
     *   1. JAR resource:  /native/<baseName><ext>  (packaged fat JAR)
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

        // 1) Try loading from JAR resource (packaged mode)
        String resourcePath = "/native/" + fileName;
        URL resourceUrl = Http2Client.class.getResource(resourcePath);
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
        String codeSource = Http2Client.class.getProtectionDomain()
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
        File tempDir = new File(System.getProperty("java.io.tmpdir"), "http2client-native");
        tempDir.mkdirs();
        File tempFile = new File(tempDir, fileName + "-" + getVersion());
        tempFile.deleteOnExit();

        try (InputStream in = Http2Client.class.getResourceAsStream(resourcePath)) {
            if (in == null) {
                throw new IOException("Resource not found: " + resourcePath);
            }
            Files.copy(in, tempFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
        return tempFile;
    }

    // ── Public API (mirrors nodejs/python bindings) ──────────────────────

    /**
     * Initialize the HTTP/2 client environment.
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
     * Send an HTTP/2 request.
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
