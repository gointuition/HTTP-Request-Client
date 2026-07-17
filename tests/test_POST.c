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

    size_t basketStrLen = 1024 * 1024;
    char *basketStr = malloc(basketStrLen);
    if (!basketStr) {
        LOG("ERROR", "failed to allocate memory");
        cleanupEnv();
        return EXIT_FAILURE;
    }

//    int actualLen = handleRequest(HTTP_REQUEST_JSON, basketStr, basketStrLen);
    int actualLen = handleRequest(requestStr, basketStr, basketStrLen);
    if (actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);
        LOG("DEBUG", "basket json %s", basketStr);
    } else {
        LOG("ERROR", "failed to handle request %d", actualLen);
    }
    free(basketStr);

//    sleep(5);

    basketStr = malloc(basketStrLen);
    if (!basketStr) {
        LOG("ERROR", "failed to allocate memory");
        cleanupEnv();
        return EXIT_FAILURE;
    }
//    actualLen = handleRequest(HTTP_REQUEST_JSON, basketStr, basketStrLen);
    actualLen = handleRequest(requestStr, basketStr, basketStrLen);
    if (actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);
        LOG("DEBUG", "basket json %s", basketStr);
    } else {
        LOG("ERROR", "failed to handle request %d", actualLen);
    }
    free(basketStr);
    free(requestStr);

    cleanupEnv();

    return 0;
}