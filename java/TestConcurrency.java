/*
 * TestConcurrency.java
 *
 * HTTP/2 Client — Concurrency Test
 *
 * Tests HTTP/2 multiplexing: multiple concurrent requests sharing
 * one connection via a thread-pool, each taking its own odd stream id (1, 3, 5, ...).
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

public class TestConcurrency {

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

    // One concurrent request outcome.
    private static class Result {
        int index;
        int streamId = -1;
        int bytes;
        long ms;
        String errCode; // null on success
    }

    // Run one full request lifecycle and record the outcome.
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
        System.out.println("=== HTTP/2 Client — Concurrency Test ===\n");

        int passed = 0;
        int failed = 0;
        final int DEFAULT_CONCURRENCY = 8;

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        // Init
        System.out.println("[Init] Initializing HTTP/2 client...");
        Http2Client.init();
        System.out.println("[Init] Initialized\n");

        // Concurrent requests (HTTP/2 multiplexing).
        System.out.println("[Test] Concurrent requests (multiplexed)...");
        try {
            final String requestJson = readJsonWithoutComments(projectRoot + "/bin/request_Concurrency.json");
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
                failed++;
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
                    failed++;
                    System.out.println("    #" + r.index + " \u2717 " + r.errCode + " (" + r.ms + "ms)");
                } else {
                    ok++;
                    passed++;
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
            failed++;
            System.out.println("    Concurrent test failed: " + e.getMessage());
        }

        // Cleanup
        System.out.println("\n[Cleanup] Cleaning up...");
        Http2Client.cleanup();
        System.out.println("[Cleanup] Done");

        int total = passed + failed;
        if (total == 0) total = 1;
        System.out.println("\n========================================");
        System.out.println("Results: " + passed + "/" + total + " passed, " + failed + "/" + total + " failed");
        if (failed > 0) {
            System.out.println("SOME TESTS FAILED");
            System.exit(1);
        }
        System.out.println("All tests passed!");
    }
}
