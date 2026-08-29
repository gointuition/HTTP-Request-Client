//
//  HttpClient.h
//  HTTP
//  
//  Created by intuition on 2024/7/28.
//  Copyright © 2024. All rights reserved.
//  
    

#ifndef HttpClient_h
#define HttpClient_h

#include <stdint.h>

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

// Unified request entry: builds the basket and starts the request, returning
// the basket pointer as an intptr_t handle (0 on failure). The "non-blocking"
// field (default 1) selects the mode; either way the response is collected by
// passing the handle (and a caller-owned buffer) to handleResponse():
//   non-blocking (default): runs in the background; handleResponse() polls the
//   request until it completes. The registry owns the basket until the result
//   is fully copied out — only pass the handle to handleResponse().
//   blocking ("non-blocking": 0): the whole exchange finishes before returning;
//   handleResponse() copies the already-complete result into the buffer.
intptr_t handleRequest(const char *requestJSONString);

// ─── Response retrieval (unified) ───
// Collect the response for a handle returned by handleRequest(). The result
// is copied into the caller-owned buffer (dest/capacity); the caller allocates
// and frees it. If the buffer is too small the full result is kept in the
// basket, *outLen carries the complete length, and the same handle must be
// passed again with a buffer of at least outLen + 1 bytes.
//   outStatus: 0 = still in flight, 1 = fully copied, 2 = complete but
//              truncated (re-call with a bigger buffer), -1 = failed/timed out
//   outLen:    full length of the response JSON (valid for status 1, 2, -1)
// Once status is 1 (or -1 with the result copied) the handle is reclaimed and
// must not be used again.
void handleResponse(intptr_t basketHandle, char *dest, int capacity, int *outStatus, int *outLen);

// Reap all in-flight requests (call at shutdown).
void cleanupAsyncRequests(void);

void executeRequest(Basket *basket, Stream **outStream, int waitForResponse);

#ifdef __cplusplus
}
#endif

#endif /* HttpClient_h */
