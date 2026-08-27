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
#include <pthread.h>
#include <zlib.h>

#include "Compat.h"

#include "SocketHandler.h"
#include "Error.h"
#include "CompressHandler.h"
#include "Session.h"
#include "AsyncRequest.h"
// #include "FrameHandler.h"
#include "Http2RequestHandler.h"
#include "Http2ResponseHandler.h"
#include "Http11RequestHandler.h"
#include "Log.h"

#define SESSION_MAGIC 0X55AA1234
#define SESSION_ACTIVE 1
#define SESSION_INACTIVE 0

// TODO log

HttpClient httpClient = {
    .initialSessionRecvWindow = 15663105 + 65535
};

//HashTable_InetAddress *tableInetAddress = NULL;

static void handleHttp2Request(Basket *basket);
static int isRetryableError(Error error);
static void prepareRetry(Basket *basket);
static int requiresHttp11Downgrade(Error error);
static int isProxyFailureError(Error error);

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

char* handleRequest(const char *requestJSONString, int *outLen) {
    // 1. prepare request
    Basket *basket = buildBasket(requestJSONString);
    if (basket != NULL && basket -> error.code == NULL) {
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

    char *result = basketToString(basket, outLen);
    freeBasket(basket);
    return result;
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

void getBasketContent(char *basketStr, char *dest) {
    if (basketStr != NULL && dest != NULL) {
        strcpy(dest, basketStr);
        free(basketStr);
    }
}