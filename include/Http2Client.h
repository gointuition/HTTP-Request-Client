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

#ifdef __cplusplus
}
#endif

#endif /* Http2Client_h */
