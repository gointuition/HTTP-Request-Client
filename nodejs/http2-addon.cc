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
    int handleRequest(const char *requestJSONString, char *basketStr, size_t basketStrLen);
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

    size_t bufferSize = 1024 * 1024;
    resultBuffer = (char *)malloc(bufferSize);
    if (!resultBuffer) {
        free(jsonStr);
        napi_throw_error(env, "ERR_NO_MEMORY", "Memory allocation failed");
        return nullptr;
    }
    actualRet = handleRequest(jsonStr, resultBuffer, bufferSize);

    free(jsonStr);

    // Create result object
    napi_value resultObj;
    napi_create_object(env, &resultObj);

    if (actualRet > 0) {
        // Success case
        napi_value dataValue;

        napi_create_string_utf8(env, resultBuffer, (size_t)actualRet, &dataValue);
        napi_set_named_property(env, resultObj, "data", dataValue);
    }

    if (resultBuffer) {
        free(resultBuffer);
    }

    return resultObj;
}

/**
 * Module initialization - DO NOT call initialiseEnv here
 */
napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
            {"initEnv", nullptr, InitEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"cleanupEnv", nullptr, CleanupEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
            {"handleRequest", nullptr, HandleRequest, nullptr, nullptr, nullptr, napi_default, nullptr}
    };

    napi_define_properties(env, exports, 3, desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
