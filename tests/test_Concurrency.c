//
// Created by Intuition on 26-7-28.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#include "jansson.h"

#include "File.h"
#include "HttpClient.h"

#define DEFAULT_CONCURRENCY 8

// One worker per concurrent request. All workers share the same request JSON and
// target the same host, so they are multiplexed over ONE connection (each gets
// its own odd stream id: 1, 3, 5, ...).
typedef struct {
    int         index;
    const char *requestStr;
    int         streamId;
    int         bytes;
    long        ms;
    char        errCode[64];
} Worker;

static long elapsedMs(struct timespec *from, struct timespec *to) {
    return (to -> tv_sec - from -> tv_sec) * 1000 + (to -> tv_nsec - from -> tv_nsec) / 1000000;
}

// Copy the response of a finished handle into a caller-owned buffer; on
// status 2 (complete but truncated) grow the buffer and re-collect. Returns
// a malloc'd NUL-terminated response JSON on status 1, otherwise NULL.
static char* collectResponse(intptr_t handle, int *outLen) {
    int capacity = 1024 * 1024;
    char *dest = malloc(capacity);
    if (dest == NULL) {
        return NULL;
    }

    int status = 0;
    int len = 0;
    handleResponse(handle, dest, capacity, &status, &len);
    if (status == 2) {
        char *bigger = realloc(dest, (size_t) len + 1);
        if (bigger == NULL) {
            free(dest);
            return NULL;
        }
        dest = bigger;
        capacity = len + 1;
        handleResponse(handle, dest, capacity, &status, &len);
    }

    if (status != 1) {
        free(dest);
        return NULL;
    }
    if (outLen != NULL) {
        *outLen = len;
    }
    return dest;
}

// Run one full request lifecycle and record the outcome into w.
static void doRequest(const char *requestStr, Worker *w) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    const intptr_t handle = handleRequest(requestStr);
    char *basketStr = NULL;
    if (handle != 0) {
        basketStr = collectResponse(handle, NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    w -> ms = elapsedMs(&t0, &t1);

    if (basketStr == NULL) {
        snprintf(w -> errCode, sizeof(w -> errCode), "NO_RESULT");
        return;
    }

    json_error_t error;
    json_t *root = json_loads(basketStr, 0, &error);
    free(basketStr);
    if (root == NULL) {
        snprintf(w -> errCode, sizeof(w -> errCode), "PARSE");
        return;
    }

    json_t *err = json_object_get(root, "error");
    if (json_is_object(err)) {
        json_t *code = json_object_get(err, "code");
        if (json_is_string(code)) {
            const char *c = json_string_value(code);
            if (c != NULL && c[0] != '\0') {
                snprintf(w -> errCode, sizeof(w -> errCode), "%s", c);
            }
        }
    }
    json_t *session = json_object_get(root, "session");
    if (json_is_object(session)) {
        json_t *sid = json_object_get(session, "streamId");
        if (json_is_integer(sid)) {
            w -> streamId = (int) json_integer_value(sid);
        }
    }
    json_t *response = json_object_get(root, "response");
    if (json_is_object(response)) {
        json_t *payload = json_object_get(response, "payload");
        if (json_is_string(payload)) {
            w -> bytes = (int) strlen(json_string_value(payload));
        }
    }

    json_decref(root);
}

static void *worker(void *arg) {
    Worker *w = (Worker *) arg;
    doRequest(w -> requestStr, w);
    return NULL;
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    // Writing to a peer-closed socket raises SIGPIPE, which would kill this
    // standalone process. Ignore it so SSL_write surfaces the error as a return
    // value instead (the Node.js/Java runtimes do this implicitly).
    // SIGPIPE is POSIX-only; on Windows/MinGW it is undefined (a write to a
    // closed socket already surfaces as an error), so guard the call.
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    initialiseEnv();

    char *requestStr = readFromFile("./request_Concurrency.json");
    if (requestStr == NULL) {
        cleanupEnv();
        return 1;
    }

    // "concurrency" is a test-only field; the native client ignores it. Read it
    // from the same JSON so this test and the Node.js test stay in sync.
    int concurrency = DEFAULT_CONCURRENCY;
    json_error_t jerr;
    json_t *cfg = json_loads(requestStr, 0, &jerr);
    if (cfg != NULL) {
        json_t *c = json_object_get(cfg, "concurrency");
        if (json_is_integer(c)) {
            concurrency = (int) json_integer_value(c);
        }
        json_decref(cfg);
    }
    if (concurrency < 1) {
        concurrency = 1;
    }

    // Warm-up: one request first to establish the shared connection. The
    // concurrent batch below then REUSES it, so we exercise multiplexing (many
    // streams on one connection) rather than a burst of cold connects (which
    // remote servers often throttle). Mirrors what the Node.js test relies on.
    printf("warming up the shared connection...\n");
    Worker warm;
    memset(&warm, 0, sizeof(warm));
    warm.streamId = -1;
    warm.requestStr = requestStr;
    doRequest(requestStr, &warm);
    if (warm.errCode[0] != '\0') {
        printf("warm-up FAILED %s (%ldms): could not establish the shared connection\n", warm.errCode, warm.ms);
        free(requestStr);
        cleanupEnv();
        return EXIT_FAILURE;
    }
    printf("warm-up OK (stream %d, %d bytes, %ldms); connection is now pooled\n", warm.streamId, warm.bytes, warm.ms);

    printf("firing %d concurrent requests (shared multiplexed connection)\n", concurrency);

    pthread_t *threads = malloc(sizeof(pthread_t) * concurrency);
    Worker *workers = calloc(concurrency, sizeof(Worker));
    if (threads == NULL || workers == NULL) {
        fprintf(stderr, "failed to allocate worker arrays\n");
        free(threads);
        free(workers);
        free(requestStr);
        cleanupEnv();
        return EXIT_FAILURE;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < concurrency; i++) {
        workers[i].index = i;
        workers[i].requestStr = requestStr;
        workers[i].streamId = -1;
        if (pthread_create(&threads[i], NULL, worker, &workers[i]) != 0) {
            fprintf(stderr, "failed to create worker thread #%d\n", i);
            snprintf(workers[i].errCode, sizeof(workers[i].errCode), "SPAWN");
            threads[i] = 0;
        }
    }
    for (int i = 0; i < concurrency; i++) {
        if (threads[i] != 0) {
            pthread_join(threads[i], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    long totalMs = elapsedMs(&start, &end);

    int ok = 0;
    for (int i = 0; i < concurrency; i++) {
        Worker *w = &workers[i];
        if (w -> errCode[0] != '\0') {
            printf("  #%d FAILED %s (%ldms)\n", w -> index, w -> errCode, w -> ms);
        } else {
            ok++;
            printf("  #%d OK %d bytes, stream %d (%ldms)\n", w -> index, w -> bytes, w -> streamId, w -> ms);
        }
    }
    printf("%d/%d succeeded, total wall time %ldms\n", ok, concurrency, totalMs);

    free(threads);
    free(workers);
    free(requestStr);

    cleanupEnv();

    return (ok == concurrency) ? 0 : EXIT_FAILURE;
}
