//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>
#include <string.h>

#include "jansson.h"

#include "File.h"
#include "Http2Client.h"
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

int main(int argc, char *argv[]) {
    int ret = EXIT_SUCCESS;

    initialiseEnv();

    char *requestStr = readFromFile("./request_POST.json");
    if (requestStr == NULL) {
        return 1;
    }

//    printf("[DEBUG] basket json\n%s\n", requestStr);

    // step 1: get the result pointer and length
    int actualLen = 0;
    char *result = handleRequest(requestStr, &actualLen);
    if (result != NULL && actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);

        // step 2: allocate exact space and get content
        char *basketStr = malloc(actualLen + 1);
        if (!basketStr) {
            LOG("ERROR", "failed to allocate memory");
            free(result);
            cleanupEnv();
            return EXIT_FAILURE;
        }
        getBasketContent(result, basketStr);
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

    // step 1: get the result pointer and length
    actualLen = 0;
    result = handleRequest(requestStr, &actualLen);
    if (result != NULL && actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);

        // step 2: allocate exact space and get content
        char *basketStr = malloc(actualLen + 1);
        if (!basketStr) {
            LOG("ERROR", "failed to allocate memory");
            free(result);
            cleanupEnv();
            return EXIT_FAILURE;
        }
        getBasketContent(result, basketStr);
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