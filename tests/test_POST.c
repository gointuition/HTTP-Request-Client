//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>
#include <string.h>

#include "File.h"
#include "Http2Client.h"
#include "Log.h"

static int validateBasket(const char *basketStr) {
    if (strstr(basketStr, "\"error\":{}") == NULL) {
        LOG("ERROR", "basket error is not empty");
        return 0;
    }
    if (strstr(basketStr, "\"payload\":\"\"") != NULL
        || strstr(basketStr, "\"response\":") == NULL) {
        LOG("ERROR", "response.payload is empty or missing");
        return 0;
    }
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