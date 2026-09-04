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
#include <time.h>
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

// Library-owned state of a streaming response sink (see ResponseStream.h).
typedef struct ResponseSink ResponseSink;

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

// RFC 7540 default flow-control window for both stream and connection scope.
#define HTTP2_INITIAL_FLOW_CONTROL_WINDOW 65535

// Per-stream state for HTTP/2 multiplexing. The connection reader thread
// demultiplexes inbound frames by stream id into these structures; the
// requesting thread waits on `cond` until `isEnded` is set, then copies the
// accumulated headers/payload into its Basket.
typedef struct Stream {
    uint32_t            streamId;
    int                 isEnded;
    ResponseSink        *sink;        // response funnel: caller contract or library collector
    ResponseHeader      *headers;
    size_t              numHeaders;
    unsigned char       *combinedPayload;
    size_t              combinedPayloadSize;
    int                 payloadDecoded; // the funnel already decoded the body (no one-shot pass)
    time_t              lastActivityTime; // last body chunk received (streaming idle timeout)
    Error               error;
    pthread_mutex_t     lock;
    pthread_cond_t      cond;
} Stream;

// Stream-scope flow-control credit consumed but not yet returned to the peer.
// Reader thread only: batched into fewer WINDOW_UPDATE frames, like a browser.
typedef struct {
    uint32_t streamId;
    uint32_t credit;
} PendingWindowUpdate;

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
    uint32_t            pendingConnWindow; // connection-scope flow credit owed (reader thread only)
    PendingWindowUpdate pendingStreamWindows[MAX_CONCURRENT_STREAMS_PER_SESSION]; // reader thread only
    int                 pendingStreamWindowCount;
    int                 connWindowFlushDue; // reader-only: the batch end must flush connection credit
    uint32_t            connWindowTarget;   // reader-only: auto-tuned total connection window
    uint32_t            connWindowSinceGrowth; // reader-only: bytes consumed since the last window growth
    unsigned char       *pendingWire;       // reader-only: control frames queued for the batch-end write
    size_t              pendingWireSize;
    size_t              pendingWireCapacity;
} Session;

typedef struct {
    const char  *url;
    const char  *method;
    // const char  *sessionId;
    int         decompress;
    int         nonBlocking;    // 1 = non-blocking (async) request, 0 = blocking
    long        requestId;      // internal async registry id (opaque to callers)
    char        *serializedResult;  // cached response JSON kept across handleResponse retries
    int         forceHttp11;    // 1 = request forced HTTP/1.1 ("session": {"protocol": "http/1.1"})
    int         connectTimeoutInMilliseconds;
    int         responseReadingTimeoutInMilliseconds;
    int         sessionExpirationInMilliseconds;
    atomic_uint streamId;   // stream id for this request
    BrowserType browserType;
    const BrowserFingerprint *fingerprint;  // resolved wire profile (version-pinned when clientHelloId is given)
    char        clientHelloId[64];
    Proxy       proxy;
    Request     request;
    Response    response;
    ResponseSink *sink;       // response funnel owned by this request (always set)
    Error       error;
    Session     *session;
} Basket;

Basket* buildBasket(const char *requestString);

// Override the "non-blocking" field of a request JSON config. Returns a new
// malloc'd JSON string (free it with freeJson()) or NULL when the input does
// not parse.
char* setNonBlocking(const char *requestJSONString, int nonBlocking);

// Free a JSON string returned by setNonBlocking. Exported because binding
// layers (e.g. cffi ABI mode) cannot resolve libc free() from the DLL.
void freeJson(char *json);

void freeBasket(Basket *basket);

char* basketToString(Basket *basket, int *outLen);

const char * getUserAgent(Basket *basket);

#ifdef __cplusplus
}
#endif

#endif //REQUEST_H