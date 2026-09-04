//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "File.h"
#include "HttpClient.h"
#include "Log.h"

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

    initialiseEnv();

    char *requestStr = readFromFile("./request_PUT.json");
    if (requestStr == NULL) {
        return 1;
    }

    // step 1: run the blocking request and collect the completed result
    int actualLen = 0;
    const intptr_t handle = handleRequest(requestStr, NULL);
    char *basketStr = NULL;
    if (handle != 0) {
        basketStr = collectResponse(handle, &actualLen);
    }
    if (basketStr != NULL && actualLen > 0) {
        LOG("DEBUG", "basket json string length %d", actualLen);
        LOG("DEBUG", "basket json %s", basketStr);
        free(basketStr);
    } else {
        LOG("ERROR", "failed to handle request");
    }
    free(requestStr);

    cleanupEnv();

    return 0;
}
