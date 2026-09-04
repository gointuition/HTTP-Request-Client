//
// Created by Intuition on 26-8-31.
//
// Streaming response test (handleRequest with a ResponseStream contract):
//   1. request_Streaming.json over HTTP/2, whole body delivered chunk by chunk
//   2. same, stopped by the consumer after a few chunks (RST_STREAM path)
//   3. same over HTTP/1.1 (in-place body reader, idle deadline renewal)
//   4. same non-blocking (callbacks on the connection reader thread)
//   5. request_Streaming_gzip.json: gzip body decoded incrementally
//   6. a contract missing onData is refused with 3-0015, request never sent
// Every case asserts that the callbacks saw the body AND that the collected
// basket reports "streamed": 1 with an empty payload.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include "jansson.h"

#include "File.h"
#include "Error.h"
#include "HttpClient.h"
#include "Log.h"

#define ABORT_AFTER_BYTES (64 * 1024)
#define ABORTED_BY_CONSUMER_CODE "3-0014"
#define INCOMPLETE_CONTRACT_CODE "3-0015"

typedef enum {
    TRANSPORT_HTTP2,
    TRANSPORT_HTTP11,
    TRANSPORT_HTTP2_ASYNC
} Transport;

// What the callbacks saw; guarded because they run on another thread than the
// polling one.
typedef struct {
    const char *label;
    pthread_mutex_t lock;
    size_t bytes;
    size_t chunks;
    int headersSeen;
    int completes;
    Error error;
    size_t abortAfter;
    long long contentLength;
    const char *expectedPrefix; // decoded body must start with this, NULL = unchecked
    char head[8];
    size_t headLen;
    int encoded;                // response carried a content-encoding
} StreamStats;

typedef struct {
    char errorCode[16];
    char status[4];
    int streamed;
    size_t payloadLen;
} BasketView;

static void initStreamStats(StreamStats *stats, const char *label, size_t abortAfter,
                            const char *expectedPrefix);
static void destroyStreamStats(StreamStats *stats);
static void onHeaders(void *userData, const ResponseHeader *headers, size_t numHeaders);
static int onData(void *userData, const unsigned char *data, size_t len);
static void onComplete(void *userData, Error error);

static char* prepareRequestJson(const char *path, Transport transport);
static int runStreamingCase(const char *path, Transport transport, size_t abortAfterBytes,
                            const char *expectedPrefix);
static int runIncompleteContractCase(void);
static char* collectResult(intptr_t handle, int poll);
static long long headerLong(const ResponseHeader *headers, size_t numHeaders, const char *name);
static const char* headerString(const ResponseHeader *headers, size_t numHeaders, const char *name);
static int readBasketView(const char *basketStr, BasketView *view);
static int validateStreaming(StreamStats *stats, const BasketView *view, size_t abortAfterBytes);

int main(void) {
    int ret = EXIT_SUCCESS;

    // Writing to a peer-closed socket raises SIGPIPE, which would kill this
    // standalone process. Ignore it so SSL_write surfaces the error as a return
    // value instead (the Node.js/Java runtimes do this implicitly).
    // SIGPIPE is POSIX-only; on Windows/MinGW it is undefined (a write to a
    // closed socket already surfaces as an error), so guard the call.
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    initialiseEnv();

    if (!runStreamingCase("./request_Streaming.json", TRANSPORT_HTTP2, 0, NULL)) { ret = EXIT_FAILURE; }
    if (!runStreamingCase("./request_Streaming.json", TRANSPORT_HTTP2, ABORT_AFTER_BYTES, NULL)) { ret = EXIT_FAILURE; }
    if (!runStreamingCase("./request_Streaming.json", TRANSPORT_HTTP11, 0, NULL)) { ret = EXIT_FAILURE; }
    if (!runStreamingCase("./request_Streaming.json", TRANSPORT_HTTP2_ASYNC, 0, NULL)) { ret = EXIT_FAILURE; }
    // the gzip body must reach the consumer decoded
    if (!runStreamingCase("./request_Streaming_gzip.json", TRANSPORT_HTTP2, 0, "{")) { ret = EXIT_FAILURE; }
    // a half contract is refused instead of silently dropping the body
    if (!runIncompleteContractCase()) { ret = EXIT_FAILURE; }

    cleanupEnv();

    return ret;
}

static int runStreamingCase(const char *path, Transport transport, size_t abortAfterBytes,
                            const char *expectedPrefix) {
    char *requestJson = prepareRequestJson(path, transport);
    if (requestJson == NULL) {
        return 0;
    }

    StreamStats stats;
    initStreamStats(&stats, path, abortAfterBytes, expectedPrefix);
    const ResponseStream stream = { onHeaders, onData, onComplete, &stats };

    const intptr_t handle = handleRequest(requestJson, &stream);
    free(requestJson);
    if (handle == 0) {
        LOG("ERROR", "%s: failed to start the streaming request", path);
        destroyStreamStats(&stats);
        return 0;
    }

    const int poll = transport == TRANSPORT_HTTP2_ASYNC;
    char *basketStr = collectResult(handle, poll);
    destroyStreamStats(&stats);
    if (basketStr == NULL) {
        LOG("ERROR", "%s: no result collected", path);
        return 0;
    }

    BasketView view;
    memset(&view, 0, sizeof(view));
    const int parsed = readBasketView(basketStr, &view);
    free(basketStr);
    if (!parsed) {
        return 0;
    }
    return validateStreaming(&stats, &view, abortAfterBytes);
}

// A contract must carry all three callbacks: without onData the body would have
// neither a consumer nor a place in the buffered response, so handleRequest()
// refuses it before the request goes out and reports the error like any other
// invalid input.
static int runIncompleteContractCase(void) {
    const char *label = "incomplete contract";

    char *requestJson = prepareRequestJson("./request_Streaming.json", TRANSPORT_HTTP2);
    if (requestJson == NULL) {
        return 0;
    }

    StreamStats stats;
    initStreamStats(&stats, label, 0, NULL);
    const ResponseStream stream = { onHeaders, NULL, NULL, &stats };

    const intptr_t handle = handleRequest(requestJson, &stream);
    free(requestJson);
    if (handle == 0) {
        LOG("ERROR", "[%s] nothing was registered", label);
        destroyStreamStats(&stats);
        return 0;
    }

    char *basketStr = collectResult(handle, 0);
    const int callbacksSeen = stats.headersSeen + (int) stats.chunks + stats.completes;
    destroyStreamStats(&stats);
    if (basketStr == NULL) {
        LOG("ERROR", "[%s] no result collected", label);
        return 0;
    }

    BasketView view;
    memset(&view, 0, sizeof(view));
    int ok = readBasketView(basketStr, &view);
    free(basketStr);
    if (!ok) {
        return 0;
    }

    if (strcmp(view.errorCode, INCOMPLETE_CONTRACT_CODE) != 0) {
        LOG("ERROR", "[%s] basket error is %s, expected %s", label,
            view.errorCode[0] != '\0' ? view.errorCode : "none", INCOMPLETE_CONTRACT_CODE);
        ok = 0;
    }
    // the request never reached the network, so no callback may have run
    if (callbacksSeen != 0) {
        LOG("ERROR", "[%s] %d callbacks ran on a refused contract", label, callbacksSeen);
        ok = 0;
    }

    LOG("INFO", "[%s] %s: refused with %s", label, ok ? "passed" : "FAILED", view.errorCode);
    return ok;
}

// Rewrite the transport knobs of the request file so one file drives every case.
static char* prepareRequestJson(const char *path, Transport transport) {
    char *requestStr = readFromFile(path);
    if (requestStr == NULL) {
        LOG("ERROR", "failed to read %s", path);
        return NULL;
    }

    json_t *config = json_loads(requestStr, 0, NULL);
    free(requestStr);
    if (config == NULL) {
        LOG("ERROR", "%s: not valid JSON", path);
        return NULL;
    }

    const int nonBlocking = transport == TRANSPORT_HTTP2_ASYNC ? 1 : 0;
    json_object_set_new(config, "non-blocking", json_integer(nonBlocking));
    if (transport == TRANSPORT_HTTP11) {
        json_t *session = json_object_get(config, "session");
        json_object_set_new(session, "protocol", json_string("http/1.1"));
    }

    char *json = json_dumps(config, JSON_COMPACT);
    json_decref(config);
    return json;
}

// Pull the result out of the handle: the blocking path is already complete, the
// async one is polled until it finishes.
static char* collectResult(intptr_t handle, int poll) {
    int capacity = 64 * 1024;
    char *dest = malloc(capacity);
    if (dest == NULL) {
        return NULL;
    }

    int status = 0;
    int len = 0;
    for (;;) {
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
        if (status != 0 || !poll) {
            break;
        }
        usleep(1000);
    }

    if (status == 0) {
        LOG("ERROR", "the request never completed");
        free(dest);
        return NULL;
    }
    return dest;
}

// Read the few basket fields the assertions need.
static int readBasketView(const char *basketStr, BasketView *view) {
    json_error_t jerr;
    json_t *root = json_loads(basketStr, 0, &jerr);
    if (root == NULL) {
        LOG("ERROR", "result is not valid JSON: %s (line %d)", jerr.text, jerr.line);
        return 0;
    }

    const json_t *error = json_object_get(root, "error");
    const json_t *errorCode = error != NULL ? json_object_get(error, "code") : NULL;
    if (json_is_string(errorCode)) {
        strncpy(view -> errorCode, json_string_value(errorCode), sizeof(view -> errorCode) - 1);
    }

    const json_t *response = json_object_get(root, "response");
    const json_t *headers = response != NULL ? json_object_get(response, "headers") : NULL;
    const json_t *status = headers != NULL ? json_object_get(headers, ":status") : NULL;
    if (json_is_string(status)) {
        strncpy(view -> status, json_string_value(status), sizeof(view -> status) - 1);
    }

    const json_t *streamed = response != NULL ? json_object_get(response, "streamed") : NULL;
    view -> streamed = json_is_integer(streamed) ? (int) json_integer_value(streamed) : -1;

    const json_t *payload = response != NULL ? json_object_get(response, "payload") : NULL;
    view -> payloadLen = json_is_string(payload) ? json_string_length(payload) : (size_t) -1;

    json_decref(root);
    return 1;
}

// ─── callbacks ───

static void onHeaders(void *userData, const ResponseHeader *headers, size_t numHeaders) {
    StreamStats *stats = (StreamStats *) userData;

    pthread_mutex_lock(&stats -> lock);
    stats -> headersSeen = 1;
    stats -> contentLength = headerLong(headers, numHeaders, "content-length");
    const char *encoding = headerString(headers, numHeaders, "content-encoding");
    stats -> encoded = encoding != NULL && strcasecmp(encoding, "identity") != 0;
    const char *status = headerString(headers, numHeaders, ":status");
    LOG("INFO", "[%s] headers: status=%s encoding=%s length=%lld",
        stats -> label, status ? status : "-", encoding ? encoding : "-", stats -> contentLength);
    pthread_mutex_unlock(&stats -> lock);
}

static int onData(void *userData, const unsigned char *data, size_t len) {
    (void) data;
    StreamStats *stats = (StreamStats *) userData;

    pthread_mutex_lock(&stats -> lock);
    stats -> bytes += len;
    stats -> chunks++;
    const size_t room = sizeof(stats -> head) - stats -> headLen;
    if (room > 0) {
        const size_t take = len < room ? len : room;
        memcpy(stats -> head + stats -> headLen, data, take);
        stats -> headLen += take;
    }
    const size_t bytes = stats -> bytes;
    const size_t abortAfter = stats -> abortAfter;
    pthread_mutex_unlock(&stats -> lock);

    if (abortAfter > 0 && bytes >= abortAfter) {
        LOG("INFO", "[%s] %zu bytes received, stopping the response", stats -> label, bytes);
        return 1; // non-zero asks the library to tear the stream down
    }
    return 0;
}

static void onComplete(void *userData, Error error) {
    StreamStats *stats = (StreamStats *) userData;

    pthread_mutex_lock(&stats -> lock);
    stats -> completes++;
    stats -> error = error;
    LOG("INFO", "[%s] completed: %zu bytes in %zu chunks, error=%s",
        stats -> label, stats -> bytes, stats -> chunks, error.code ? error.code : "none");
    pthread_mutex_unlock(&stats -> lock);
}

static void initStreamStats(StreamStats *stats, const char *label, size_t abortAfter,
                            const char *expectedPrefix) {
    memset(stats, 0, sizeof(*stats));
    stats -> label = label;
    stats -> abortAfter = abortAfter;
    stats -> expectedPrefix = expectedPrefix;
    stats -> contentLength = -1;
    stats -> error = ERR_NONE;
    pthread_mutex_init(&stats -> lock, NULL);
}

static void destroyStreamStats(StreamStats *stats) {
    pthread_mutex_destroy(&stats -> lock);
}

// ─── assertions ───

static int bodyStartsAsExpected(const StreamStats *stats) {
    if (stats -> expectedPrefix == NULL) { return 1; }

    const size_t prefixLen = strlen(stats -> expectedPrefix);
    const int longEnough = stats -> headLen >= prefixLen;
    const int matched = longEnough && memcmp(stats -> head, stats -> expectedPrefix, prefixLen) == 0;
    if (!matched) {
        LOG("ERROR", "[%s] body starts with '%.*s', expected '%s'", stats -> label,
            (int) stats -> headLen, stats -> head, stats -> expectedPrefix);
    }
    return matched;
}

static int validateStreaming(StreamStats *stats, const BasketView *view, size_t abortAfterBytes) {
    const int aborted = abortAfterBytes > 0;
    int ok = 1;

    const int expectedError = aborted && strcmp(view -> errorCode, ABORTED_BY_CONSUMER_CODE) == 0;
    const int cleanError = !aborted && view -> errorCode[0] == '\0';
    if (!expectedError && !cleanError) {
        LOG("ERROR", "[%s] unexpected basket error: %s", stats -> label, view -> errorCode);
        ok = 0;
    }
    if (!aborted && stats -> error.code != NULL) {
        LOG("ERROR", "[%s] onComplete reported %s (%s)", stats -> label,
            stats -> error.code, stats -> error.msg);
        ok = 0;
    }
    if (view -> streamed != 1) {
        LOG("ERROR", "[%s] response.streamed is %d, expected 1", stats -> label, view -> streamed);
        ok = 0;
    }
    if (view -> payloadLen != 0) {
        LOG("ERROR", "[%s] response.payload must be empty, got %zu bytes", stats -> label, view -> payloadLen);
        ok = 0;
    }
    // an aborted request is finalized on the error path, which keeps no headers
    if (!aborted && strcmp(view -> status, "200") != 0) {
        LOG("ERROR", "[%s] :status is %s, expected 200", stats -> label, view -> status);
        ok = 0;
    }
    if (stats -> completes != 1) {
        LOG("ERROR", "[%s] onComplete ran %d times, expected 1", stats -> label, stats -> completes);
        ok = 0;
    }
    if (stats -> bytes == 0) {
        LOG("ERROR", "[%s] no body byte delivered", stats -> label);
        ok = 0;
    }
    if (aborted && stats -> bytes < abortAfterBytes) {
        LOG("ERROR", "[%s] stopped after %zu bytes, expected at least %zu", stats -> label,
            stats -> bytes, abortAfterBytes);
        ok = 0;
    }
    if (aborted && stats -> error.code != NULL && strcmp(stats -> error.code, ABORTED_BY_CONSUMER_CODE) != 0) {
        LOG("ERROR", "[%s] onComplete reported %s, expected %s", stats -> label,
            stats -> error.code, ABORTED_BY_CONSUMER_CODE);
        ok = 0;
    }
    // an uncompressed body must arrive complete
    const int sizeKnown = !aborted && !stats -> encoded && stats -> contentLength > 0;
    if (sizeKnown && (long long) stats -> bytes != stats -> contentLength) {
        LOG("ERROR", "[%s] delivered %zu of %lld advertised bytes", stats -> label,
            stats -> bytes, stats -> contentLength);
        ok = 0;
    }
    if (!bodyStartsAsExpected(stats)) {
        ok = 0;
    }

    LOG("INFO", "[%s] %s: %zu bytes in %zu chunks", stats -> label,
        ok ? "passed" : "FAILED", stats -> bytes, stats -> chunks);
    return ok;
}

// ─── header helpers ───

static const char* headerString(const ResponseHeader *headers, size_t numHeaders, const char *name) {
    for (size_t i = 0; i < numHeaders; i++) {
        const int match = headers[i].name != NULL && strcasecmp(headers[i].name, name) == 0;
        if (match) {
            return headers[i].value;
        }
    }
    return NULL;
}

static long long headerLong(const ResponseHeader *headers, size_t numHeaders, const char *name) {
    const char *value = headerString(headers, numHeaders, name);
    if (value == NULL) { return -1; }
    return strtoll(value, NULL, 10);
}
