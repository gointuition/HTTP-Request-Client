/*
 * Example.java
 *
 * HTTP/2 Client Example
 *
 * demonstrates GET, POST, and custom timeout requests.
 */

import org.json.JSONObject;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

public class Example {

    /**
     * Read a JSON file and strip comment lines (// or # style).
     * Mirrors read_json_without_comments() in python/example.py.
     */
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

    /**
     * Compact JSON serialization (no spaces) to match C library's JSON_COMPACT.
     */
    private static String compactJson(Object obj) {
        if (obj instanceof JSONObject) {
            return ((JSONObject) obj).toString();
        }
        return obj.toString();
    }

    public static void main(String[] args) {
        System.out.println("HTTP/2 Client Example\n");

        String projectRoot = System.getProperty("user.dir")
                .replace("/java", "").replace("\\java", "");

        // Example 1: Simple GET request
        System.out.println("Example 1: GET request");
        try {
            String jsonStr = readJsonWithoutComments(projectRoot + "/bin/request_GET.json");
            String result = Http2Client.request(jsonStr);
            JSONObject resultObj = new JSONObject(result);
            System.out.println(resultObj.toString(2));
        } catch (Exception e) {
            System.out.println("Error: " + e.getMessage());
        }

        // Example 2: POST request with JSON
        System.out.println("\nExample 2: POST request");
        try {
            String jsonStr = readJsonWithoutComments(projectRoot + "/bin/request_POST.json");
            JSONObject requestJson = new JSONObject(jsonStr);

            // Correct content-length: compact JSON (no spaces), byte length
            Object payload = requestJson.opt("payload");
            if (payload == null) payload = new JSONObject();
            String payloadStr = compactJson(payload);
            int byteLen = payloadStr.getBytes("UTF-8").length;
            requestJson.getJSONObject("headers").put("content-length", String.valueOf(byteLen));

            String result = Http2Client.request(requestJson.toString());
            JSONObject resultObj = new JSONObject(result);
            System.out.println(resultObj.toString(2));
        } catch (Exception e) {
            System.out.println("Error: " + e.getMessage());
        }

        // Example 3: Custom timeouts
        System.out.println("\nExample 3: Custom timeouts");
        try {
            String jsonStr = readJsonWithoutComments(projectRoot + "/bin/request_GET.json");
            JSONObject requestJson = new JSONObject(jsonStr);
            requestJson.put("url", "https://httpbin.org/delay/10");
            requestJson.put("connectTimeoutInMilliseconds", 5000);
            requestJson.put("responseReadingTimeoutInMilliseconds", 5000);

            String result = Http2Client.request(requestJson.toString());
            JSONObject resultObj = new JSONObject(result);
            System.out.println(resultObj.toString(2));
        } catch (Exception e) {
            System.out.println("Error: " + e.getMessage());
        }

        // Cleanup
        Http2Client.cleanup();
        System.out.println("\nDone!");
    }
}
