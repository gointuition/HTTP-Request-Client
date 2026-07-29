#include <node_api.h>
#include <stdlib.h>
#include <string.h>

// Forward-declare the C library API directly to avoid pulling in the full
// header chain (Basket.h -> pthread.h, openssl/ssl.h, ...) which requires
// POSIX/BoringSSL headers that MSVC does not ship. The addon only calls
// these three functions; it never touches the struct definitions.
extern "C" {
    void initialiseEnv(void);
    void cleanupEnv(void);
    char* handleRequest(const char *requestJSONString, int *outLen);
    void getBasketContent(char *basketStr, char *dest);
}

// Global initialization flag
static int envInitialized = 0;

/**
 * Initialize HTTP/2 client environment
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
 * Cleanup HTTP/2 client environment
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
 * Handle HTTP/2 request with automatic buffer management
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

    char *resultBuffer = nullptr;
    int actualRet = 0;

    char *result = handleRequest(jsonStr, &actualRet);

    free(jsonStr);

    // Create result object
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (result != nullptr && actualRet > 0) {
        // Allocate exact buffer and get content (getBasketContent frees result)
        resultBuffer = (char *)malloc(actualRet + 1);
        if (!resultBuffer) {
            free(result);
            napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
            return nullptr;
        }
        getBasketContent(result, resultBuffer);
        resultBuffer[actualRet] = '\0';

        // Success case
        napi_value dataValue;

        napi_create_string_utf8(env, resultBuffer, (size_t)actualRet, &dataValue);
        napi_set_named_property(env, resultObj, "data", dataValue);

        free(resultBuffer);
    }

    return resultObj;
}

// ─── Asynchronous request (napi_async_work + Promise) ───
//
// handleRequest() blocks the calling thread until the request completes, so
// running it directly on the JS thread serializes requests and stalls the event
// loop. Instead we copy the input JSON on the JS thread, run handleRequest on a
// libuv worker thread, and resolve a Promise on the JS thread with the result.
// Multiple pending Promises therefore execute concurrently across the pool, and
// the C core multiplexes same-host requests over one HTTP/2 connection.
struct AsyncRequest {
    napi_deferred   deferred;
    napi_async_work work;
    char            *jsonStr;   // input, owned by this struct until Complete
    char            *result;    // output from handleRequest
    int             resultLen;
};

// Runs on a libuv worker thread: MUST NOT touch napi_env / V8.
static void ExecuteRequest(napi_env env, void *data) {
    AsyncRequest *req = (AsyncRequest *)data;
    req->result = handleRequest(req->jsonStr, &req->resultLen);
}

// Runs back on the JS thread once ExecuteRequest returns.
static void CompleteRequest(napi_env env, napi_status status, void *data) {
    AsyncRequest *req = (AsyncRequest *)data;

    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (req->result != nullptr && req->resultLen > 0) {
        char *resultBuffer = (char *)malloc(req->resultLen + 1);
        if (resultBuffer) {
            getBasketContent(req->result, resultBuffer); // frees req->result
            resultBuffer[req->resultLen] = '\0';

            napi_value dataValue;
            napi_create_string_utf8(env, resultBuffer, (size_t)req->resultLen, &dataValue);
            napi_set_named_property(env, resultObj, "data", dataValue);
            free(resultBuffer);
        } else if (req->result) {
            free(req->result);
        }
    }

    // handleRequest embeds request-level errors in the JSON payload, mirroring
    // the synchronous path, so we always resolve (never reject) here.
    napi_resolve_deferred(env, req->deferred, resultObj);

    napi_delete_async_work(env, req->work);
    free(req->jsonStr);
    free(req);
}

/**
 * Handle HTTP/2 request asynchronously, returning a Promise. The blocking C
 * call runs on a libuv worker thread so concurrent requests run in parallel.
 */
napi_value HandleRequestAsync(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];

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

    napi_value promise;
    status = napi_create_promise(env, &req->deferred, &promise);
    if (status != napi_ok) {
        free(jsonStr);
        free(req);
        napi_throw_error(env, "ERR_PROMISE", "Failed to create promise");
        return nullptr;
    }

    napi_value resourceName;
    napi_create_string_utf8(env, "http2HandleRequest", NAPI_AUTO_LENGTH, &resourceName);
    status = napi_create_async_work(env, nullptr, resourceName,
                                    ExecuteRequest, CompleteRequest, req, &req->work);
    if (status != napi_ok) {
        free(jsonStr);
        free(req);
        napi_throw_error(env, "ERR_ASYNC_WORK", "Failed to create async work");
        return nullptr;
    }

    napi_queue_async_work(env, req->work);
    return promise;
}

/**
 * Module initialization - DO NOT call initialiseEnv here
 */
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
            {"initEnv", nullptr, InitEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"cleanupEnv", nullptr, CleanupEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"handleRequest", nullptr, HandleRequest, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"requestAsync", nullptr, HandleRequestAsync, nullptr, nullptr, nullptr, napi_default, nullptr}
    };

    napi_define_properties(env, exports, 4, desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
