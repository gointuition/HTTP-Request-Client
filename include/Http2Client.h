//
//  Http2Client.h
//  HTTP2
//  
//  Created by intuition on 2024/7/28.
//  Copyright © 2024. All rights reserved.
//  
    

#ifndef Http2Client_h
#define Http2Client_h

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
} Http2Client;

typedef struct {
    int statusCode;
    long sessionPtr;
    char *errorMsg;
} SessionResponse;

Http2Client* newHttp2Client(void);

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

// Prepare + send a request into a registered stream without waiting for the
// response. On success returns 0 and sets *outStream; the caller owns both the
// basket and the stream (used by the async request registry). On failure
// returns -1 and basket->error is set.
int http2StartRequest(Basket *basket, Stream **outStream);

#ifdef __cplusplus
}
#endif

#endif /* Http2Client_h */
