//
// Created by Intuition on 26-2-7.
//

#include "Session.h"

#include <string.h>
//#include <pthread/pthread.h>
#include <pthread.h>

#include "Compat.h"

#include "RequestHandler.h"
#include "SocketHandler.h"
#include "SSLHandler.h"
#include "Log.h"

// common file shared between task thread and daemon thread
#define SHM_NAME "/http2_session_pool"

// ─── TLS Session Cache (for TLS 1.3 session resumption / pre_shared_key) ───
#define MAX_TLS_SESSION_CACHE 256

typedef struct {
    char host[256];
    char port[8];
    SSL_SESSION *session;
    time_t createdAt;
} TLSSessionCacheEntry;

static TLSSessionCacheEntry tlsSessionCache[MAX_TLS_SESSION_CACHE];
static pthread_mutex_t tlsSessionCacheMutex;

static void initTLSSessionCache(void);
static SSL_SESSION* lookupTLSSession(const char *host, const char *port);
// ─────────────────────────────────────────────────────────────────────────────

static SharedSessionPool pool;

static void createSession(Basket *basket);
static Session* initSession(Basket *basket, int sockfd, SSL_CTX * sslCtx, SSL * ssl, TLSConnInfo *connInfo);
static void getSessionInfo(Basket * basket);
static void registerSession(Basket * basket);
static HpackContext *initHpackContext(Basket *basket);
static void freeHpackContext(HpackContext *ctx);

void handleSession(Basket *basket) {
    cleanupSessions(0);
    getSessionInfo(basket);
    if (basket -> session == NULL) {
        createSession(basket);
        if (basket -> session != NULL) {
            registerSession(basket);
        }
    }
}

void cleanupSessions(int isAll) {
    if (isAll != 1) {
        if (pool.sessionCount < 200) {
            return;
        }
    }

    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (pool.sessionMap[i] == 1) {
            Session *session = pool.sessions[i];

            if (isAll == 1 // cleanup all sessions before stopping the process
                || isConnecting(session) != 1 // cleanup closed sessions
                || (time(NULL) - session -> lastUsedTime) > (session -> expirationInMilliseconds) // cleanup expired sessions
            ) {
                LOG("DEBUG", "cleanup session %s//:%s:%s#%s//:%s:%s@%s", pool.sessions[i] -> scheme, pool.sessions[i] -> host, pool.sessions[i] -> port, pool.sessions[i] -> proxy.scheme, pool.sessions[i] -> proxy.host, pool.sessions[i] -> proxy.port, pool.sessions[i] -> proxy.authorization);
                closeSession(session, ERR_NONE);
                pool.sessions[i] = NULL;
                pool.sessionMap[i] = 0;
                pool.sessionCount--;
            }
        }
    }
    LOG("DEBUG", "session count: %lu", pool.sessionCount);
}

void cleanupTargetSession(Basket *basket) {
    // remove session from session pool
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (basket -> session == pool.sessions[i]) {
            pool.sessions[i] = NULL;
            pool.sessionMap[i] = 0;
            pool.sessionCount--;
            break;
        }
    }

    // free session
    closeSession(basket -> session, basket -> error);
}

int isConnecting(Session *session) {
    int error = 0;
    socklen_t len = sizeof(error);

    int ret = getsockopt(session -> sockfd, SOL_SOCKET, SO_ERROR, (char *) &error, &len);

    if (ret != 0) {
        // invalid socket
        return 0;
    }

    if (error != 0) {
        errno = error;
        LOG("ERROR", "something wrong with socket, connection is closed： %s (errno: %d)", strerror(errno), errno);
        return 0;
    }

    return 1;
}

int closeSession(Session *session, Error error) {
    if (session == NULL) {
        return -1;
    }

    if (session -> magic != SESSION_MAGIC) {
        return -2;
    }

    pthread_mutex_lock(&(session -> lock));

    // clear SSL app_data before freeing SSL (so callback won't access freed connInfo)
    if (session -> ssl) {
        SSL_set_app_data(session -> ssl, NULL);
    }
    if (session -> connInfo) {
        free(session -> connInfo);
        session -> connInfo = NULL;
    }

    // free(session -> id);
    freeSession(session -> ssl, session -> sslCtx, session -> sockfd, session -> hpackCtx, error);
    session -> isActive = SESSION_INACTIVE;

    pthread_mutex_unlock(&session -> lock);
    free(session);

    return 0;
}

void freeSession(SSL* ssl, SSL_CTX* ctx, int sockfd, HpackContext *hpackCtx, Error error) {
    if (ssl) {
        // if HTTP/2 SETTINGS_TIMEOUT, calling SSL_shutdown() will cause crash
        if (error.code == NULL || strcmp(error.code, ERR_SESSION_SETTINGS_TIMEOUT.code) != 0) {
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
    }
    if (ctx) {
        SSL_CTX_free(ctx);
    }
    if (sockfd >= 0) {
        closeSocket(sockfd);
    }
    if (hpackCtx) {
        // TODO not encountered yet
        freeHpackContext(hpackCtx);
    }
    // after OPENSSL 1.1.0+, call it to shut down engine
    EVP_cleanup();
}

static void getSessionInfo(Basket * basket) {
    pthread_mutex_lock(&pool.mutex);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (pool.sessionMap[i] == 1
            && pool.sessions[i] -> magic == SESSION_MAGIC
            && strcmp(pool.sessions[i] -> scheme, basket -> request.urlComponents.scheme) == 0
            && strcmp(pool.sessions[i] -> host, basket -> request.urlComponents.host) == 0
            && strcmp(pool.sessions[i] -> port, basket -> request.urlComponents.port) == 0
            && strcmp(pool.sessions[i] -> proxy.scheme, basket -> proxy.scheme) == 0
            && strcmp(pool.sessions[i] -> proxy.host, basket -> proxy.host) == 0
            && strcmp(pool.sessions[i] -> proxy.port, basket -> proxy.port) == 0
            && strcmp(pool.sessions[i] -> proxy.authorization, basket -> proxy.authorization) == 0
                ) {
            const int connecting = isConnecting(pool.sessions[i]);
            if (connecting == 1) {
                // TODO lock session required or not?
                pool.sessions[i] -> lastUsedTime = time(NULL);
                basket -> session = pool.sessions[i];
                LOG("DEBUG", "reuse the session %s//:%s:%s#%s//:%s:%s@%s", pool.sessions[i] -> scheme, pool.sessions[i] -> host, pool.sessions[i] -> port, pool.sessions[i] -> proxy.scheme, pool.sessions[i] -> proxy.host, pool.sessions[i] -> proxy.port, pool.sessions[i] -> proxy.authorization);
                break;
            }
        }
    }

    pthread_mutex_unlock(&pool.mutex);
}

static void createSession(Basket *basket) {
    int sockfd = -1;
    // create TCP connection
    if (strlen(basket -> proxy.host) > 0) {
        sockfd = createSocketThroughProxy(basket);
    } else {
        sockfd = createSocket(basket, basket -> request.urlComponents.host, basket -> request.urlComponents.port, 0);
    }
    if (sockfd < 0) {
        return;
    }

    // create SSL context
    SSL_CTX *sslCtx = createSSLContext(basket);
    if (!sslCtx) {
        freeSession(NULL, sslCtx, sockfd, NULL, basket -> error);
        return;
    }

    // create SSL
    SSL *ssl = createSSL(basket, sslCtx, sockfd);
    if (!ssl) {
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);
        return;
    }

    // configure SSL settings to simulate browsers behaviour
    int configured = configureSSLSettings(basket, ssl);
    if (configured == -1) {
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);
        return;
    }

    // SSL handshake
    // if a cached TLS session exists for this host:port, set it for session resumption (pre_shared_key)
    SSL_SESSION *cachedSession = lookupTLSSession(basket -> request.urlComponents.host, basket -> request.urlComponents.port);
    if (cachedSession) {
        SSL_set_session(ssl, cachedSession);
        LOG("DEBUG", "resuming TLS session for %s:%s", basket -> request.urlComponents.host, basket -> request.urlComponents.port);
    }

    // set conn info on SSL so the new-session callback can cache the session
    // the connInfo lives for the session's lifetime (TLS 1.3 NewSessionTicket is async)
    TLSConnInfo *connInfo = malloc(sizeof(TLSConnInfo));
    connInfo -> host = basket -> request.urlComponents.host;
    connInfo -> port = basket -> request.urlComponents.port;
    SSL_set_app_data(ssl, connInfo);

    int connect = SSL_connect(ssl);

    if (connect != 1) {
        SSL_set_app_data(ssl, NULL);
        free(connInfo);
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);

        LOG("ERROR", "SSL_connect failed: %s", ERR_error_string(ERR_get_error(), NULL));
        basket -> error = ERR_SESSION_SSL_CONNECT_FAILED;
        return;
    }

    // verify HTTP/2 negotiation
    const unsigned char *alpnProto;
    unsigned int alpnLen;
    SSL_get0_alpn_selected(ssl, &alpnProto, &alpnLen);

    if (alpnLen != 2 || memcmp(alpnProto, "h2", 2) != 0) {
        SSL_set_app_data(ssl, NULL);
        free(connInfo);
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);

        LOG("ERROR", "ALPN negotiation failed");
        basket -> error = ERR_SESSION_SSL_CONNECT_FAILED;
        return;
    }

    // establish HTTP/2 transport, send Settings frame
    const int transport = establishTransport(basket, ssl);
    if (transport < 0) {
        SSL_set_app_data(ssl, NULL);
        free(connInfo);
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);
        return;
    }

    Session *session = initSession(basket, sockfd, sslCtx, ssl, connInfo);
    if (session == NULL || basket -> error.code != NULL) {
        SSL_set_app_data(ssl, NULL);
        free(connInfo);
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);
        return;
    }

    // basket -> sessionId = strdup(session -> id);
    basket -> session = session;
}

static Session* initSession(Basket *basket, int sockfd, SSL_CTX * sslCtx, SSL * ssl, TLSConnInfo *connInfo) {
    Session *session = malloc(sizeof(Session));
    if (session == NULL) {
        LOG("ERROR", "failed to allocate memory for a new session");
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }
    strncpy(session -> scheme, basket -> request.urlComponents.scheme, sizeof(session -> scheme) - 1);
    session -> scheme[sizeof(session -> scheme) - 1] = '\0';
    strncpy(session -> host, basket -> request.urlComponents.host, sizeof(session -> host) - 1);
    session -> host[sizeof(session -> host) - 1] = '\0';
    strncpy(session -> port, basket -> request.urlComponents.port, sizeof(session -> port) - 1);
    session -> port[sizeof(session -> port) - 1] = '\0';

    session -> sockfd = sockfd;
    session -> sslCtx = sslCtx;
    session -> ssl = ssl;
    session -> isActive = SESSION_ACTIVE;
    session -> magic = SESSION_MAGIC;

    session -> creationTime = time(NULL);

    session -> expirationInMilliseconds = basket -> sessionExpirationInMilliseconds;

    session -> proxy = (Proxy) { 0, 0, 0, 0 };
    strncpy(session -> proxy.scheme, basket -> proxy.scheme, sizeof(session -> proxy.scheme) - 1);
    session -> proxy.scheme[sizeof(session -> proxy.scheme) - 1] = '\0';
    strncpy(session -> proxy.host, basket -> proxy.host, sizeof(session -> proxy.host) - 1);
    session -> proxy.host[sizeof(session -> proxy.host) - 1] = '\0';
    strncpy(session -> proxy.port, basket -> proxy.port, sizeof(session -> proxy.port) - 1);
    session -> proxy.port[sizeof(session -> proxy.port) - 1] = '\0';
    strncpy(session -> proxy.authorization, basket -> proxy.authorization, sizeof(session -> proxy.authorization) - 1);
    session -> proxy.authorization[sizeof(session -> proxy.authorization) - 1] = '\0';

    session -> hpackCtx = initHpackContext(basket);
    session -> connInfo = connInfo; // ownership transferred from createSession

    atomic_init(&session -> streamId, 1);

    pthread_mutex_init(&(session -> lock), NULL);

    return session;
}

static HpackContext *initHpackContext(Basket *basket) {
    HpackContext *ctx = malloc(sizeof(HpackContext));
    if (!ctx) {
        LOG("ERROR", "HpackContext memory allocation failed");
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }

    ctx -> dynamicTable = NULL;
    ctx -> dynamicTableSize = 0;
    ctx -> dynamicTableCapacity = 0;
    ctx -> dynamicTableMaxSize = 4096;

    return ctx;
}

static void freeHpackContext(HpackContext *ctx) {
    if (!ctx) { return; }

    if (ctx -> dynamicTable) {
        for (size_t i = 0; i < ctx -> dynamicTableSize; i++) {
            if (ctx -> dynamicTable[i].name) free(ctx -> dynamicTable[i].name);
            if (ctx -> dynamicTable[i].value) free(ctx -> dynamicTable[i].value);
        }
        free(ctx -> dynamicTable);
    }
    free(ctx);
}

static void registerSession(Basket * basket);

// ─── TLS Session Cache Implementation ───

static void initTLSSessionCache(void) {
    pthread_mutex_init(&tlsSessionCacheMutex, NULL);
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        tlsSessionCache[i].session = NULL;
    }
}

void cleanupTLSSessionCache(void) {
    pthread_mutex_lock(&tlsSessionCacheMutex);
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session) {
            SSL_SESSION_free(tlsSessionCache[i].session);
            tlsSessionCache[i].session = NULL;
        }
    }
    pthread_mutex_unlock(&tlsSessionCacheMutex);
    pthread_mutex_destroy(&tlsSessionCacheMutex);
}

static SSL_SESSION* lookupTLSSession(const char *host, const char *port) {
    pthread_mutex_lock(&tlsSessionCacheMutex);
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session != NULL
            && strcmp(tlsSessionCache[i].host, host) == 0
            && strcmp(tlsSessionCache[i].port, port) == 0) {
            SSL_SESSION *session = tlsSessionCache[i].session;
            SSL_SESSION_up_ref(session);
            pthread_mutex_unlock(&tlsSessionCacheMutex);
            return session;
        }
    }
    pthread_mutex_unlock(&tlsSessionCacheMutex);
    return NULL;
}

void cacheTLSSession(const char *host, const char *port, SSL_SESSION *session) {
    pthread_mutex_lock(&tlsSessionCacheMutex);

    // update existing entry
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session != NULL
            && strcmp(tlsSessionCache[i].host, host) == 0
            && strcmp(tlsSessionCache[i].port, port) == 0) {
            SSL_SESSION_free(tlsSessionCache[i].session);
            tlsSessionCache[i].session = session;
            tlsSessionCache[i].createdAt = time(NULL);
            SSL_SESSION_up_ref(session);
            LOG("DEBUG", "updated TLS session cache for %s:%s", host, port);
            pthread_mutex_unlock(&tlsSessionCacheMutex);
            return;
        }
    }

    // find empty slot
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session == NULL) {
            strncpy(tlsSessionCache[i].host, host, sizeof(tlsSessionCache[i].host) - 1);
            tlsSessionCache[i].host[sizeof(tlsSessionCache[i].host) - 1] = '\0';
            strncpy(tlsSessionCache[i].port, port, sizeof(tlsSessionCache[i].port) - 1);
            tlsSessionCache[i].port[sizeof(tlsSessionCache[i].port) - 1] = '\0';
            tlsSessionCache[i].session = session;
            tlsSessionCache[i].createdAt = time(NULL);
            SSL_SESSION_up_ref(session);
            LOG("DEBUG", "cached TLS session for %s:%s", host, port);
            pthread_mutex_unlock(&tlsSessionCacheMutex);
            return;
        }
    }

    // cache full — replace oldest entry
    int oldestIdx = 0;
    for (int i = 1; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].createdAt < tlsSessionCache[oldestIdx].createdAt) {
            oldestIdx = i;
        }
    }
    SSL_SESSION_free(tlsSessionCache[oldestIdx].session);
    strncpy(tlsSessionCache[oldestIdx].host, host, sizeof(tlsSessionCache[oldestIdx].host) - 1);
    tlsSessionCache[oldestIdx].host[sizeof(tlsSessionCache[oldestIdx].host) - 1] = '\0';
    strncpy(tlsSessionCache[oldestIdx].port, port, sizeof(tlsSessionCache[oldestIdx].port) - 1);
    tlsSessionCache[oldestIdx].port[sizeof(tlsSessionCache[oldestIdx].port) - 1] = '\0';
    tlsSessionCache[oldestIdx].session = session;
    tlsSessionCache[oldestIdx].createdAt = time(NULL);
    SSL_SESSION_up_ref(session);
    LOG("DEBUG", "replaced oldest TLS session cache entry for %s:%s", host, port);

    pthread_mutex_unlock(&tlsSessionCacheMutex);
}

// ─────────────────────────────────────────────────────────────────────────────

static void registerSession(Basket * basket) {
    pthread_mutex_lock(&pool.mutex);

    // find an empty slot
    int slotIndex = -1;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (pool.sessionMap[i] == 0) {
            slotIndex = i;
            break;
        }
    }

    if (slotIndex == -1) {
        // TODO more sessions?
        LOG("DEBUG", "no available slot");
    } else {
        pool.sessionMap[slotIndex] = 1;

        pool.sessions[slotIndex] = basket -> session;
        pool.sessions[slotIndex] -> lastUsedTime = time(NULL);

        pool.sessionCount++;
        LOG("DEBUG", "registered a new session %s//:%s:%s#%s//:%s:%s@%s", pool.sessions[slotIndex] -> scheme, pool.sessions[slotIndex] -> host, pool.sessions[slotIndex] -> port, pool.sessions[slotIndex] -> proxy.scheme, pool.sessions[slotIndex] -> proxy.host, pool.sessions[slotIndex] -> proxy.port, pool.sessions[slotIndex] -> proxy.authorization);
    }


    pthread_mutex_unlock(&pool.mutex);
}

void initSharedSessionPool(void) {
    // initialize mutex lock attributes
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
//    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
    pthread_mutex_init(&pool.mutex, &attr);

    // initialize session pool
    pool.sessionCount = 0;
    pool.running = 1;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        pool.sessionMap[i] = 0;
    }

    // initialize TLS session cache
    initTLSSessionCache();
}
