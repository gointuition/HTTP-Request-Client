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

        // Cleanup
        System.out.println("\n[Cleanup] Cleaning up...");
        Http2Client.cleanup();
        System.out.println("[Cleanup] Done");

        System.out.println("\nAll tests completed!");
    }
}
