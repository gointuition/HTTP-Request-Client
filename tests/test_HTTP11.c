//
// Created by Intuition on 26-8-23.
//
// HTTP/1.1 downgrade / fallback test:
//   1. request_HTTP11.json       https://… with "session": {"protocol": "http/1.1"}
//      (ALPN advertises http/1.1 only, so the whole exchange runs on the
//      serial HTTP/1.1 transport).
//   2. request_HTTP11_plain.json plain http:// URL (no TLS at all).
// Each request runs twice to exercise keep-alive session reuse.
//   3. async: fire several requests via handleRequestAsync() and reap them
//      with pollRequest(); on the serial HTTP/1.1 transport they queue on the
//      session's writeMutex and run one exchange at a time.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jansson.h"

#include "AsyncRequest.h"
#include "File.h"
#include "HttpClient.h"
#include "Log.h"

#define ASYNC_COUNT 4

static int validateBasket(const char *basketStr) {
    json_error_t jerr;
    json_t *root = json_loads(basketStr, 0, &jerr);
    if (root == NULL) {
        LOG("ERROR", "basket is not valid JSON: %s (line %d)", jerr.text, jerr.line);
        return 0;
    }

    // error field must be an empty object
    json_t *err = json_object_get(root, "error");
    char *errStr = json_dumps(err, JSON_COMPACT);
    if (errStr == NULL || strcmp(errStr, "{}") != 0) {
        LOG("ERROR", "basket error is not empty: %s", errStr ? errStr : "(null)");
        free(errStr);
        json_decref(root);
        return 0;
    }
    free(errStr);

    // response must carry the status in the ":status" header (HTTP/2 shape)
    json_t *response = json_object_get(root, "response");
    if (response == NULL) {
        LOG("ERROR", "basket has no response field");
        json_decref(root);
        return 0;
    }
    json_t *headers = json_object_get(response, "headers");
    json_t *status = headers != NULL ? json_object_get(headers, ":status") : NULL;
    if (status == NULL || !json_is_string(status) || strcmp(json_string_value(status), "200") != 0) {
        LOG("ERROR", "response :status is missing or not 200");
        json_decref(root);
        return 0;
    }

    // response.payload must be a non-empty string
    json_t *payload = json_object_get(response, "payload");
    if (payload == NULL || !json_is_string(payload) || json_string_length(payload) == 0) {
        LOG("ERROR", "response.payload is empty or missing");
        json_decref(root);
        return 0;
    }

    json_decref(root);
    return 1;
}

// Run one request file twice (2nd run must reuse the pooled keep-alive session)
// and validate both responses. Returns 1 on success.
static int runRequestFile(const char *path) {
    char *requestStr = readFromFile(path);
    if (requestStr == NULL) {
        LOG("ERROR", "failed to read %s", path);
        return 0;
    }

    int ok = 1;
    for (int attempt = 1; attempt <= 2; attempt++) {
        int actualLen = 0;
        char *result = handleRequest(requestStr, &actualLen);
        if (result == NULL || actualLen <= 0) {
            LOG("ERROR", "%s: failed to handle request (attempt %d)", path, attempt);
            ok = 0;
            break;
        }
        LOG("DEBUG", "%s: basket json string length %d (attempt %d)", path, actualLen, attempt);

        char *basketStr = malloc(actualLen + 1);
        if (!basketStr) {
            LOG("ERROR", "failed to allocate memory");
            free(result);
            ok = 0;
            break;
        }
        getBasketContent(result, basketStr);
        LOG("DEBUG", "basket json %s", basketStr);
        if (!validateBasket(basketStr)) {
            LOG("ERROR", "%s: validation failed (attempt %d)", path, attempt);
            ok = 0;
        }
        free(basketStr);
        if (!ok) {
            break;
        }
    }

    free(requestStr);
    return ok;
}

// Fire ASYNC_COUNT requests without blocking, then reap them from this single
// thread. Returns 1 when every request completes with a valid basket.
static int runRequestFileAsync(const char *path) {
    char *requestStr = readFromFile(path);
    if (requestStr == NULL) {
        LOG("ERROR", "failed to read %s", path);
        return 0;
    }

    long ids[ASYNC_COUNT];
    int started = 0;
    for (int i = 0; i < ASYNC_COUNT; i++) {
        ids[i] = handleRequestAsync(requestStr);
        if (ids[i] == 0) {
            LOG("ERROR", "%s: failed to start async request #%d", path, i);
        } else {
            started++;
        }
    }
    free(requestStr);
    if (started < ASYNC_COUNT) {
        return 0;
    }

    int ok = 1;
    int pending = started;
    while (pending > 0) {
        for (int i = 0; i < ASYNC_COUNT; i++) {
            if (ids[i] == 0) { continue; }

            int status = 0;
            int outLen = 0;
            char *result = pollRequest(ids[i], &status, &outLen);
            if (status == 0) {
                usleep(1000);
                continue;
            }
            pending--;
            ids[i] = 0;

            const int completed = status == 1 && result != NULL && outLen > 0;
            if (!completed) {
                LOG("ERROR", "%s: async request #%d failed (status %d)", path, i, status);
                free(result);
                ok = 0;
                continue;
            }

            char *basketStr = malloc(outLen + 1);
            if (basketStr == NULL) {
                LOG("ERROR", "failed to allocate memory");
                free(result);
                ok = 0;
                continue;
            }
            getBasketContent(result, basketStr);
            if (!validateBasket(basketStr)) {
                LOG("ERROR", "%s: async validation failed (#%d)", path, i);
                ok = 0;
            }
            free(basketStr);
        }
    }
    return ok;
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    int ret = EXIT_SUCCESS;

    initialiseEnv();

    // 1. forced HTTP/1.1 over TLS (ALPN advertises http/1.1 only)
    if (!runRequestFile("./request_HTTP11.json")) {
        ret = EXIT_FAILURE;
    }

    // 2. plain http:// (no TLS, HTTP/1.1 from the start)
    if (!runRequestFile("./request_HTTP11_plain.json")) {
        ret = EXIT_FAILURE;
    }

    // 3. async fire-and-forget over the serial HTTP/1.1 transport
    if (!runRequestFileAsync("./request_HTTP11.json")) {
        ret = EXIT_FAILURE;
    }
    if (!runRequestFileAsync("./request_HTTP11_plain.json")) {
        ret = EXIT_FAILURE;
    }

    cleanupEnv();

    return ret;
}
