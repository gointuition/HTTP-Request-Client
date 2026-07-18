//
// Created by Intuition on 25-11-1.
//

#ifndef REQUEST_H
#define REQUEST_H

#ifdef __cplusplus
    #include <atomic>
    typedef std::atomic<unsigned int> atomic_uint;
#else
    #include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include "UrlParser.h"
#include "Error.h"
#include "BrowserHandler.h"

#include "openssl/ssl.h"

// carries host:port through the SSL object for the new-session callback
typedef struct {
    const char *host;
    const char *port;
} TLSConnInfo;

#define RESPONSE_HEADERS_MAX_SIZE 64

#define HTTP_METHOD_GET "GET"
#define HTTP_METHOD_POST "POST"
#define HTTP_METHOD_PUT "PUT"
#define HTTP_METHOD_PATCH "PATCH"
#define HTTP_METHOD_DELETE "DELETE"

typedef enum {
    SESSION_AVAILABLE,
    SESSION_UNAVAILABLE
} SessionStatus;

typedef struct {
    const char  *name;
    const char  *value;
    int         isPseudo;
    int         freeName;
    int         freeValue;
} RequestHeader;

typedef struct {
    char            *payload;
    int             containsContentLength;
    size_t          numHeaders;
    URLComponents   urlComponents;
    RequestHeader   *headers;
} Request;

typedef struct {
    char    *name;
    char    *value;
    int     freeName;
    int     freeValue;
} ResponseHeader;

typedef struct {
    size_t          numHeaders;
    size_t          payloadSize;
    unsigned char   *payload;
    ResponseHeader  *headers;
} Response;

typedef struct {
    // TODO fixed size?
    char                scheme[16];
    char                host[256];
    char                port[8];
    char                authorization[1024];
} Proxy;

// Hpack dynamic table
typedef struct {
    char *name;
    char *value;
} HpackTableEntry;

// Hpack Context
typedef struct {
    HpackTableEntry *dynamicTable;
    size_t dynamicTableSize;
    size_t dynamicTableCapacity;
    size_t dynamicTableMaxSize;
} HpackContext;

typedef struct {
    char                scheme[16];
    char                host[256];
    char                port[8];
    int                 sockfd;
    int                 isActive;
    int                 expirationInMilliseconds;
    uint32_t            magic;
    atomic_uint         streamId; // stream id to be used
    time_t              creationTime;
    time_t              lastUsedTime;
    pthread_mutex_t     lock;
    // TODO proxy information
    Proxy               proxy;
    SSL_CTX             *sslCtx;
    SSL                 *ssl;
    HpackContext        *hpackCtx;
    TLSConnInfo         *connInfo; // heap-allocated, lives for session lifetime (used by newSessionCallback)
} Session;

typedef struct {
    const char  *url;
    const char  *method;
    // const char  *sessionId;
    int         decompress;
    int         connectTimeoutInMilliseconds;
    int         responseReadingTimeoutInMilliseconds;
    int         sessionExpirationInMilliseconds;
    atomic_uint streamId;   // stream id for this request
    BrowserType browserType;
    Proxy       proxy;
    Request     request;
    Response    response;
    Error       error;
    Session     *session;
} Basket;

Basket* buildBasket(const char *requestString);

void freeBasket(Basket *basket);

int basketToString(Basket *basket, char *basketJSONString, size_t basketStrLen);

const char * getUserAgent(Basket *basket);

#ifdef __cplusplus
}
#endif

#endif //REQUEST_H