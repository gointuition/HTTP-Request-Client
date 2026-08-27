//
//  HttpClient.h
//  HTTP
//  
//  Created by intuition on 2024/7/28.
//  Copyright © 2024. All rights reserved.
//  
    

#ifndef HttpClient_h
#define HttpClient_h

#include "Basket.h"
#include "Version.h"

#ifdef __cplusplus
extern "C" {
#endif

//#include "ClientConnector.h"
//#include "temp/InetAddress.h"

//#include "temp/CacheInetAddress.h"

enum State {
    STARTING
};

typedef struct {
    int initialSessionRecvWindow;
    enum State state;
} HttpClient;

typedef struct {
    int statusCode;
    long sessionPtr;
    char *errorMsg;
} SessionResponse;

HttpClient* newHttpClient(void);

void initialiseEnv(void);
void cleanupEnv(void);

int connectTo(const char *hostname, const char *port);

char* handleRequest(const char *requestJSONString, int *outLen);

void getBasketContent(char *basketStr, char *dest);

// ─── Non-blocking / async request API ───
// Start a request (must carry "non-blocking": 1) without waiting for the
// response. Returns a positive request id to poll, or 0 on failure.
long handleRequestAsync(const char *requestJSONString);

// Poll an async request. Sets *outStatus to 0 (in flight), 1 (completed) or -1
// (failed/timed out), and *outLen to the length of the returned JSON string.
// Returns a malloc'd response JSON string only on completion (caller frees it);
// otherwise returns NULL. On completion the id is reclaimed.
char* pollRequest(long requestId, int *outStatus, int *outLen);

// Reap all in-flight async requests (call at shutdown).
void cleanupAsyncRequests(void);

void executeRequest(Basket *basket, Stream **outStream, int waitForResponse);

#ifdef __cplusplus
}
#endif

#endif /* HttpClient_h */
