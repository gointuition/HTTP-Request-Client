//
// Created by Intuition on 26-8-15.
//

#include "AsyncRequest.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Basket.h"
#include "HttpClient.h"
#include "Session.h"
#include "Http2ResponseHandler.h"
#include "Error.h"
#include "Log.h"

#define MAX_ASYNC_REQUESTS 1024

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

long handleRequestAsync(const char *requestJSONString) {
    pthread_mutex_lock(&asyncRegistryMutex);
    long requestId = asyncAllocSlot();
    AsyncRequestEntry *entry = requestId != 0 ? asyncFindSlot(requestId) : NULL;
    pthread_mutex_unlock(&asyncRegistryMutex);

    if (entry == NULL) {
        LOG("ERROR", "async request registry full (max %d)", MAX_ASYNC_REQUESTS);
        return 0;
    }

    // Build the basket outside the registry lock (jansson parsing can be slow).
    Basket *basket = buildBasket(requestJSONString);
    if (basket == NULL) {
        pthread_mutex_lock(&asyncRegistryMutex);
        entry -> inUse = 0;
        pthread_mutex_unlock(&asyncRegistryMutex);
        return 0;
    }

    Stream *stream = NULL;
    if (basket -> error.code == NULL) {
        executeRequest(basket, &stream, 0);
        if (basket -> error.code != NULL) {
            // executeRequest left basket->error set; the request stays registered
            // so pollRequest() surfaces it as a completed-with-error entry.
            stream = NULL;
        }
    }

    // Publish the basket/stream to the slot under the lock so a concurrent
    // pollRequest() on another thread never observes a half-written entry. A
    // poll that arrives before this point sees basket == NULL and reports
    // "still in flight", which is correct.
    pthread_mutex_lock(&asyncRegistryMutex);
    entry -> basket = basket;
    entry -> stream = stream;
    entry -> startTime = time(NULL);
    pthread_mutex_unlock(&asyncRegistryMutex);

    return requestId;
}

char* pollRequest(long requestId, int *outStatus, int *outLen) {
    if (outStatus) { *outStatus = 0; }
    if (outLen) { *outLen = 0; }

    // Snapshot the entry under the lock. If it is complete-and-reapable, detach
    // it atomically here so two concurrent pollers of the same id cannot both
    // free the basket/stream.
    Basket *basket = NULL;
    Stream *stream = NULL;
    time_t startTime = 0;
    int detached = 0;

    pthread_mutex_lock(&asyncRegistryMutex);
    AsyncRequestEntry *entry = asyncFindSlot(requestId);
    if (entry != NULL) {
        basket = entry -> basket;
        stream = entry -> stream;
        startTime = entry -> startTime;
    }
    pthread_mutex_unlock(&asyncRegistryMutex);

    if (entry == NULL) {
        // Unknown id (already reaped or never started).
        if (outStatus) { *outStatus = -1; }
        return NULL;
    }

    int done = 0;
    int failed = 0;

    if (basket == NULL) {
        // Not fully started yet (or startup failed before a basket existed).
        done = 0;
    } else if (basket -> error.code != NULL && stream == NULL) {
        // Startup failed before sending: report as failed.
        done = 1;
        failed = 1;
    } else if (stream != NULL) {
        pthread_mutex_lock(&stream -> lock);
        int ended = stream -> isEnded;
        pthread_mutex_unlock(&stream -> lock);

        if (ended) {
            done = 1;
            failed = 0;
        } else {
            // Still in flight: check the reading timeout.
            const int timeoutMs = basket -> responseReadingTimeoutInMilliseconds;
            const time_t now = time(NULL);
            const long elapsedMs = (long) (now - startTime) * 1000;
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
        return NULL;
    }

    // Finalize (move headers/payload into basket + decompress) before claiming
    // the slot, so the result is fully assembled when we detach.
    if (stream != NULL && basket -> error.code == NULL) {
        finalizeStreamIntoBasket(basket, stream);
    }

    char *result = basketToString(basket, outLen);

    // Atomically claim + detach the slot (no-op if another poller already did).
    Basket *detachedBasket = NULL;
    Stream *detachedStream = NULL;
    pthread_mutex_lock(&asyncRegistryMutex);
    AsyncRequestEntry *current = asyncFindSlot(requestId);
    if (current != NULL && current -> basket == basket) {
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

    if (outStatus) { *outStatus = failed ? -1 : 1; }
    return result;
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
