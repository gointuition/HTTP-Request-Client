/*
 * TestNonBlocking.java
 *
 * HTTP Client — Non-Blocking Concurrency Test
 *
 * Unlike TestConcurrency.java (which runs the blocking request() on a
 * thread-pool), this uses the async surface (startRequest + pollRequest):
 * requests are fired without blocking, then polled from a single thread, so the
 * number of concurrent in-flight requests is NOT bounded by any thread-pool size.
 */

import org.json.JSONObject;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

public class TestNonBlocking {

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

    public static void main(String[] args) {
        System.out.println("=== HTTP Client — Non-Blocking Concurrency Test ===\n");

        int passed = 0;
        int failed = 0;
        final int DEFAULT_CONCURRENCY = 8;

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        System.out.println("[Init] Initializing HTTP client...");
        HttpClient.init();
        System.out.println("[Init] Initialized\n");

        System.out.println("[Test] non-blocking concurrent requests (async surface)...");
        try {
            final String requestJson = readJsonWithoutComments(projectRoot + "/bin/request_Concurrency.json");
            JSONObject config = new JSONObject(requestJson);
            int concurrency = config.optInt("concurrency", DEFAULT_CONCURRENCY);
            if (concurrency < 1) {
                concurrency = 1;
            }
            String url = config.optString("url");

            // Force the non-blocking socket I/O mode for the async surface.
            config.put("non-blocking", 1);
            final String requestJsonNb = config.toString();

            System.out.println("    firing " + concurrency + " requests to " + url + " (all in flight at once)");
            long start = System.currentTimeMillis();

            // Fire every request without blocking: this loop returns almost
            // instantly even though none of the responses have arrived yet.
            Map<Long, Integer> pending = new HashMap<>();
            int[] started = new int[concurrency];
            for (int i = 0; i < concurrency; i++) {
                long id = HttpClient.startRequest(requestJsonNb);
                if (id == 0) {
                    failed++;
                    started[i] = -1;
                    System.out.println("    #" + i + " \u2717 failed to start (id=0)");
                } else {
                    started[i] = 1;
                    pending.put(id, i);
                }
            }

            // Reap results by polling; one thread drives all requests.
            String[] results = new String[concurrency];
            while (!pending.isEmpty()) {
                for (Long rid : new ArrayList<>(pending.keySet())) {
                    HttpClient.PollResult r = HttpClient.pollRequest(rid);
                    if (r.status == 0) {
                        continue; // still in flight
                    }
                    int i = pending.remove(rid);
                    results[i] = r.data;
                }
                if (!pending.isEmpty()) {
                    Thread.sleep(5);
                }
            }
            long totalMs = System.currentTimeMillis() - start;

            int ok = 0;
            TreeSet<Integer> ids = new TreeSet<>();
            for (int i = 0; i < concurrency; i++) {
                if (started[i] == -1) {
                    continue;
                }
                String data = results[i];
                if (data == null) {
                    failed++;
                    System.out.println("    #" + i + " \u2717 no data");
                    continue;
                }
                JSONObject root = new JSONObject(data);
                JSONObject error = root.optJSONObject("error");
                if (error != null && !error.optString("code", "").isEmpty()) {
                    failed++;
                    System.out.println("    #" + i + " \u2717 " + error.optString("code"));
                    continue;
                }
                ok++;
                passed++;
                JSONObject session = root.optJSONObject("session");
                int streamId = session != null ? session.optInt("streamId", -1) : -1;
                ids.add(streamId);
                JSONObject response = root.optJSONObject("response");
                int nbytes = response != null ? response.optString("payload", "").length() : 0;
                System.out.println("    #" + i + " \u2713 " + nbytes + " bytes, stream " + streamId);
            }

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
            System.out.println("    Non-blocking test failed: " + e.getMessage());
        }

        System.out.println("\n[Cleanup] Cleaning up...");
        HttpClient.cleanup();
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
