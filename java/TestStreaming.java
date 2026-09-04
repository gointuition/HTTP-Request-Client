/*
 * TestStreaming.java
 *
 * HTTP Client - Streaming Response Test
 *
 * Mirrors tests/test_Streaming.c, python/test_streaming.py and
 * nodejs/test_streaming.js: the decoded body is handed to a ResponseListener
 * chunk by chunk instead of being buffered, and every case asserts both what the
 * callbacks saw and what the response JSON reports ("streamed": 1, empty
 * payload).
 *
 *   1. request_Streaming.json over HTTP/2, whole body delivered chunk by chunk
 *   2. same, stopped by onData after 64 KB (RST_STREAM path)
 *   3. same over HTTP/1.1
 *   4. same non-blocking (callbacks fire on the connection reader thread)
 *   5. request_Streaming_gzip.json: gzip body decoded incrementally
 *   6. control: no listener at all keeps the buffered response intact
 */

import org.json.JSONObject;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class TestStreaming {

    private static final int ABORT_AFTER_BYTES = 64 * 1024;
    private static final String ABORTED_BY_CONSUMER_CODE = "3-0014";
    private static final int POLL_INTERVAL_MS = 5;

    /** One streaming case: which request file to send, over which transport, and
     *  how much of the body the consumer is willing to read. */
    private static final class Case {
        final String label;
        final String file;
        final boolean http11;
        final boolean nonBlocking;
        final int abortAfter;           // 0 = read the whole body
        final String expectedPrefix;    // null = do not look at the body bytes

        Case(String label, String file, boolean http11, boolean nonBlocking,
             int abortAfter, String expectedPrefix) {
            this.label = label;
            this.file = file;
            this.http11 = http11;
            this.nonBlocking = nonBlocking;
            this.abortAfter = abortAfter;
            this.expectedPrefix = expectedPrefix;
        }
    }

    private static final Case[] CASES = {
            new Case("http/2, whole body",
                    "request_Streaming.json", false, false, 0, null),
            new Case("http/2, stopped by the consumer",
                    "request_Streaming.json", false, false, ABORT_AFTER_BYTES, null),
            new Case("http/1.1, whole body",
                    "request_Streaming.json", true, false, 0, null),
            new Case("non-blocking, callbacks on the reader thread",
                    "request_Streaming.json", false, true, 0, null),
            new Case("gzip body decoded incrementally",
                    "request_Streaming_gzip.json", false, false, 0, "{"),
    };

    private static final Case BUFFERED =
            new Case("no listener, buffered control",
                    "request_Streaming.json", false, false, 0, null);

    /** What the callbacks saw. They run on the thread that receives the bytes, so
     *  every field is read and written under the recorder's own lock. */
    private static final class Recorder implements HttpClient.ResponseListener {
        private final int abortAfter;
        private final byte[] head = new byte[8];
        private Map<String, String> headers;
        private String errorCode;
        private int completes;
        private int headLength;
        private int chunks;
        private long bytes;

        Recorder(int abortAfter) {
            this.abortAfter = abortAfter;
        }

        @Override
        public synchronized void onHeaders(Map<String, String> headers) {
            this.headers = headers;
        }

        @Override
        public synchronized boolean onData(byte[] chunk) {
            bytes += chunk.length;
            chunks++;
            for (byte b : chunk) {
                if (headLength >= head.length) {
                    break;
                }
                head[headLength++] = b;
            }
            // returning true asks the library to tear the stream down
            return abortAfter > 0 && bytes >= abortAfter;
        }

        @Override
        public synchronized void onComplete(String errorCode, String errorMessage) {
            completes++;
            this.errorCode = errorCode;
        }

        synchronized long getBytes() {
            return bytes;
        }

        /** The first bytes of the body, so a case can tell gzip from plain text. */
        synchronized byte[] headBytes() {
            byte[] copy = new byte[headLength];
            System.arraycopy(head, 0, copy, 0, headLength);
            return copy;
        }

        synchronized int getChunks() {
            return chunks;
        }

        synchronized int getCompletes() {
            return completes;
        }

        synchronized String getErrorCode() {
            return errorCode;
        }

        synchronized String header(String name) {
            return headers == null ? null : headers.get(name);
        }
    }

    private static String readJsonWithoutComments(String filePath) throws IOException {
        List<String> lines = Files.readAllLines(Paths.get(filePath));
        StringBuilder sb = new StringBuilder();
        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.isEmpty() || trimmed.startsWith("//") || trimmed.startsWith("#")) {
                continue;
            }
            sb.append(line).append("\n");
        }
        return sb.toString();
    }

    /** Load a request file (comments stripped) and rewrite its transport knobs. */
    private static String prepareConfig(String projectRoot, Case testCase) throws IOException {
        JSONObject config = new JSONObject(readJsonWithoutComments(projectRoot + "/bin/" + testCase.file));
        if (testCase.http11) {
            JSONObject session = config.optJSONObject("session");
            if (session == null) {
                session = new JSONObject();
                config.put("session", session);
            }
            session.put("protocol", "http/1.1");
        }
        return config.toString();
    }

    private static String errorCodeOf(String resultJson) {
        if (resultJson == null || resultJson.isEmpty()) {
            return null;
        }
        JSONObject error = new JSONObject(resultJson).optJSONObject("error");
        return error != null && error.has("code") ? error.optString("code") : null;
    }

    /** Compare what the callbacks saw with what the response JSON reports. */
    private static List<String> validate(Recorder recorder, String resultJson, Case testCase) {
        List<String> problems = new ArrayList<>();
        boolean aborted = testCase.abortAfter > 0;

        JSONObject response = resultJson == null || resultJson.isEmpty()
                ? new JSONObject()
                : new JSONObject(resultJson).optJSONObject("response");
        if (response == null) {
            response = new JSONObject();
        }
        String errorCode = errorCodeOf(resultJson);

        if (response.optInt("streamed", -1) != 1) {
            problems.add("response.streamed is " + response.opt("streamed") + ", expected 1");
        }
        if (!response.optString("payload", "").isEmpty()) {
            problems.add("response.payload must be empty, got "
                    + response.optString("payload").length() + " bytes");
        }
        if (aborted && !ABORTED_BY_CONSUMER_CODE.equals(errorCode)) {
            problems.add("error.code is " + errorCode + ", expected " + ABORTED_BY_CONSUMER_CODE);
        }
        if (!aborted && errorCode != null) {
            problems.add("unexpected error.code " + errorCode);
        }

        if (recorder.getCompletes() != 1) {
            problems.add("onComplete ran " + recorder.getCompletes() + " times, expected 1");
        }
        if (recorder.getBytes() == 0) {
            problems.add("no body byte delivered");
        }
        if (aborted && recorder.getBytes() < testCase.abortAfter) {
            problems.add("stopped after " + recorder.getBytes() + " bytes, expected at least "
                    + testCase.abortAfter);
        }

        String completedCode = recorder.getErrorCode();
        if (!aborted && completedCode != null) {
            problems.add("onComplete reported " + completedCode);
        }
        if (aborted && completedCode != null && !ABORTED_BY_CONSUMER_CODE.equals(completedCode)) {
            problems.add("onComplete reported " + completedCode + ", expected "
                    + ABORTED_BY_CONSUMER_CODE);
        }

        String status = recorder.header(":status");
        if (!aborted && !"200".equals(status)) {
            problems.add(":status is " + status + ", expected 200");
        }

        // an uncompressed body must arrive complete
        String contentLength = recorder.header("content-length");
        String encoding = recorder.header("content-encoding");
        boolean encoded = encoding != null && !encoding.equalsIgnoreCase("identity");
        if (!aborted && !encoded && contentLength != null && contentLength.matches("\\d+")) {
            long advertised = Long.parseLong(contentLength);
            if (recorder.getBytes() != advertised) {
                problems.add("delivered " + recorder.getBytes() + " of " + advertised
                        + " advertised bytes");
            }
        }

        if (testCase.expectedPrefix != null) {
            String head = new String(recorder.headBytes(), StandardCharsets.UTF_8);
            if (!head.startsWith(testCase.expectedPrefix)) {
                problems.add("body starts with " + head + ", expected " + testCase.expectedPrefix);
            }
        }
        return problems;
    }

    /** Print one case verdict. */
    private static boolean report(String label, List<String> problems, long byteCount, int chunkCount) {
        if (!problems.isEmpty()) {
            System.out.println("    FAILED [" + label + "]");
            for (String problem : problems) {
                System.out.println("      - " + problem);
            }
            return false;
        }
        System.out.println("    passed [" + label + "]: " + byteCount + " bytes in "
                + chunkCount + " chunks");
        return true;
    }

    private static boolean runCase(String projectRoot, Case testCase) throws IOException {
        String config = prepareConfig(projectRoot, testCase);
        Recorder recorder = new Recorder(testCase.abortAfter);

        String resultJson = testCase.nonBlocking
                ? HttpClient.requestNonBlocking(config, POLL_INTERVAL_MS, recorder)
                : HttpClient.request(config, recorder);

        return report(testCase.label, validate(recorder, resultJson, testCase),
                recorder.getBytes(), recorder.getChunks());
    }

    /** Control: without a listener the response stays a complete buffered one. */
    private static boolean runBufferedCase(String projectRoot) throws IOException {
        String config = prepareConfig(projectRoot, BUFFERED);
        JSONObject response = new JSONObject(HttpClient.request(config)).optJSONObject("response");
        if (response == null) {
            response = new JSONObject();
        }

        List<String> problems = new ArrayList<>();
        String payload = response.optString("payload", "");
        if (response.optInt("streamed", -1) != 0) {
            problems.add("response.streamed is " + response.opt("streamed") + ", expected 0");
        }
        if (payload.isEmpty()) {
            problems.add("response.payload is empty");
        }
        return report(BUFFERED.label, problems, payload.length(), 1);
    }

    public static void main(String[] args) {
        System.out.println("=== HTTP Client - Streaming Response Test ===\n");

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        System.out.println("[Init] Initializing HTTP client...");
        HttpClient.init();
        System.out.println("[Init] Initialized\n");

        int passed = 0;
        int failed = 0;

        for (Case testCase : CASES) {
            boolean ok;
            try {
                ok = runCase(projectRoot, testCase);
            } catch (Exception e) {
                System.out.println("    FAILED [" + testCase.label + "]: " + e.getMessage());
                ok = false;
            }
            if (ok) {
                passed++;
            } else {
                failed++;
            }
        }

        try {
            if (runBufferedCase(projectRoot)) {
                passed++;
            } else {
                failed++;
            }
        } catch (Exception e) {
            System.out.println("    FAILED [" + BUFFERED.label + "]: " + e.getMessage());
            failed++;
        }

        System.out.println("\nCleaning up...");
        HttpClient.cleanup();

        int total = passed + failed;
        System.out.println("\n========================================");
        System.out.println("Results: " + passed + "/" + total + " passed, " + failed + "/" + total + " failed");
        if (failed > 0) {
            System.out.println("SOME TESTS FAILED");
            System.exit(1);
        }
        System.out.println("All tests passed!");
    }
}
