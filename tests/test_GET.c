//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "jansson.h"

#include "File.h"
#include "HttpClient.h"
#include "Log.h"

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

    // response.payload must be a non-empty string
    json_t *response = json_object_get(root, "response");
    if (response == NULL) {
        LOG("ERROR", "basket has no response field");
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

int main(int argc, char *argv[]) {
    int ret = EXIT_SUCCESS;

    initialiseEnv();

    char *requestStr = readFromFile("./request_GET.json");
    if (requestStr == NULL) {
        return 1;
    }

//    printf("[DEBUG] basket json\n%s\n", requestStr);

    // step 1: run the blocking request and collect the completed result
    int actualLen = 0;
    intptr_t handle = handleRequest(requestStr, NULL);
    char *basketStr = NULL;
    if (handle != 0) {
        basketStr = collectResponse(handle, &actualLen);
    }
    if (basketStr != NULL && actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);
        LOG("DEBUG", "basket json %s", basketStr);
        if (!validateBasket(basketStr)) {
            ret = EXIT_FAILURE;
        }
        free(basketStr);
    } else {
        LOG("ERROR", "failed to handle request (first)");
        ret = EXIT_FAILURE;
    }

//    sleep(5);

    // step 1: run the blocking request again (must reuse the pooled session)
    actualLen = 0;
    handle = handleRequest(requestStr, NULL);
    basketStr = NULL;
    if (handle != 0) {
        basketStr = collectResponse(handle, &actualLen);
    }
    if (basketStr != NULL && actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);
        LOG("DEBUG", "basket json %s", basketStr);
        if (!validateBasket(basketStr)) {
            ret = EXIT_FAILURE;
        }
        free(basketStr);
    } else {
        LOG("ERROR", "failed to handle request (second)");
        ret = EXIT_FAILURE;
    }
    free(requestStr);

    cleanupEnv();

    return ret;
}

//
// int main(int argc, char *argv[]) {
//     int ret = 0;
//
//     printf("%s\n", HTTP_REQUEST_JSON);
//     json_error_t error;
//     json_t *jsonRequest = json_loads(HTTP_REQUEST_JSON, 0, &error);
//     if (jsonRequest == NULL) {
//         printf("[ERROR] failed to %s\n", error.text);
//         ret = EXIT_FAILURE;
//     } else {
//         const char *url = json_string_value(json_object_get(jsonRequest, "url"));
//         // printf("[DEBUG] url: %s\n\n", url);
//
//         URLComponents components;
//         if (parseUrl(url, &components) == 0) {
//             printUrlComponents(&components);
//         } else {
//             printf("[ERROR] parsing url: %s\n", url);
//             ret = EXIT_FAILURE;
//         }
//
//         if (ret == 0) {
//             json_t *headers = json_object_get(jsonRequest, "headers");
//             const char *userAgent = json_string_value(json_object_get(headers, "user-agent"));
//             SessionResponse sessionRes = createSession(components.host, components.port, userAgent, 5000);
//             if (sessionRes.statusCode == NO) {
//                 printf("[ERROR] session creation failed\n");
//                 ret = EXIT_FAILURE;
//             } else {
//                 printf("[DEBUG] session created: %ld\n", sessionRes.sessionPtr);
//                 ResponseSS *response = sendRequest(sessionRes.sessionPtr, jsonRequest);
//                 if (response != NULL) {
//                     printf("[DEBUG] response status code: %d\n", response -> statusCode);
//                     if (strcmp(response -> errorMsg, "") != 0) {
//                         printf("[DEBUG] response error: %s\n", response -> errorMsg);
//                     }
//                     free(response -> responseHeaders);
//                     free(response -> responseBody);
//                     free(response);
//                 }
//
//                 int sessionStatus = closeSession(sessionRes.sessionPtr);
//                 printf("[DEBUG] session closed: %d\n", sessionStatus);
//             }
//         }
//
//         json_decref(jsonRequest);
//     }
//
//     return ret;
// }