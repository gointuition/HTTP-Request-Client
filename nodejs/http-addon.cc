#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Mirrors Error (include/Error.h), ResponseHeader (include/Basket.h) and
// ResponseStream (include/ResponseStream.h). Declared here instead of included
// so MSVC never sees the project's POSIX/BoringSSL header chain; only the layout
// matters and it has to match field for field.
typedef struct {
    const char *code;
    const char *msg;
} HttpError;

typedef struct {
    char *name;
    char *value;
    int freeName;
    int freeValue;
} HttpHeader;

typedef struct {
    void (*onHeaders)(void *userData, const HttpHeader *headers, size_t numHeaders);
    int (*onData)(void *userData, const unsigned char *data, size_t len);
    void (*onComplete)(void *userData, HttpError error);
    void *userData;
} HttpStream;

// Forward-declare the C library API directly to avoid pulling in the full
// header chain (Basket.h -> pthread.h, openssl/ssl.h, ...) which requires
// POSIX/BoringSSL headers that MSVC does not ship.
extern "C" {
    void initialiseEnv(void);
    void cleanupEnv(void);
    // returns basket pointer as intptr_t, 0 = failure; stream = ResponseStream
    // contract, NULL lets the library collect the whole body itself
    intptr_t handleRequest(const char *requestJSONString, const HttpStream *stream);
    char* setNonBlocking(const char *requestJSONString, int nonBlocking);
    void freeJson(char *json);
    void handleResponse(intptr_t basketHandle, char *dest, int capacity, int *outStatus, int *outLen);
}

// Global initialization flag
static int envInitialized = 0;

/**
 * Initialize HTTP client environment
 */
napi_value InitEnv(napi_env env, napi_callback_info info) {
    if (!envInitialized) {
        initialiseEnv();
        envInitialized = 1;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

/**
 * Cleanup HTTP client environment
 */
napi_value CleanupEnv(napi_env env, napi_callback_info info) {
    if (envInitialized) {
        cleanupEnv();
        envInitialized = 0;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

/**
 * Handle HTTP request with automatic buffer management
 */
napi_value HandleRequest(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];

    napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, "ERR_INVALID_ARGS", "Expected 1 argument");
        return nullptr;
    }

    // Validate input type
    napi_valuetype valueType;
    napi_typeof(env, args[0], &valueType);
    if (valueType != napi_string) {
        napi_throw_type_error(env, "ERR_INVALID_TYPE", "Argument must be a string");
        return nullptr;
    }

    // Get JSON string length
    size_t jsonLength;
    status = napi_get_value_string_utf8(env, args[0], nullptr, 0, &jsonLength);
    if (status != napi_ok) {
        napi_throw_error(env, "ERR_STRING_LENGTH", "Failed to get string length");
        return nullptr;
    }

    // Allocate and copy JSON string
    char *jsonStr = (char *)malloc(jsonLength + 1);
    if (!jsonStr) {
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }

    size_t copied;
    status = napi_get_value_string_utf8(env, args[0], jsonStr, jsonLength + 1, &copied);
    if (status != napi_ok) {
        free(jsonStr);
        napi_throw_error(env, "ERR_STRING_COPY", "Failed to copy string");
        return nullptr;
    }
    jsonStr[jsonLength] = '\0';

    // Ensure environment is initialized (lazy initialization)
    if (!envInitialized) {
        initialiseEnv();
        envInitialized = 1;
    }

    int actualRet = 0;

    // Pin blocking mode: handleRequest picks the mode from "non-blocking"
    char *blockingJson = setNonBlocking(jsonStr, 0);
    free(jsonStr);

    char *result = nullptr;
    int resultStatus = 0;
    if (blockingJson != nullptr) {
        const intptr_t handle = handleRequest(blockingJson, nullptr);
        freeJson(blockingJson);
        if (handle != 0) {
            int capacity = 1024 * 1024;
            result = (char *)malloc(capacity);
            if (result != nullptr) {
                handleResponse(handle, result, capacity, &resultStatus, &actualRet);
                if (resultStatus == 2) {
                    // response bigger than the buffer: grow and re-collect
                    char *bigger = (char *)realloc(result, (size_t)actualRet + 1);
                    if (bigger != nullptr) {
                        result = bigger;
                        capacity = actualRet + 1;
                        handleResponse(handle, result, capacity, &resultStatus, &actualRet);
                    }
                }
            }
        }
    }

    // Create result object
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (resultStatus == 1 && result != nullptr && actualRet > 0) {
        // Success case
        napi_value dataValue;
        napi_create_string_utf8(env, result, (size_t)actualRet, &dataValue);
        napi_set_named_property(env, resultObj, "data", dataValue);
    }
    free(result);

    return resultObj;
}

// ─── Streaming response bridge ───
//
// The C core runs the ResponseStream contract on the thread that receives the
// bytes (the connection reader, or the thread running an HTTP/1.1 exchange),
// never on the JS thread, so every event is copied and posted through a
// threadsafe function.
//
// A JS callback cannot hand its verdict back synchronously: waiting for the JS
// thread inside the reader would stall the connection, and would deadlock
// outright whenever the JS thread is itself inside the library. "Stop" is
// therefore an atomic flag the JS callback sets, which the next chunk honours,
// so `return true` in JS still means "stop this response".
//
// Streaming is only offered on the two paths that leave the JS thread free
// (requestAsync and startRequest/pollRequest). The synchronous handleRequest
// blocks the event loop, so no callback could ever be delivered.

// One posted callback; owned by the JS thread once RunStreamEvent picks it up.
struct StreamEvent {
    enum Kind { HEADERS, DATA, COMPLETE };

    Kind kind;
    struct StreamBridge *bridge;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string chunk;
    std::string errorCode;
    std::string errorMessage;
    bool hasError;

    StreamEvent() : kind(HEADERS), bridge(nullptr), hasError(false) {}
};

struct StreamBridge {
    HttpStream contract;    // what handleRequest holds; userData points back here
    napi_threadsafe_function tsfn;
    napi_ref onHeaders;
    napi_ref onData;
    napi_ref onComplete;
    std::atomic<int> abortRequested;
    // Two owners drop a reference: the request that reaps the result, and the
    // threadsafe function once its queue has drained. Either may go first, so
    // the bridge dies with the last one instead of at a fixed point.
    std::atomic<int> refCount;
    std::atomic<int> tsfnRetired;
    std::atomic<int> ownerReleased;

    StreamBridge() : contract(), tsfn(nullptr), onHeaders(nullptr), onData(nullptr),
                     onComplete(nullptr), abortRequested(0), refCount(2),
                     tsfnRetired(0), ownerReleased(0) {}
};

// Non-blocking request id -> bridge, so polling can release it. JS thread only.
static std::unordered_map<intptr_t, StreamBridge *> pendingStreams;

static void PostStreamEvent(StreamBridge *bridge, StreamEvent *event);
static napi_value BuildHeaderObject(napi_env env, const StreamEvent *event);
static napi_value BuildErrorObject(napi_env env, const StreamEvent *event);
static int VerdictFromJs(napi_env env, napi_value result);
static void ReportStreamFailure(napi_env env, StreamBridge *bridge);
static void RunStreamEvent(napi_env env, napi_value jsCallback, void *context, void *data);
static void ReleaseStreamRefs(napi_env env, StreamBridge *bridge);
static void DropStreamBridge(napi_env env, StreamBridge *bridge);
static void RetireStreamBridge(StreamBridge *bridge);
static void ReleaseStreamBridge(napi_env env, StreamBridge *bridge);
static void FinalizeStreamBridge(napi_env env, void *finalizeData, void *finalizeHint);

static void OnStreamHeaders(void *userData, const HttpHeader *headers, size_t numHeaders) {
    StreamBridge *bridge = (StreamBridge *)userData;

    StreamEvent *event = new StreamEvent();
    event->kind = StreamEvent::HEADERS;
    event->bridge = bridge;
    for (size_t i = 0; i < numHeaders; i++) {
        if (headers[i].name == nullptr) { continue; }
        event->headers.emplace_back(headers[i].name, headers[i].value != nullptr ? headers[i].value : "");
    }
    PostStreamEvent(bridge, event);
}

static int OnStreamData(void *userData, const unsigned char *data, size_t len) {
    StreamBridge *bridge = (StreamBridge *)userData;
    if (bridge->abortRequested.load() != 0) { return 1; }

    // the library reuses its decode buffer, so the chunk is copied before it is
    // posted to the JS thread
    StreamEvent *event = new StreamEvent();
    event->kind = StreamEvent::DATA;
    event->bridge = bridge;
    event->chunk.assign((const char *)data, len);
    PostStreamEvent(bridge, event);
    return 0;
}

static void OnStreamComplete(void *userData, HttpError error) {
    StreamBridge *bridge = (StreamBridge *)userData;

    StreamEvent *event = new StreamEvent();
    event->kind = StreamEvent::COMPLETE;
    event->bridge = bridge;
    event->hasError = error.code != nullptr;
    if (event->hasError) {
        event->errorCode = error.code;
        event->errorMessage = error.msg != nullptr ? error.msg : "";
    }
    PostStreamEvent(bridge, event);
}

static void PostStreamEvent(StreamBridge *bridge, StreamEvent *event) {
    // blocking: a full queue must not drop body chunks, and every path that
    // streams keeps the JS thread free to drain it
    const napi_status status = napi_call_threadsafe_function(bridge->tsfn, event, napi_tsfn_blocking);
    if (status != napi_ok) {
        delete event;
    }
}

static napi_value BuildHeaderObject(napi_env env, const StreamEvent *event) {
    napi_value headers = nullptr;
    if (napi_create_object(env, &headers) != napi_ok) { return nullptr; }

    for (const auto &entry : event->headers) {
        napi_value value = nullptr;
        if (napi_create_string_utf8(env, entry.second.c_str(), entry.second.size(), &value) != napi_ok) {
            continue;
        }
        napi_set_named_property(env, headers, entry.first.c_str(), value);
    }
    return headers;
}

// null when the body ended cleanly, { code, message } otherwise: the shape the
// response JSON already uses.
static napi_value BuildErrorObject(napi_env env, const StreamEvent *event) {
    napi_value result = nullptr;
    if (!event->hasError) {
        napi_get_null(env, &result);
        return result;
    }

    if (napi_create_object(env, &result) != napi_ok) { return nullptr; }
    napi_value code = nullptr;
    napi_value message = nullptr;
    napi_create_string_utf8(env, event->errorCode.c_str(), event->errorCode.size(), &code);
    napi_create_string_utf8(env, event->errorMessage.c_str(), event->errorMessage.size(), &message);
    napi_set_named_property(env, result, "code", code);
    napi_set_named_property(env, result, "message", message);
    return result;
}

// Mirrors the C contract: a non-zero return asks the library to stop.
static int VerdictFromJs(napi_env env, napi_value result) {
    napi_valuetype type;
    if (napi_typeof(env, result, &type) != napi_ok) { return 0; }

    if (type == napi_boolean) {
        bool stop = false;
        napi_get_value_bool(env, result, &stop);
        return stop ? 1 : 0;
    }
    if (type == napi_number) {
        double value = 0;
        napi_get_value_double(env, result, &value);
        return value != 0 ? 1 : 0;
    }
    return 0;
}

// A throwing callback must not leave a pending exception on the JS thread, and
// the response is stopped: that consumer is no longer usable.
static void ReportStreamFailure(napi_env env, StreamBridge *bridge) {
    bridge->abortRequested.store(1);

    napi_value exception = nullptr;
    if (napi_get_and_clear_last_exception(env, &exception) != napi_ok) { return; }

    napi_value global = nullptr;
    napi_value console = nullptr;
    napi_value log = nullptr;
    if (napi_get_global(env, &global) != napi_ok) { return; }
    if (napi_get_named_property(env, global, "console", &console) != napi_ok) { return; }
    if (napi_get_named_property(env, console, "error", &log) != napi_ok) { return; }

    napi_value argv[1] = { exception };
    napi_value ignored = nullptr;
    napi_call_function(env, console, log, 1, argv, &ignored);
}

static void RunStreamEvent(napi_env env, napi_value jsCallback, void *context, void *data) {
    StreamEvent *event = (StreamEvent *)data;
    if (event == nullptr) { return; }
    StreamBridge *bridge = event->bridge;

    // env is NULL while the runtime tears down: nothing may be called any more
    if (env == nullptr) {
        delete event;
        return;
    }

    napi_ref *ref = &bridge->onComplete;
    napi_value argv[1] = { nullptr };
    if (event->kind == StreamEvent::HEADERS) {
        ref = &bridge->onHeaders;
        argv[0] = BuildHeaderObject(env, event);
    } else if (event->kind == StreamEvent::DATA) {
        ref = &bridge->onData;
        napi_create_buffer_copy(env, event->chunk.size(), event->chunk.data(), nullptr, &argv[0]);
    } else {
        argv[0] = BuildErrorObject(env, event);
    }

    if (*ref != nullptr && argv[0] != nullptr) {
        napi_value callback = nullptr;
        napi_value undefined = nullptr;
        napi_value result = nullptr;
        napi_get_reference_value(env, *ref, &callback);
        napi_get_undefined(env, &undefined);

        const napi_status status = napi_call_function(env, undefined, callback, 1, argv, &result);
        if (status != napi_ok) {
            ReportStreamFailure(env, bridge);
        } else if (event->kind == StreamEvent::DATA && VerdictFromJs(env, result) != 0) {
            bridge->abortRequested.store(1);
        }
    }

    const bool last = event->kind == StreamEvent::COMPLETE;
    delete event;
    if (last) { RetireStreamBridge(bridge); }
}

static void ReleaseStreamRefs(napi_env env, StreamBridge *bridge) {
    napi_ref *refs[3] = { &bridge->onHeaders, &bridge->onData, &bridge->onComplete };
    for (napi_ref *ref : refs) {
        if (*ref == nullptr) { continue; }
        napi_delete_reference(env, *ref);
        *ref = nullptr;
    }
}

static void DropStreamBridge(napi_env env, StreamBridge *bridge) {
    if (bridge == nullptr) { return; }
    if (bridge->refCount.fetch_sub(1) != 1) { return; }

    // env is NULL only while the runtime tears down, where the refs die with it
    if (env != nullptr) { ReleaseStreamRefs(env, bridge); }
    delete bridge;
}

// No further event may be posted. The ones already queued still run, so this
// never frees anything by itself.
static void RetireStreamBridge(StreamBridge *bridge) {
    if (bridge == nullptr) { return; }
    if (bridge->tsfnRetired.exchange(1) != 0) { return; }

    napi_release_threadsafe_function(bridge->tsfn, napi_tsfn_release);
}

// Called on the JS thread once the exchange is over: either onComplete ran, or
// the request failed before a stream was registered and no callback ever came.
static void ReleaseStreamBridge(napi_env env, StreamBridge *bridge) {
    if (bridge == nullptr) { return; }

    RetireStreamBridge(bridge);
    if (bridge->ownerReleased.exchange(1) != 0) { return; }
    DropStreamBridge(env, bridge);
}

static void FinalizeStreamBridge(napi_env env, void *finalizeData, void *finalizeHint) {
    (void)finalizeHint;
    DropStreamBridge(env, (StreamBridge *)finalizeData);
}

// Builds the bridge from args[first..first+2] (onHeaders, onData, onComplete).
// NULL when the caller passed no callback at all, which keeps the library's own
// collector and therefore a complete buffered response.
static StreamBridge* CreateStreamBridge(napi_env env, napi_value *args, size_t first) {
    StreamBridge *bridge = new StreamBridge();
    napi_ref *slots[3] = { &bridge->onHeaders, &bridge->onData, &bridge->onComplete };

    int callbacks = 0;
    for (int i = 0; i < 3; i++) {
        napi_value callback = args[first + i];
        if (callback == nullptr) { continue; }

        napi_valuetype type;
        if (napi_typeof(env, callback, &type) != napi_ok || type != napi_function) { continue; }
        if (napi_create_reference(env, callback, 1, slots[i]) != napi_ok) { continue; }
        callbacks++;
    }
    if (callbacks == 0) {
        delete bridge;
        return nullptr;
    }

    bridge->contract.onHeaders = OnStreamHeaders;
    bridge->contract.onData = OnStreamData;
    bridge->contract.onComplete = OnStreamComplete;
    bridge->contract.userData = bridge;

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "httpResponseStream", NAPI_AUTO_LENGTH, &resourceName);
    const napi_status status = napi_create_threadsafe_function(
        env, nullptr, nullptr, resourceName, 0, 1,
        bridge, FinalizeStreamBridge, nullptr, RunStreamEvent, &bridge->tsfn);
    if (status != napi_ok) {
        ReleaseStreamRefs(env, bridge);
        delete bridge;
        return nullptr;
    }
    return bridge;
}

// ─── Asynchronous request (napi_async_work + Promise) ───
//
// handleRequest() blocks the calling thread until the request completes, so
// running it directly on the JS thread serializes requests and stalls the event
// loop. Instead we copy the input JSON on the JS thread, run handleRequest on a
// libuv worker thread, and resolve a Promise on the JS thread with the result.
// Multiple pending Promises therefore execute concurrently across the pool, and
// the C core multiplexes same-host requests over one HTTP connection.
struct AsyncRequest {
    napi_deferred   deferred;
    napi_async_work work;
    char            *jsonStr;   // input, owned by this struct until Complete
    char            *result;    // output from handleRequest
    int             resultLen;
    StreamBridge    *bridge;    // streaming callbacks, NULL = buffered response
};

// Runs on a libuv worker thread: MUST NOT touch napi_env / V8.
static void ExecuteRequest(napi_env env, void *data) {
    AsyncRequest *req = (AsyncRequest *)data;
    // Pin blocking mode: handleRequest picks the mode from "non-blocking"
    char *blockingJson = setNonBlocking(req->jsonStr, 0);
    if (blockingJson == nullptr) {
        return;
    }
    const intptr_t handle = handleRequest(blockingJson,
                                         req->bridge != nullptr ? &req->bridge->contract : nullptr);
    freeJson(blockingJson);
    if (handle == 0) {
        return;
    }

    int capacity = 1024 * 1024;
    char *buffer = (char *)malloc(capacity);
    if (buffer == nullptr) {
        return;
    }
    int status = 0;
    handleResponse(handle, buffer, capacity, &status, &req->resultLen);
    if (status == 2) {
        // response bigger than the buffer: grow and re-collect
        char *bigger = (char *)realloc(buffer, (size_t)req->resultLen + 1);
        if (bigger != nullptr) {
            buffer = bigger;
            capacity = req->resultLen + 1;
            handleResponse(handle, buffer, capacity, &status, &req->resultLen);
        }
    }
    if (status == 1) {
        req->result = buffer;
    } else {
        req->resultLen = 0;
        free(buffer);
    }
}

// Runs back on the JS thread once ExecuteRequest returns.
static void CompleteRequest(napi_env env, napi_status status, void *data) {
    AsyncRequest *req = (AsyncRequest *)data;

    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (req->result != nullptr && req->resultLen > 0) {
        napi_value dataValue;
        napi_create_string_utf8(env, req->result, (size_t)req->resultLen, &dataValue);
        napi_set_named_property(env, resultObj, "data", dataValue);
    }

    // handleRequest embeds request-level errors in the JSON payload, mirroring
    // the synchronous path, so we always resolve (never reject) here.
    napi_resolve_deferred(env, req->deferred, resultObj);

    // the exchange is over: a request that failed before a stream was registered
    // never fires onComplete, so the bridge is released here as well
    ReleaseStreamBridge(env, req->bridge);
    req->bridge = nullptr;

    napi_delete_async_work(env, req->work);
    free(req->jsonStr);
    free(req->result);
    free(req);
}

/**
 * Handle HTTP request asynchronously, returning a Promise. The blocking C
 * call runs on a libuv worker thread so concurrent requests run in parallel.
 * Optional args 2-4 are the onHeaders / onData / onComplete streaming
 * callbacks; passing any of them streams the body instead of buffering it.
 */
napi_value HandleRequestAsync(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr, nullptr, nullptr, nullptr };

    napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, "ERR_INVALID_ARGS", "Expected 1 argument");
        return nullptr;
    }

    napi_valuetype valueType;
    napi_typeof(env, args[0], &valueType);
    if (valueType != napi_string) {
        napi_throw_type_error(env, "ERR_INVALID_TYPE", "Argument must be a string");
        return nullptr;
    }

    size_t jsonLength;
    status = napi_get_value_string_utf8(env, args[0], nullptr, 0, &jsonLength);
    if (status != napi_ok) {
        napi_throw_error(env, "ERR_STRING_LENGTH", "Failed to get string length");
        return nullptr;
    }

    // Copy the input JSON on the JS thread; the worker thread cannot call napi.
    char *jsonStr = (char *)malloc(jsonLength + 1);
    if (!jsonStr) {
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }
    size_t copied;
    status = napi_get_value_string_utf8(env, args[0], jsonStr, jsonLength + 1, &copied);
    if (status != napi_ok) {
        free(jsonStr);
        napi_throw_error(env, "ERR_STRING_COPY", "Failed to copy string");
        return nullptr;
    }
    jsonStr[jsonLength] = '\0';

    if (!envInitialized) {
        initialiseEnv();
        envInitialized = 1;
    }

    AsyncRequest *req = (AsyncRequest *)calloc(1, sizeof(AsyncRequest));
    if (!req) {
        free(jsonStr);
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }
    req->jsonStr = jsonStr;
    req->bridge = CreateStreamBridge(env, args, 1);

    napi_value promise;
    status = napi_create_promise(env, &req->deferred, &promise);
    if (status != napi_ok) {
        ReleaseStreamBridge(env, req->bridge);
        free(jsonStr);
        free(req);
        napi_throw_error(env, "ERR_PROMISE", "Failed to create promise");
        return nullptr;
    }

    napi_value resourceName;
    napi_create_string_utf8(env, "httpHandleRequest", NAPI_AUTO_LENGTH, &resourceName);
    status = napi_create_async_work(env, nullptr, resourceName,
                                    ExecuteRequest, CompleteRequest, req, &req->work);
    if (status != napi_ok) {
        ReleaseStreamBridge(env, req->bridge);
        free(jsonStr);
        free(req);
        napi_throw_error(env, "ERR_ASYNC_WORK", "Failed to create async work");
        return nullptr;
    }

    napi_queue_async_work(env, req->work);
    return promise;
}

// ─── Non-blocking request (fire-and-forget start + poll) ───
//
// Unlike requestAsync (which runs the fully-blocking handleRequest on a libuv
// worker), this path uses the C core's non-blocking surface: handleRequest on
// a non-blocking request sends HEADERS/DATA and returns immediately with a
// basket handle, and handleResponse checks/collects the result later when
// passed that handle. Both are fast, non-blocking calls that run on the JS thread,
// so the event loop never stalls and UV_THREADPOOL_SIZE is irrelevant. The JS
// layer drives the poll loop with a timer.

// Start a non-blocking request; returns { id } (id === 0 means start failed).
// Optional args 2-4 are the onHeaders / onData / onComplete streaming callbacks.
napi_value HandleRequestNonBlocking(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr, nullptr, nullptr, nullptr };

    napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, "ERR_INVALID_ARGS", "Expected 1 argument");
        return nullptr;
    }

    napi_valuetype valueType;
    napi_typeof(env, args[0], &valueType);
    if (valueType != napi_string) {
        napi_throw_type_error(env, "ERR_INVALID_TYPE", "Argument must be a string");
        return nullptr;
    }

    size_t jsonLength;
    status = napi_get_value_string_utf8(env, args[0], nullptr, 0, &jsonLength);
    if (status != napi_ok) {
        napi_throw_error(env, "ERR_STRING_LENGTH", "Failed to get string length");
        return nullptr;
    }

    char *jsonStr = (char *)malloc(jsonLength + 1);
    if (!jsonStr) {
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }
    size_t copied;
    status = napi_get_value_string_utf8(env, args[0], jsonStr, jsonLength + 1, &copied);
    if (status != napi_ok) {
        free(jsonStr);
        napi_throw_error(env, "ERR_STRING_COPY", "Failed to copy string");
        return nullptr;
    }
    jsonStr[jsonLength] = '\0';

    if (!envInitialized) {
        initialiseEnv();
        envInitialized = 1;
    }

    // Pin non-blocking mode; the basket handle is the id polled later.
    char *asyncJson = setNonBlocking(jsonStr, 1);
    free(jsonStr);

    StreamBridge *bridge = CreateStreamBridge(env, args, 1);

    intptr_t id = 0;
    if (asyncJson != nullptr) {
        id = handleRequest(asyncJson, bridge != nullptr ? &bridge->contract : nullptr);
        freeJson(asyncJson);
    }

    if (id == 0) {
        // nothing will ever call back, so the bridge must not be left behind
        ReleaseStreamBridge(env, bridge);
    } else if (bridge != nullptr) {
        // pollRequest releases it once the id is reaped
        pendingStreams[id] = bridge;
    }

    napi_value resultObj;
    napi_create_object(env, &resultObj);
    napi_value idValue;
    napi_create_int64(env, (int64_t)id, &idValue);
    napi_set_named_property(env, resultObj, "id", idValue);
    return resultObj;
}

// Poll a non-blocking request. Args: requestId (number).
// Returns { status: 0|1|-1, data?: string }.
napi_value PollRequest(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];

    napi_status status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, "ERR_INVALID_ARGS", "Expected 1 argument");
        return nullptr;
    }

    int64_t id = 0;
    status = napi_get_value_int64(env, args[0], &id);
    if (status != napi_ok) {
        napi_throw_type_error(env, "ERR_INVALID_TYPE", "Argument must be a number");
        return nullptr;
    }

    int capacity = 1024 * 1024;
    char *resultBuffer = (char *)malloc(capacity);
    if (resultBuffer == nullptr) {
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }

    int pollStatus = 0;
    int outLen = 0;
    handleResponse((intptr_t)id, resultBuffer, capacity, &pollStatus, &outLen);
    if (pollStatus == 2) {
        // response bigger than the buffer: grow and re-collect
        char *bigger = (char *)realloc(resultBuffer, (size_t)outLen + 1);
        if (bigger != nullptr) {
            resultBuffer = bigger;
            capacity = outLen + 1;
            handleResponse((intptr_t)id, resultBuffer, capacity, &pollStatus, &outLen);
        }
    }

    // the id is reaped, so a streaming bridge registered for it can go: this
    // also covers a request that failed before a stream was registered, where
    // onComplete never runs
    if (pollStatus != 0) {
        const auto pending = pendingStreams.find((intptr_t)id);
        if (pending != pendingStreams.end()) {
            ReleaseStreamBridge(env, pending->second);
            pendingStreams.erase(pending);
        }
    }

    napi_value resultObj;
    napi_create_object(env, &resultObj);
    napi_value statusValue;
    napi_create_int32(env, pollStatus, &statusValue);
    napi_set_named_property(env, resultObj, "status", statusValue);

    if (pollStatus != 0 && outLen > 0) {
        napi_value dataValue;
        napi_create_string_utf8(env, resultBuffer, (size_t)outLen, &dataValue);
        napi_set_named_property(env, resultObj, "data", dataValue);
    }
    free(resultBuffer);

    return resultObj;
}

/**
 * Module initialization - DO NOT call initialiseEnv here
 */
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
            {"initEnv", nullptr, InitEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"cleanupEnv", nullptr, CleanupEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"handleRequest", nullptr, HandleRequest, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"requestAsync", nullptr, HandleRequestAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"startRequest", nullptr, HandleRequestNonBlocking, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"pollRequest", nullptr, PollRequest, nullptr, nullptr, nullptr, napi_default, nullptr}
    };

    napi_define_properties(env, exports, 6, desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
