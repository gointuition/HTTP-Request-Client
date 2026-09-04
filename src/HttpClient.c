//
//  HttpClient.c
//  HTTP
//
//  Created by intuition on 2024/7/28.
//  Copyright © 2024. All rights reserved.
//
    
/**
 1. implement TLS connection
 2. implement HTTP/2 frame
 3. implement HPACK header compression
 4. implement Stream control
 */

#include "HttpClient.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <zlib.h>

#include "Compat.h"

#include "SocketHandler.h"
#include "Error.h"
#include "CompressHandler.h"
#include "Session.h"
// #include "FrameHandler.h"
#include "Http2RequestHandler.h"
#include "Http2ResponseHandler.h"
#include "Http11RequestHandler.h"
#include "Log.h"

#define SESSION_MAGIC 0X55AA1234
#define SESSION_ACTIVE 1
#define SESSION_INACTIVE 0

#define MAX_ASYNC_REQUESTS 1024

// request registry entry (see handleRequest / handleResponse)
typedef struct {
    long        requestId;
    int         inUse;
    Basket      *basket;
    Stream      *stream;    // NULL until the request has been sent
    time_t      startTime;  // wall-clock start, used for the reading timeout
} AsyncRequestEntry;

static AsyncRequestEntry asyncRegistry[MAX_ASYNC_REQUESTS];
static pthread_mutex_t asyncRegistryMutex = PTHREAD_MUTEX_INITIALIZER;
static long nextRequestId = 1;

// TODO log

HttpClient httpClient = {
    .initialSessionRecvWindow = 15663105 + 65535
};

//HashTable_InetAddress *tableInetAddress = NULL;

static intptr_t handleRequestSync(Basket *basket);
static intptr_t handleRequestAsync(Basket *basket);
static void handleHttp2Request(Basket *basket);
static int isRetryableError(Error error);
static void prepareRetry(Basket *basket);
static int requiresHttp11Downgrade(Error error);
static int isProxyFailureError(Error error);
static int copyBasketResult(Basket *basket, char *dest, int capacity, int *outLen);
static void handleResponseSync(Basket *basket, char *dest, int capacity, int *outStatus, int *outLen);
static void handleResponseAsync(Basket *basket, char *dest, int capacity, int *outStatus, int *outLen);
static AsyncRequestEntry *asyncAllocEntry(void);
static long asyncAllocSlot(void);
static AsyncRequestEntry *asyncFindSlot(long requestId);
static AsyncRequestEntry *asyncFindSlotByBasket(Basket *basket);
static void asyncDetachSlot(AsyncRequestEntry *entry, Basket **outBasket, Stream **outStream);

HttpClient* newHttpClient(void) {
//    printf("new HttpClient\n");
//    newClientConnector();
    return &httpClient;
}

void initialiseEnv(void) {
#ifdef _WIN32
    // Winsock must be initialised before any socket call
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    initSharedSessionPool();
    buildHuffmanTree();

    // (&httpClient) -> state = STARTING;
    // if (tableInetAddress == NULL) {
    //     tableInetAddress = createHashTable_InetAddress(8);
    // }
    // printf("httpClient is starting\n");
}

void cleanupEnv(void) {
    // Reap any in-flight async requests before tearing down sessions so their
    // streams are unregistered/freed while the sessions are still alive.
    cleanupAsyncRequests();
    cleanupSessions(1);
    cleanupTLSSessionCache();

#ifdef _WIN32
    WSACleanup();
#endif

    // freeHashTable_InetAddress(tableInetAddress);
    // tableInetAddress = NULL;
}

//int connectTo(const char *hostname, const char *port) {
//    InetAddress *inetAddress = get_InetAddress(tableInetAddress, hostname);
//    if (inetAddress == NULL) {
//        printf("Not from cache\n"); // TODO
//        inetAddress = getInetAddressBy(hostname, port);
//        if (inetAddress != NULL) {
//            put_InetAddress(tableInetAddress, hostname, inetAddress, sizeof(InetAddress), 86400);
//        }
//    }
//    if (inetAddress == NULL) {
//        return -1;
//    }
//    // [2001:0db8:85a3:0000:0000:8a2e:0370:7334]:443
//    printf("hostname: %s, port: %s, address: %s, version: %s\n", inetAddress -> hostname, inetAddress -> port, inetAddress -> address, inetAddress -> version); // TODO
//
//
//
//    freeInetAddress(inetAddress);
//    inetAddress = NULL;
//    return 1;
//}

// Unified request entry: builds the basket and starts the request, returning
// the basket pointer as an intptr_t handle (0 on failure). The "non-blocking"
// field (default 1) selects the mode; either way the response is collected by
// passing the handle to handleResponse():
//   non-blocking: the request runs in the background and handleResponse()
//   polls it. The registry owns the basket until it is reaped — only pass the
//   handle to handleResponse().
//   blocking ("non-blocking": 0): the whole exchange finishes before returning;
//   handleResponse() serializes the already-complete basket and frees it (call
//   it exactly once).
// Every response travels the streaming funnel; `stream` only decides who
// receives the body: NULL installs the library's own collector, which
// accumulates the decoded body into the basket payload so handleResponse()
// returns the complete response, while a contract gets it chunk by chunk and
// the collected JSON then reports "streamed": 1 with no payload.
intptr_t handleRequest(const char *requestJSONString, const ResponseStream *stream) {
    Basket *basket = buildBasket(requestJSONString);
    if (basket == NULL) {
        return 0;
    }

    // the contract is all or nothing: a partial one would leave the body with
    // neither a consumer nor a place in the basket, so no request runs with it
    const ResponseStream *contract = stream;
    if (contract != NULL && isCompleteResponseStream(contract) == 0) {
        LOG("ERROR", "incomplete ResponseStream, the request is not sent");
        if (basket -> error.code == NULL) {
            basket -> error = ERR_RESPONSE_STREAM_INCOMPLETE_CONTRACT;
        }
        contract = NULL;
    }

    basket -> sink = buildResponseSink(basket, contract);
    if (basket -> sink == NULL) {
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
    }

    if (basket -> nonBlocking) {
        return handleRequestAsync(basket);
    }
    return handleRequestSync(basket);
}

// Response collection, unified for both modes: dispatch on basket->nonBlocking.
// Non-blocking requests are polled via the registry; blocking baskets are
// already complete.
// The result is copied into the caller-owned buffer; if it does not fit, the
// full JSON stays cached in the basket, *outLen carries the complete length
// and the caller re-calls with a larger buffer.
// *outStatus: 0 = in flight, 1 = fully copied, 2 = complete but truncated
// (re-call with a bigger buffer), -1 = failed.
void handleResponse(intptr_t basketHandle, char *dest, int capacity, int *outStatus, int *outLen) {
    if (outStatus) { *outStatus = 0; }
    if (outLen) { *outLen = 0; }

    Basket *basket = (Basket *) basketHandle;
    if (basket == NULL || dest == NULL || capacity <= 0) {
        if (outStatus) { *outStatus = -1; }
        return;
    }

    if (basket -> nonBlocking) {
        handleResponseAsync(basket, dest, capacity, outStatus, outLen);
        return;
    }
    handleResponseSync(basket, dest, capacity, outStatus, outLen);
}

// Blocking exchange with one retry for connection-level errors. The returned
// handle points at the completed basket (response finalized or error embedded);
// the caller collects it via handleResponse(). No registry entry and no
// request id are needed.
static intptr_t handleRequestSync(Basket *basket) {
    if (basket -> error.code == NULL) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            Stream *stream = NULL;
            executeRequest(basket, &stream, 1);

            // a successfully registered stream must be unregistered after the
            // exchange; on failure executeRequest already cleaned it up
            if (stream != NULL) {
                unregisterStream(basket, stream);
            }

            // connection-level GOAWAY / SETTINGS_TIMEOUT / HTTP_1_1_REQUIRED are
            // retryable once
            const int retryable = isRetryableError(basket -> error);
            if (retryable && attempt == 0) {
                prepareRetry(basket);
                continue;
            }

            // proxy failures are not retryable (tunnel already invalidated)
            const int proxyFailure = isProxyFailureError(basket -> error);
            if (proxyFailure) {
                LOG("WARN", "proxy error, not retrying: %s", basket -> error.code);
            }
            break;
        }
    }
    return (intptr_t) basket;
}

// Fire-and-forget: start the request without waiting and publish it to the
// registry; the returned handle is later passed to handleResponse() to reap
// the result.
static intptr_t handleRequestAsync(Basket *basket) {
    AsyncRequestEntry *entry = asyncAllocEntry();
    if (entry == NULL) {
        LOG("ERROR", "request registry full (max %d)", MAX_ASYNC_REQUESTS);
        freeBasket(basket);
        return 0;
    }
    basket -> requestId = entry -> requestId;

    Stream *stream = NULL;
    if (basket -> error.code == NULL) {
        // connection-level GOAWAY / SETTINGS_TIMEOUT / HTTP_1_1_REQUIRED are
        // retryable once, same as the blocking path
        for (int attempt = 0; attempt < 2; ++attempt) {
            executeRequest(basket, &stream, 0);

            const int retryable = isRetryableError(basket -> error);
            if (retryable && attempt == 0) {
                // a failed attempt may leave a registered stream behind
                if (stream != NULL) {
                    unregisterStream(basket, stream);
                    stream = NULL;
                }
                prepareRetry(basket);
                continue;
            }
            break;
        }
        if (basket -> error.code != NULL) {
            // executeRequest left basket->error set; the request stays
            // registered so handleResponse() surfaces it as completed-with-error.
            stream = NULL;
        }
    }

    // Publish the basket/stream to the slot under the lock so a concurrent
    // handleResponse() on another thread never observes a half-written entry.
    // A poll that arrives before this point sees basket == NULL and reports
    // "still in flight", which is correct.
    pthread_mutex_lock(&asyncRegistryMutex);
    entry -> basket = basket;
    entry -> stream = stream;
    entry -> startTime = time(NULL);
    pthread_mutex_unlock(&asyncRegistryMutex);

    return (intptr_t) basket;
}

// Full request flow: establish/reuse a session, register a multiplexed stream,
// dispatch the request over HTTP/1.1 or HTTP/2. In sync mode (waitForResponse)
// also wait for the response and finalize it into the basket (and decompress);
// in async mode return immediately and let the caller poll the stream. On
// success *outStream is set to the registered stream; failure is reported via
// basket->error (*outStream stays NULL, the basket is NOT freed here so the
// caller can serialize the error).
void executeRequest(Basket *basket, Stream **outStream, int waitForResponse) {
    *outStream = NULL;

    // 1. obtain/reuse a session (a new connection starts its own reader thread
    //    that demultiplexes frames by stream id)
    handleSession(basket);
    if (basket -> session == NULL || basket -> error.code != NULL) {
        LOG("ERROR", "session creation failed");
        return;
    }

    // 2. register a multiplexed stream on this connection
    Stream *stream = registerStream(basket);
    if (stream == NULL) {
        return;
    }

    // 3. dispatch the request over HTTP/1.1 or HTTP/2
    if (basket -> session -> protocol == HTTP_PROTOCOL_1_1) {
        handleHttp11Request(basket, stream, !waitForResponse);
    } else {
        handleHttp2Request(basket);
    }
    if (basket -> error.code != NULL) {
        completeResponseSink(stream);
        unregisterStream(basket, stream);
        return;
    }

    *outStream = stream;

    // for reading responses, go to handleSession -> createSession -> readerLoop -> readerDispatch -> handleStreamFrame

    // 4. sync: wait for the stream to finish, then finalize into the basket
    if (waitForResponse) {
        awaitStream(basket, stream);
        if (basket -> error.code == NULL) {
            finalizeStreamIntoBasket(basket, stream);
        }
    }
}

static void handleHttp2Request(Basket *basket) {
    // Serialize all writes on this connection so concurrent streams' HEADERS /
    // DATA frames are never interleaved on the wire. The stream id was already
    // assigned in registerStream (basket -> streamId).
    pthread_mutex_lock(&basket -> session -> writeMutex);
    sendHeadersFrame(basket);
    if (basket -> error.code == NULL && basket -> request.payload != NULL) {
        sendDataFrame(basket);
    }
    pthread_mutex_unlock(&basket -> session -> writeMutex);
}

// A connection-level GOAWAY / SETTINGS_TIMEOUT is retryable once;
// HTTP_1_1_REQUIRED additionally forces the retry over HTTP/1.1.
static int isRetryableError(Error error) {
    if (error.code == NULL) { return 0; }
    return strcmp(error.code, ERR_SESSION_SETTINGS_TIMEOUT.code) == 0
        || strcmp(error.code, ERR_SESSION_GO_AWAY.code) == 0
        || strcmp(error.code, ERR_SESSION_HTTP_1_1_REQUIRED.code) == 0;
}

// Invalidate the failed connection so no new request reuses it (the reaper
// closes it once its in-flight streams drain) and clear the error so the next
// attempt starts fresh. HTTP_1_1_REQUIRED additionally downgrades the retry.
static void prepareRetry(Basket *basket) {
    if (requiresHttp11Downgrade(basket -> error)) {
        basket -> forceHttp11 = 1;
    }
    prepareStreamRetry(basket);
    basket -> session -> goingAway = 1;
    basket -> session = NULL;
    basket -> error = ERR_NONE;
    LOG("WARN", "session goes away, retry with a new session");
}

static int requiresHttp11Downgrade(Error error) {
    if (error.code == NULL) { return 0; }
    return strcmp(error.code, ERR_SESSION_HTTP_1_1_REQUIRED.code) == 0;
}

// Proxy errors (e.g. 407 authorization failed) are NOT retryable: the proxy
// tunnel is unusable and all sessions through it were already invalidated in
// handleSession(); retrying would just repeat the failure.
static int isProxyFailureError(Error error) {
    if (error.code == NULL) { return 0; }
    return strcmp(error.code, ERR_PROXY_AUTHORIZATION_FAILED.code) == 0
        || strcmp(error.code, ERR_PROXY_SOCKET_NONBLOCK_SETTING_FAILED.code) == 0
        || strcmp(error.code, ERR_PROXY_SOCKET_CONNECTING_FAILED.code) == 0
        || strcmp(error.code, ERR_PROXY_SEND_CONNECT_REQUEST_FAILED.code) == 0
        || strcmp(error.code, ERR_PROXY_UNEXPECTED_RESPONSE.code) == 0
        || strcmp(error.code, ERR_PROXY_SOCKET_CONNECTING_TIMEOUT.code) == 0
        || strcmp(error.code, ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR.code) == 0
        || strcmp(error.code, ERR_PROXY_SOCKET_CONNECTING_REFUSED.code) == 0;
}

// Serialize once (cached in the basket) and copy into the caller buffer.
// Returns 1 when the full result fitted, 2 when truncated (basket keeps the
// full result for a retry), -1 when serialization failed.
static int copyBasketResult(Basket *basket, char *dest, int capacity, int *outLen) {
    if (basket -> serializedResult == NULL) {
        basket -> serializedResult = basketToString(basket, outLen);
    }
    if (basket -> serializedResult == NULL) {
        return -1;
    }
    const int len = (int) strlen(basket -> serializedResult);
    if (outLen) { *outLen = len; }
    if (len >= capacity) {
        return 2;
    }
    memcpy(dest, basket -> serializedResult, len + 1);
    return 1;
}

// Blocking basket: the exchange already finished inside handleRequest(), so
// copy the result out and release the basket once it fully fitted.
static void handleResponseSync(Basket *basket, char *dest, int capacity, int *outStatus, int *outLen) {
    int code = copyBasketResult(basket, dest, capacity, outLen);
    if (code == 1) {
        freeBasket(basket);
    }
    if (outStatus) { *outStatus = code; }
}

// Non-blocking request: snapshot the registry entry, poll the stream until it
// ends (or the reading timeout fires), finalize the result, then atomically
// claim the slot and free the basket/stream.
static void handleResponseAsync(Basket *basket, char *dest, int capacity, int *outStatus, int *outLen) {
    Stream *stream = NULL;
    time_t startTime = 0;
    long requestId = 0;
    int detached = 0;

    pthread_mutex_lock(&asyncRegistryMutex);
    AsyncRequestEntry *entry = asyncFindSlotByBasket(basket);
    if (entry != NULL) {
        stream = entry -> stream;
        startTime = entry -> startTime;
        requestId = entry -> requestId;
    }
    pthread_mutex_unlock(&asyncRegistryMutex);

    if (entry == NULL) {
        // Unknown handle (already reaped).
        if (outStatus) { *outStatus = -1; }
        return;
    }

    int done = 0;
    int failed = 0;

    if (basket -> error.code != NULL && stream == NULL) {
        // Startup failed before sending: report as failed.
        done = 1;
        failed = 1;
    } else if (stream != NULL) {
        pthread_mutex_lock(&stream -> lock);
        const int ended = stream -> isEnded;
        // A streaming response is bounded by its idle time, not by the whole
        // transfer time.
        const time_t lastActivity = isResponseStreaming(stream) && stream -> lastActivityTime > startTime
                                    ? stream -> lastActivityTime : startTime;
        pthread_mutex_unlock(&stream -> lock);

        if (ended) {
            done = 1;
            failed = 0;
        } else {
            // Still in flight: check the reading timeout.
            const int timeoutMs = basket -> responseReadingTimeoutInMilliseconds;
            const time_t now = time(NULL);
            const long elapsedMs = (long) (now - lastActivity) * 1000;
            if (timeoutMs > 0 && elapsedMs > timeoutMs) {
                LOG("ERROR", "async request %ld timed out after %ldms", requestId, elapsedMs);
                basket -> error = ERR_RESPONSE_NO_CONTENT_AFTER_READING_TIMEOUT;
                done = 1;
                failed = 1;
            }
        }
    } else {
        // basket exists, no error, but no stream: nothing was sent. Treat as
        // still pending (should not normally happen).
        done = 0;
    }

    if (!done) {
        if (outStatus) { *outStatus = 0; }
        return;
    }

    // Finalize (move headers/payload into basket + decompress) before claiming
    // the slot, so the result is fully assembled when we detach.
    if (stream != NULL && basket -> error.code == NULL) {
        finalizeStreamIntoBasket(basket, stream);
    }

    // Hand the result to the caller buffer; on truncation keep the basket
    // registered so the retry finds the cached serialized result.
    int code = copyBasketResult(basket, dest, capacity, outLen);
    if (code < 0) {
        code = -1;
    }

    if (code == 1) {
        // Atomically claim + detach the slot (no-op if another poller already did).
        Basket *detachedBasket = NULL;
        Stream *detachedStream = NULL;
        pthread_mutex_lock(&asyncRegistryMutex);
        AsyncRequestEntry *current = asyncFindSlotByBasket(basket);
        if (current != NULL) {
            asyncDetachSlot(current, &detachedBasket, &detachedStream);
            detached = 1;
        }
        pthread_mutex_unlock(&asyncRegistryMutex);

        if (detached) {
            if (detachedStream != NULL) {
                unregisterStream(detachedBasket, detachedStream);
            }
            if (detachedBasket != NULL) {
                freeBasket(detachedBasket);
            }
        }
    }

    if (outStatus) { *outStatus = failed ? -1 : code; }
}

// Allocate a fresh request id + free registry slot under the registry lock.
static AsyncRequestEntry *asyncAllocEntry(void) {
    pthread_mutex_lock(&asyncRegistryMutex);
    const long requestId = asyncAllocSlot();
    AsyncRequestEntry *entry = requestId != 0 ? asyncFindSlot(requestId) : NULL;
    pthread_mutex_unlock(&asyncRegistryMutex);
    return entry;
}

// Allocate a fresh request id and a free registry slot. Returns 0 on failure
// (registry full). The slot is left locked by the caller (asyncRegistryMutex).
static long asyncAllocSlot(void) {
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++) {
        if (!asyncRegistry[i].inUse) {
            asyncRegistry[i].inUse = 1;
            asyncRegistry[i].requestId = nextRequestId++;
            asyncRegistry[i].basket = NULL;
            asyncRegistry[i].stream = NULL;
            asyncRegistry[i].startTime = time(NULL);
            return asyncRegistry[i].requestId;
        }
    }
    return 0;
}

static AsyncRequestEntry *asyncFindSlot(long requestId) {
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++) {
        if (asyncRegistry[i].inUse && asyncRegistry[i].requestId == requestId) {
            return &asyncRegistry[i];
        }
    }
    return NULL;
}

// Locate the registry entry that currently owns the given basket (the handle
// passed to handleResponse). Caller MUST hold asyncRegistryMutex.
static AsyncRequestEntry *asyncFindSlotByBasket(Basket *basket) {
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++) {
        if (asyncRegistry[i].inUse && asyncRegistry[i].basket == basket) {
            return &asyncRegistry[i];
        }
    }
    return NULL;
}

// Detach an entry's basket/stream and mark the slot reusable. Caller MUST hold
// asyncRegistryMutex. The detached basket/stream are returned via out params so
// the caller can free them outside the registry lock (unregisterStream and
// freeBasket take their own locks).
static void asyncDetachSlot(AsyncRequestEntry *entry, Basket **outBasket, Stream **outStream) {
    *outBasket = entry -> basket;
    *outStream = entry -> stream;
    entry -> basket = NULL;
    entry -> stream = NULL;
    entry -> inUse = 0;
}

void cleanupAsyncRequests(void) {
    // Detach all in-use entries under the lock, then free their baskets/streams
    // outside it (unregisterStream/freeBasket take their own locks).
    Basket *baskets[MAX_ASYNC_REQUESTS];
    Stream *streams[MAX_ASYNC_REQUESTS];
    int count = 0;

    pthread_mutex_lock(&asyncRegistryMutex);
    for (int i = 0; i < MAX_ASYNC_REQUESTS; i++) {
        if (asyncRegistry[i].inUse) {
            asyncDetachSlot(&asyncRegistry[i], &baskets[count], &streams[count]);
            count++;
        }
    }
    pthread_mutex_unlock(&asyncRegistryMutex);

    for (int i = 0; i < count; i++) {
        if (streams[i] != NULL) {
            unregisterStream(baskets[i], streams[i]);
        }
        if (baskets[i] != NULL) {
            freeBasket(baskets[i]);
        }
    }
}
