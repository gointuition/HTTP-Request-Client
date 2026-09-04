//
// Created by Intuition on 26-9-2.
//
// Session expiration verification: proves that session.expirationInMilliseconds
// is honoured as *milliseconds* (idle window from last use), not seconds.
//
// The connection pool stamps every response with session.streamId, an
// per-session counter that starts at 1 and grows by 2 (1, 3, 5, ...). It is
// therefore a deterministic reuse probe:
//   - reusing a pooled session  -> streamId keeps incrementing (3, 5, ...)
//   - establishing a new session -> streamId resets to 1
//
// Timeline (EXPIRATION_MS idle window):
//   run A  -> cold connect          (expect streamId 1)
//   +1s    (< EXPIRATION_MS)
//   run B  -> must reuse A          (expect streamId 3)
//   +6s    (> EXPIRATION_MS, idle)
//   run C  -> A must have been reaped, new session (expect streamId 1)
//
// With the units bug (ms compared against seconds) 6s idle is far below
// EXPIRATION_MS interpreted as seconds, so C would wrongly reuse the stale
// session (streamId 5) instead of opening a fresh one.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "jansson.h"

#include "HttpClient.h"
#include "Log.h"

// Idle window the pooled session must survive; short enough to test, long
// enough that a 1s gap still reuses.
#define EXPIRATION_MS 4000

// Build the blocking HTTP/1.1 request JSON with the given expiration.
static char *buildRequest(int expirationMs) {
    json_t *root = json_object();
    json_object_set_new(root, "method", json_string("GET"));
    json_object_set_new(root, "url", json_string("https://www.cloudflare.com/cdn-cgi/trace"));
    json_object_set_new(root, "connectTimeoutInMilliseconds", json_integer(3000));
    json_object_set_new(root, "responseReadingTimeoutInMilliseconds", json_integer(30000));
    json_object_set_new(root, "decompress", json_integer(15));
    json_object_set_new(root, "log", json_integer(0));
    json_object_set_new(root, "non-blocking", json_integer(0));

    json_t *headers = json_object();
    json_object_set_new(headers, "host", json_string("www.cloudflare.com"));
    json_object_set_new(headers, "user-agent", json_string("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36"));
    json_object_set_new(headers, "accept", json_string("*/*"));
    json_object_set_new(root, "headers", headers);

    json_t *session = json_object();
    json_object_set_new(session, "protocol", json_string("http/1.1"));
    json_object_set_new(session, "clientHelloId", json_string("hellochrome_auto"));
    json_object_set_new(session, "expirationInMilliseconds", json_integer(expirationMs));
    json_object_set_new(root, "session", session);

    return json_dumps(root, JSON_COMPACT);
}

// Run one blocking request and return the streamId assigned by the pool
// (>=1), or -1 on failure.
static int runOnce(const char *requestJson) {
    // NULL contract: the library collects the body, this test only reads the basket
    const intptr_t handle = handleRequest(requestJson, NULL);
    if (handle == 0) {
        printf("  handleRequest() returned 0\n");
        return -1;
    }

    int capacity = 256 * 1024;
    char *basketStr = malloc(capacity);
    if (basketStr == NULL) {
        return -1;
    }

    int status = 0;
    int len = 0;
    handleResponse(handle, basketStr, capacity, &status, &len);
    if (status == 2) {
        char *bigger = realloc(basketStr, (size_t) len + 1);
        if (bigger == NULL) {
            free(basketStr);
            return -1;
        }
        basketStr = bigger;
        handleResponse(handle, basketStr, len + 1, &status, &len);
    }
    if (status != 1) {
        printf("  request failed (status %d)\n", status);
        free(basketStr);
        return -1;
    }

    int streamId = -1;
    json_error_t jerr;
    json_t *root = json_loads(basketStr, 0, &jerr);
    free(basketStr);
    if (root == NULL) {
        printf("  basket is not valid JSON: %s\n", jerr.text);
        return -1;
    }

    // an error embedded in the basket means the exchange failed
    json_t *err = json_object_get(root, "error");
    json_t *errCode = err != NULL ? json_object_get(err, "code") : NULL;
    if (errCode != NULL && json_is_string(errCode)) {
        printf("  basket error: %s\n", json_string_value(errCode));
        json_decref(root);
        return -1;
    }

    json_t *session = json_object_get(root, "session");
    json_t *stream = session != NULL ? json_object_get(session, "streamId") : NULL;
    if (stream != NULL && json_is_integer(stream)) {
        streamId = (int) json_integer_value(stream);
    }
    json_decref(root);
    return streamId;
}

int main(void) {
    initialiseEnv();

    char *requestJson = buildRequest(EXPIRATION_MS);
    if (requestJson == NULL) {
        printf("FAIL: could not build request json\n");
        cleanupEnv();
        return EXIT_FAILURE;
    }

    int ok = 1;
    printf("expiration = %d ms\n", EXPIRATION_MS);

    // A: cold connect, must be a fresh session (streamId 1).
    int a = runOnce(requestJson);
    printf("[A] streamId = %d (expect 1, cold connect)\n", a);

    // small gap, well inside the idle window.
    sleep(1);

    // B: must reuse A's pooled session (streamId 3).
    int b = runOnce(requestJson);
    printf("[B] streamId = %d (expect 3, reuse within %dms)\n", b, EXPIRATION_MS);
    if (b != 3) {
        printf("  -> unexpected: idle reuse failed, expiration is too aggressive\n");
        ok = 0;
    }

    // idle past the expiration window so the reaper must drop the session.
    printf("  sleeping %ds ( > %dms idle window )...\n", EXPIRATION_MS / 1000 + 2, EXPIRATION_MS);
    sleep(EXPIRATION_MS / 1000 + 2);

    // C: the expired session must have been reaped -> fresh session (streamId 1).
    int c = runOnce(requestJson);
    printf("[C] streamId = %d (expect 1, expired session reaped)\n", c);
    if (c != 1) {
        printf("  -> BUG: stale session reused, expiration is not enforced in ms\n");
        ok = 0;
    }

    if (a != 1) {
        ok = 0;
    }

    free(requestJson);
    cleanupEnv();

    printf("%s\n", ok ? "PASS: session expiration honoured in milliseconds"
                      : "FAIL: session expiration not honoured as configured");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
