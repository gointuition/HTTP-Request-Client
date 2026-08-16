//
//  Http2Client.c
//  HTTP2
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

#include "Http2Client.h"

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
#include "RequestHandler.h"
#include "ResponseHandler.h"
#include "Log.h"

#define SESSION_MAGIC 0X55AA1234
#define SESSION_ACTIVE 1
#define SESSION_INACTIVE 0

// TODO log

Http2Client http2Client = {
    .initialSessionRecvWindow = 15663105 + 65535
};

//HashTable_InetAddress *tableInetAddress = NULL;

static void sendRequest(Basket *basket);

Http2Client* newHttp2Client(void) {
//    printf("new Http2Client\n");
//    newClientConnector();
    return &http2Client;
}

void initialiseEnv(void) {
#ifdef _WIN32
    // Winsock must be initialised before any socket call
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    initSharedSessionPool();
    buildHuffmanTree();

    // (&http2Client) -> state = STARTING;
    // if (tableInetAddress == NULL) {
    //     tableInetAddress = createHashTable_InetAddress(8);
    // }
    // printf("http2Client is starting\n");
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

// Prepare + send a request: build the basket, obtain/reuse a session, register
// a multiplexed stream, and send HEADERS(+DATA). On success, *outStream is set
// to the registered stream and 0 is returned; the caller owns both the basket
// and the stream. On failure, returns -1 and basket->error is set (the basket is
// NOT freed here so the caller can serialize the error).
int http2StartRequest(Basket *basket, Stream **outStream) {
    *outStream = NULL;

    // 1. obtain a session (reuse or create; a new connection starts its own
    //    reader thread that demultiplexes frames by stream id)
    handleSession(basket);
    if (basket -> session == NULL) {
        LOG("ERROR", "session creation failed");
        return -1;
    }
    if (basket -> error.code != NULL) {
        return -1; // session creation reported an error
    }

    // 2. register a multiplexed stream on this connection
    Stream *stream = registerStream(basket);
    if (stream == NULL) {
        return -1;
    }

    // 3. send HEADERS(+DATA), serialized with other streams' writes
    sendRequest(basket);
    if (basket -> error.code != NULL) {
        unregisterStream(basket, stream);
        return -1;
    }

    *outStream = stream;
    return 0;
}

char* handleRequest(const char *requestJSONString, int *outLen) {
    // 1. prepare request
    Basket *basket = buildBasket(requestJSONString);
    if (basket != NULL && basket -> error.code == NULL) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            Stream *stream = NULL;
            if (http2StartRequest(basket, &stream) == 0 && stream != NULL) {
                // 2. wait for the reader thread to finish this stream, then move
                //    the collected headers/payload into the basket (and decompress)
                awaitStream(basket, stream);
                if (basket -> error.code == NULL) {
                    finalizeStreamIntoBasket(basket, stream);
                }
            }

            // a connection-level GOAWAY / SETTINGS_TIMEOUT is retryable once.
            // Proxy errors (e.g. 407 authorization failed) are NOT retryable: the
            // proxy tunnel is unusable and all sessions through it were already
            // invalidated in handleSession(); retrying would just repeat the failure.
            const int retryable = basket -> error.code != NULL
                && (strcmp(basket -> error.code, ERR_SESSION_SETTINGS_TIMEOUT.code) == 0
                    || strcmp(basket -> error.code, ERR_SESSION_GO_AWAY.code) == 0);
            const int proxyError = basket -> error.code != NULL
                && (strcmp(basket -> error.code, ERR_PROXY_AUTHORIZATION_FAILED.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SOCKET_NONBLOCK_SETTING_FAILED.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_FAILED.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SEND_CONNECT_REQUEST_FAILED.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_UNEXPECTED_RESPONSE.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_TIMEOUT.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR.code) == 0
                    || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_REFUSED.code) == 0);

            if (stream != NULL) {
                unregisterStream(basket, stream);
            }

            if (retryable && attempt == 0) {
                // Mark the connection unusable so no new request reuses it; the
                // reaper closes it once its in-flight streams drain. Retry on a
                // fresh connection.
                basket -> session -> goingAway = 1;
                basket -> session = NULL;
                basket -> error = ERR_NONE;
                LOG("WARN", "session goes away, retry with a new session");
                continue;
            }
            if (proxyError) {
                // Proxy is unusable (e.g. 407). Don't retry on a fresh connection;
                // all sessions through this proxy were already invalidated.
                LOG("WARN", "proxy error, not retrying: %s", basket -> error.code);
                break;
            }
            break;
        }
    }

    char *result = basketToString(basket, outLen);
    freeBasket(basket);
    return result;
}

static void sendRequest(Basket *basket) {
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

void getBasketContent(char *basketStr, char *dest) {
    if (basketStr != NULL && dest != NULL) {
        strcpy(dest, basketStr);
        free(basketStr);
    }
}