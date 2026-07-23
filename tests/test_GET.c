//
// Created by Intuition on 25-10-25.
//

#include <stdio.h>

//#include "jansson.h"

#include "File.h"
#include "Http2Client.h"
#include "Log.h"

int main(int argc, char *argv[]) {

    initialiseEnv();

    char *requestStr = readFromFile("./request_GET.json");
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