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
#include <pthread.h>

#include "UrlParser.h"
#include "Error.h"
#include "BrowserHandler.h"

#include "openssl/ssl.h"

// carries host:port (+ proxy identity) through the SSL object for the new-session callback.
// proxy identity is included so TLS 1.3 session resumption (pre_shared_key) is scoped per
// proxy: a session established over one proxy must NOT be reused when connecting to the same
// host through a different proxy.
typedef struct {
    const char *host;
    const char *port;
    const char *proxyScheme;
    const char *proxyHost;
    const char *proxyPort;
    const char *proxyAuthorization;
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

typedef enum {
    HTTP_PROTOCOL_2 = 0,
    HTTP_PROTOCOL_1_1 = 1
} HttpProtocol;

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
    char                response[1024];
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

// Maximum number of concurrent streams multiplexed over a single connection.
#define MAX_CONCURRENT_STREAMS_PER_SESSION 256

// Per-stream state for HTTP/2 multiplexing. The connection reader thread
// demultiplexes inbound frames by stream id into these structures; the
// requesting thread waits on `cond` until `isEnded` is set, then copies the
// accumulated headers/payload into its Basket.
typedef struct Stream {
    uint32_t            streamId;
    int                 isEnded;
    ResponseHeader      *headers;
    size_t              numHeaders;
    unsigned char       *combinedPayload;
    size_t              combinedPayloadSize;
    Error               error;
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
} Stream;

typedef struct {
    char                scheme[16];
    char                host[256];
    char                port[8];
    char                clientHelloId[64];
    int                 sockfd;
    int                 isActive;
    int                 nonBlocking;    // 1 = socket runs in non-blocking I/O mode
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
    HttpProtocol        protocol;
    int                 plainProxy;
    TLSConnInfo         *connInfo;
    // ── HTTP/2 multiplexing / concurrency ──
    pthread_mutex_t     writeMutex;     // serialize SSL_write across concurrent streams
    pthread_mutex_t     streamsMutex;   // guard the stream registry below
    Stream              *streams[MAX_CONCURRENT_STREAMS_PER_SESSION];
    int                 inflightCount;  // streams currently awaiting completion
    pthread_t           reader;         // per-connection frame reader thread
    volatile int        readerRunning;  // reader loop control flag
    int                 readerStarted;  // whether pthread_create succeeded (join guard)
    volatile int        goingAway;      // set on GOAWAY / connection loss; blocks reuse
    Error               connError;      // connection-level error (GOAWAY reason), applied to streams
} Session;

typedef struct {
    const char  *url;
    const char  *method;
    // const char  *sessionId;
    int         decompress;
    int         nonBlocking;    // 1 = use non-blocking socket I/O for this request
    int         forceHttp11;    // 1 = request forced HTTP/1.1 ("session": {"protocol": "http/1.1"})
    int         connectTimeoutInMilliseconds;
    int         responseReadingTimeoutInMilliseconds;
    int         sessionExpirationInMilliseconds;
    atomic_uint streamId;   // stream id for this request
    BrowserType browserType;
    char        clientHelloId[64];
    Proxy       proxy;
    Request     request;
    Response    response;
    Error       error;
    Session     *session;
} Basket;

Basket* buildBasket(const char *requestString);

void freeBasket(Basket *basket);

char* basketToString(Basket *basket, int *outLen);

const char * getUserAgent(Basket *basket);

#ifdef __cplusplus
}
#endif

#endif //REQUEST_H