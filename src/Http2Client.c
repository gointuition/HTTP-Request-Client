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
            // 2. create session
            handleSession(basket);
            // createSession(basket);
            if (basket -> session == NULL) {
                LOG("ERROR", "session creation failed");
            } else {
                if (basket -> error.code == NULL) {
                    // 3. send request
                    sendRequest(basket);
                }
                if (basket -> error.code == NULL) {
                    // 4. receive response
                    receiveResponse(basket);

                    if (basket -> error.code != NULL
                        && (strcmp(basket -> error.code, ERR_SESSION_SETTINGS_TIMEOUT.code) == 0 || strcmp(basket -> error.code, ERR_SESSION_GO_AWAY.code) == 0)
                    ) {
                        // 5. close session and retry
                        cleanupTargetSession(basket);
                        basket -> session = NULL;
                        basket -> error = ERR_NONE;
                        LOG("WARN", "session goes away, retry with a new session");
                        continue;
                    }
                }
            }
            break;
        }
    }

    char *result = basketToString(basket, outLen);
    freeBasket(basket);
    return result;
}

static void sendRequest(Basket *basket) {
    pthread_mutex_lock(&basket -> session -> lock);
    basket -> streamId = atomic_fetch_add(&basket -> session -> streamId, 2);
    LOG("DEBUG", "current stream id: %d", basket -> streamId);
    pthread_mutex_unlock(&basket -> session -> lock);

    sendHeadersFrame(basket);
    if (basket -> error.code == NULL) {
        if (basket -> request.payload != NULL) {
            sendDataFrame(basket);
        }
    }
}

void getBasketContent(char *basketStr, char *dest) {
    if (basketStr != NULL && dest != NULL) {
        strcpy(dest, basketStr);
        free(basketStr);
    }
}