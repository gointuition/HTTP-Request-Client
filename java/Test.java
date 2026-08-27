/*
 * Test.java
 *
 * HTTP Client Test
 *
 * Basic test for init, single request, and cleanup.
 */

import org.json.JSONObject;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

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

    public static void main(String[] args) {
        System.out.println("=== HTTP Client Test ===\n");

        int passed = 0;
        int failed = 0;

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        // Init
        System.out.println("[Init] Initializing HTTP client...");
        HttpClient.init();
        System.out.println("[Init] Initialized\n");

        // Test 1: Single request
        System.out.println("[Test 1] Single request...");
        try {
            String jsonStr = readJsonWithoutComments(projectRoot + "/bin/request_GET.json");
            long start = System.currentTimeMillis();
            String result = HttpClient.request(jsonStr);
            long elapsed = System.currentTimeMillis() - start;

            JSONObject resultObj = new JSONObject(result);
            JSONObject response = resultObj.optJSONObject("response");
            JSONObject error = resultObj.optJSONObject("error");

            if (error != null && error.has("code")) {
                System.out.println("    FAILED: error code " + error.optString("code", ""));
                failed++;
            } else if (response == null || response.optString("payload", "").isEmpty()) {
                System.out.println("    FAILED: response.payload is missing or empty");
                failed++;
            } else {
                passed++;
                System.out.println("    PASSED in " + elapsed + "ms");
                System.out.println("    URL: " + resultObj.optString("url", "N/A"));
            }

            if (response != null) {
                System.out.println("    Status: " + response.optString("statusCode", "N/A"));
            }
            if (error != null) {
                String msg = error.optString("message", "");
                if (!msg.isEmpty()) {
                    System.out.println("    Error: " + msg);
                }
            }
        } catch (Exception e) {
            failed++;
            System.out.println("    Failed: " + e.getMessage());
        }

        // Cleanup
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
