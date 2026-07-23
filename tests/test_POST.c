//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>

#include "File.h"
#include "Http2Client.h"
#include "Log.h"

int main(int argc, char *argv[]) {

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
        free(basketStr);
    } else {
        LOG("ERROR", "failed to handle request");
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
        free(basketStr);
    } else {
        LOG("ERROR", "failed to handle request");
    }
    free(requestStr);

    cleanupEnv();

    return 0;
}