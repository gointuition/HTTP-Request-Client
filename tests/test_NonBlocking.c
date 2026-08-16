//
// Created by Intuition on 26-8-16.
//
// Non-blocking concurrency test. Unlike test_Concurrency.c (which spawns one
// thread per request, each blocking in handleRequest), this test exercises the
// async surface: handleRequestAsync() fires every request and returns
// immediately, then a single thread reaps the results with pollRequest(). The
// number of in-flight requests is therefore not bounded by any thread count —
// a single thread drives all of them.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jansson.h"

#include "Compat.h"
#include "File.h"
#include "Http2Client.h"
#include "Log.h"

#define MAX_REQUESTS 64

static int validateBasket(const char *basketStr) {
    json_error_t jerr;
    json_t *root = json_loads(basketStr, 0, &jerr);
    if (root == NULL) {
        LOG("ERROR", "response is not valid JSON: %s (line %d)", jerr.text, jerr.line);
        return 0;
    }

    // error field must be an empty object
    json_t *err = json_object_get(root, "error");
    char *errStr = json_dumps(err, JSON_COMPACT);
    if (errStr == NULL || strcmp(errStr, "{}") != 0) {
        LOG("ERROR", "response error is not empty: %s", errStr ? errStr : "(null)");
        free(errStr);
        json_decref(root);
        return 0;
    }
    free(errStr);

    // response.payload must be a non-empty string
    json_t *response = json_object_get(root, "response");
    if (response == NULL) {
        LOG("ERROR", "response has no response field");
        json_decref(root);
        return 0;
    }
    json_t *payload = json_object_get(response, "payload");
    if (payload == NULL || !json_is_string(payload) || json_string_length(payload) == 0) {
        LOG("ERROR", "response.payload is empty or missing");
        json_decref(root);
        return 0;
    }

    json_decref(root);
    return 1;
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    int ret = EXIT_SUCCESS;

    initialiseEnv();

    char *requestStr = readFromFile("./request_Concurrency.json");
    if (requestStr == NULL) {
        cleanupEnv();
        return EXIT_FAILURE;
    }

    json_error_t jerr;
    json_t *config = json_loads(requestStr, 0, &jerr);
    if (config == NULL) {
        LOG("ERROR", "request json parse failed: %s", jerr.text);
        free(requestStr);
        cleanupEnv();
        return EXIT_FAILURE;
    }

    int concurrency = 8;
    json_t *concurrencyField = json_object_get(config, "concurrency");
    if (json_is_integer(concurrencyField)) {
        concurrency = (int) json_integer_value(concurrencyField);
    }
    if (concurrency < 1) { concurrency = 1; }
    if (concurrency > MAX_REQUESTS) { concurrency = MAX_REQUESTS; }

    // Force the non-blocking socket I/O mode for the async surface, and drop the
    // test-only "concurrency" field before handing the config to the core.
    json_object_set_new(config, "non-blocking", json_integer(1));
    json_object_del(config, "concurrency");

    char *requestJson = json_dumps(config, JSON_COMPACT);
    json_decref(config);
    free(requestStr);
    if (requestJson == NULL) {
        cleanupEnv();
        return EXIT_FAILURE;
    }

    printf("firing %d non-blocking requests...\n", concurrency);

    // Step 1: fire every request without blocking. This loop returns almost
    // instantly even though none of the responses have arrived yet.
    long ids[MAX_REQUESTS];
    int started = 0;
    for (int i = 0; i < concurrency; i++) {
        long id = handleRequestAsync(requestJson);
        if (id == 0) {
            LOG("ERROR", "failed to start request #%d (id=0)", i);
            ids[i] = 0;
        } else {
            ids[i] = id;
            started++;
        }
    }
    printf("started %d/%d requests in flight\n", started, concurrency);

    // Step 2: reap the results by polling from this single thread.
    int pending = started;
    int ok = 0;
    int failed = 0;
    while (pending > 0) {
        for (int i = 0; i < concurrency; i++) {
            if (ids[i] == 0) { continue; } // already reaped (or never started)

            int status = 0;
            int outLen = 0;
            char *result = pollRequest(ids[i], &status, &outLen);
            if (status == 0) {
                continue; // still in flight
            }
            pending--;

            if (status == 1 && result != NULL && outLen > 0) {
                char *dest = malloc(outLen + 1);
                if (dest == NULL) {
                    free(result);
                    failed++;
                    ids[i] = 0;
                    continue;
                }
                getBasketContent(result, dest); // copies into dest and frees result
                dest[outLen] = '\0';

                if (validateBasket(dest)) {
                    // extract stream id for the summary
                    json_t *root = json_loads(dest, 0, NULL);
                    int streamId = -1;
                    if (root != NULL) {
                        json_t *session = json_object_get(root, "session");
                        json_t *sid = session ? json_object_get(session, "streamId") : NULL;
                        if (json_is_integer(sid)) {
                            streamId = (int) json_integer_value(sid);
                        }
                        json_decref(root);
                    }
                    printf("  #%d OK stream %d\n", i, streamId);
                    ok++;
                } else {
                    printf("  #%d FAILED (invalid response)\n", i);
                    failed++;
                }
                free(dest);
            } else {
                printf("  #%d FAILED (status=%d)\n", i, status);
                if (result != NULL) {
                    free(result);
                }
                failed++;
            }
            ids[i] = 0; // mark reaped
        }
        if (pending > 0) {
            sleepMicroseconds(5000);
        }
    }

    printf("done: %d succeeded, %d failed\n", ok, failed);

    free(requestJson);
    cleanupEnv();

    if (failed > 0) {
        ret = EXIT_FAILURE;
    }
    return ret;
}
