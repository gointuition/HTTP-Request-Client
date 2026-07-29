/*
 * Test.java
 *
 * HTTP/2 Client Test
 *
 * Mirrors nodejs/test.js and python/test.py -
 * tests init, single request, and cleanup.
 */

import org.json.JSONObject;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

public class Test {

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

    // One concurrent request outcome (mirrors the Worker struct in test_Concurrency.c).
    private static class Result {
        int index;
        int streamId = -1;
        int bytes;
        long ms;
        String errCode; // null on success
    }

    // Run one full request lifecycle and record the outcome, parsing the same
    // error.code / session.streamId / response.payload fields as the C/Node tests.
    private static Result doRequest(int index, String requestJson) {
        Result r = new Result();
        r.index = index;
        long t0 = System.currentTimeMillis();
        try {
            String res = Http2Client.request(requestJson);
            r.ms = System.currentTimeMillis() - t0;
            JSONObject root = new JSONObject(res);
            JSONObject error = root.optJSONObject("error");
            if (error != null) {
                String code = error.optString("code", "");
                if (!code.isEmpty()) {
                    r.errCode = code;
                }
            }
            JSONObject session = root.optJSONObject("session");
            if (session != null) {
                r.streamId = session.optInt("streamId", -1);
            }
            JSONObject response = root.optJSONObject("response");
            if (response != null) {
                r.bytes = response.optString("payload", "").length();
            }
        } catch (Exception e) {
            r.ms = System.currentTimeMillis() - t0;
            r.errCode = e.getMessage() == null ? "EXCEPTION" : e.getMessage();
        }
        return r;
    }

    public static void main(String[] args) {
        System.out.println("=== HTTP/2 Client Test ===\n");

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        // Init
        System.out.println("[Init] Initializing HTTP/2 client...");
        Http2Client.init();
        System.out.println("[Init] Initialized\n");

        // Test 1: Single request
        System.out.println("[Test 1] Single request...");
        try {
            String jsonStr = readJsonWithoutComments(projectRoot + "/bin/request_GET.json");
            long start = System.currentTimeMillis();
            String result = Http2Client.request(jsonStr);
            long elapsed = System.currentTimeMillis() - start;

            JSONObject resultObj = new JSONObject(result);
            System.out.println("    Completed in " + elapsed + "ms");
            System.out.println("    URL: " + resultObj.optString("url", "N/A"));

            JSONObject response = resultObj.optJSONObject("response");
            if (response != null) {
                System.out.println("    Status: " + response.optString("statusCode", "N/A"));
            }

            JSONObject error = resultObj.optJSONObject("error");
            if (error != null) {
                System.out.println("    Error: " + error.optString("message", "none"));
            }
        } catch (Exception e) {
            System.out.println("    Failed: " + e.getMessage());
        }

        // Test 2: concurrent requests (HTTP/2 multiplexing).
        // Http2Client.request() is blocking, so real concurrency needs threads
        // (ExecutorService) - mirrors the pthread workers in test_Concurrency.c and
        // Promise.all in nodejs/test.js. Same-host requests share ONE multiplexed
        // connection, each taking its own odd stream id (1, 3, 5, ...). A warm-up
        // request establishes the shared connection first; the concurrent batch then
        // REUSES it, exercising multiplexing rather than a burst of cold connects
        // (which servers often throttle).
        System.out.println("\n[Test 2] Concurrent requests (multiplexed)...");
        final int DEFAULT_CONCURRENCY = 8;
        try {
            // Read the raw JSON (comments stripped) and pass it to the native client
            // UNCHANGED - org.json.JSONObject is HashMap-backed and would scramble the
            // header order, which is part of this client's browser fingerprint. Parse a
            // throwaway copy only to read the test-only "concurrency" field; the native
            // client ignores that unknown field (mirrors test_Concurrency.c).
            final String requestJson = readJsonWithoutComments(projectRoot + "/tests/test_Concurrency.json");
            JSONObject config = new JSONObject(requestJson);
            int concurrency = config.optInt("concurrency", DEFAULT_CONCURRENCY);
            if (concurrency < 1) {
                concurrency = 1;
            }
            String url = config.optString("url");

            // Warm-up: establish and pool the shared connection.
            System.out.println("    warming up the shared connection...");
            Result warm = doRequest(-1, requestJson);
            if (warm.errCode != null) {
                System.out.println("    warm-up FAILED " + warm.errCode + " (" + warm.ms + "ms)");
                throw new RuntimeException("could not establish the shared connection");
            }
            System.out.println("    warm-up OK (stream " + warm.streamId + ", " + warm.bytes
                    + " bytes, " + warm.ms + "ms); connection pooled");

            System.out.println("    firing " + concurrency + " concurrent requests to " + url);
            ExecutorService executor = Executors.newFixedThreadPool(concurrency);
            List<Future<Result>> futures = new ArrayList<>();
            long start = System.currentTimeMillis();
            for (int i = 0; i < concurrency; i++) {
                final int idx = i;
                futures.add(executor.submit(() -> doRequest(idx, requestJson)));
            }

            int ok = 0;
            TreeSet<Integer> ids = new TreeSet<>();
            for (Future<Result> f : futures) {
                Result r = f.get();
                if (r.errCode != null) {
                    System.out.println("    #" + r.index + " \u2717 " + r.errCode + " (" + r.ms + "ms)");
                } else {
                    ok++;
                    ids.add(r.streamId);
                    System.out.println("    #" + r.index + " \u2713 " + r.bytes + " bytes, stream "
                            + r.streamId + " (" + r.ms + "ms)");
                }
            }
            long totalMs = System.currentTimeMillis() - start;
            executor.shutdown();

            StringBuilder idStr = new StringBuilder();
            for (Integer id : ids) {
                if (idStr.length() > 0) {
                    idStr.append(", ");
                }
                idStr.append(id);
            }
            System.out.println("\u2713 " + ok + "/" + concurrency + " succeeded, total wall time " + totalMs + "ms");
            System.out.println("\u2713 distinct stream ids on the shared connection: " + idStr);
        } catch (Exception e) {
            System.out.println("    Concurrent test failed: " + e.getMessage());
        }

        // Cleanup
        System.out.println("\n[Cleanup] Cleaning up...");
        Http2Client.cleanup();
        System.out.println("[Cleanup] Done");

        System.out.println("\nAll tests completed!");
    }
}
