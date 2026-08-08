//
// Created by Intuition on 26-2-7.
//

#include "Session.h"

#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/select.h>
#endif
//#include <pthread/pthread.h>
#include <pthread.h>

#include "Compat.h"

#include "RequestHandler.h"
#include "ResponseHandler.h"
#include "SocketHandler.h"
#include "SSLHandler.h"
#include "BrowserHandler.h"
#include "Log.h"

// common file shared between task thread and daemon thread
#define SHM_NAME "/http2_session_pool"

// ─── TLS Session Cache (for TLS 1.3 session resumption / pre_shared_key) ───
#define MAX_TLS_SESSION_CACHE 256

typedef struct {
    char host[256];
    char port[8];
    // proxy identity is part of the cache key: a TLS session resumed via
    // pre_shared_key must be scoped to the same proxy it was established with.
    char proxyScheme[16];
    char proxyHost[256];
    char proxyPort[8];
    char proxyAuthorization[1024];
    SSL_SESSION *session;
    time_t createdAt;
} TLSSessionCacheEntry;

static TLSSessionCacheEntry tlsSessionCache[MAX_TLS_SESSION_CACHE];
static pthread_mutex_t tlsSessionCacheMutex;

static void initTLSSessionCache(void);
static SSL_SESSION* lookupTLSSession(const char *host, const char *port,
                                     const char *proxyScheme, const char *proxyHost,
                                     const char *proxyPort, const char *proxyAuthorization);
// ─────────────────────────────────────────────────────────────────────────────

static SharedSessionPool pool;

// ── Per-target session-establish locks ─────────────────────────────────────
// A single global lock would serialize find-or-create for ALL targets, so a
// slow TLS handshake to host A would block an unrelated request to host B from
// even starting. Instead each lock is keyed on the full session identity
// (scheme + host + port + proxy), so that:
//   • concurrent requests to the SAME target serialize -> they share ONE
//     connection (HTTP/2 multiplexing) instead of racing to open their own;
//   • requests to DIFFERENT targets establish in parallel.
// Locks are ref-counted and the slot is reclaimed when the last user leaves.
typedef struct {
    char            key[2048];
    pthread_mutex_t lock;
    int             refCount;
    int             inUse;
} EstablishLock;

static EstablishLock establishLocks[MAX_SESSIONS];
static pthread_mutex_t establishRegistryMutex = PTHREAD_MUTEX_INITIALIZER;

// Fallback used only if the lock table is momentarily full (should not happen:
// distinct in-flight targets are bounded by the worker-thread count).
static pthread_mutex_t sessionEstablishFallbackMutex = PTHREAD_MUTEX_INITIALIZER;

static void createSession(Basket *basket);
static Session* initSession(Basket *basket, int sockfd, SSL_CTX * sslCtx, SSL * ssl, TLSConnInfo *connInfo);
static void getSessionInfo(Basket * basket);
static int isProxyError(Basket *basket);
static void markProxySessionsGoingAway(Basket *basket);
static void registerSession(Basket * basket);
static HpackContext *initHpackContext(Basket *basket);
static void freeHpackContext(HpackContext *ctx);
static void buildSessionKey(Basket *basket, char *key, size_t size);
static EstablishLock* acquireEstablishLock(Basket *basket);
static void releaseEstablishLock(EstablishLock *slot);

// ── HTTP/2 multiplexing: per-connection reader thread & stream registry ──
static void* readerLoop(void *arg);
static void readerDispatch(Session *session, unsigned char *payload, uint32_t length,
                           uint8_t type, uint8_t flags, uint32_t streamId);
static Stream* findStream(Session *session, uint32_t streamId);
static void failAllStreams(Session *session, int onlyPending);
static void stopReader(Session *session);

void handleSession(Basket *basket) {
    cleanupSessions(0);

    // The find (getSessionInfo) and create (createSession + registerSession)
    // must be atomic per target: without this, concurrent request threads to the
    // same target all miss the empty pool and each opens its own connection,
    // defeating multiplexing. The lock is scoped to the target so requests to
    // different hosts/proxies still establish concurrently.
    EstablishLock *establishLock = acquireEstablishLock(basket);
    pthread_mutex_t *fallback = NULL;
    if (establishLock == NULL) {
        fallback = &sessionEstablishFallbackMutex;
        pthread_mutex_lock(fallback);
    }

    getSessionInfo(basket);
    if (basket -> session == NULL) {
        createSession(basket);
        if (basket -> session != NULL) {
            registerSession(basket);
        } else if (isProxyError(basket)) {
            // A 407 (proxy authorization failed) or any other proxy-level error means
            // this proxy is currently unusable. Invalidate every pooled session that
            // was established through the same proxy so requests don't keep reusing a
            // dead/forbidden tunnel. See ROADMAP: "Proxy 407 Reusage — Disable reusage
            // if 407". The reaper closes them once in-flight streams drain.
            markProxySessionsGoingAway(basket);
        }
    }

    if (establishLock != NULL) {
        releaseEstablishLock(establishLock);
    } else {
        pthread_mutex_unlock(fallback);
    }
}

void cleanupSessions(int isAll) {
    Session *toClose[MAX_SESSIONS];
    int closeCount = 0;

    // Collect reap candidates while holding pool.mutex, then close them outside
    // the lock: closeSession joins the reader thread, which can block briefly
    // and must never run under pool.mutex.
    pthread_mutex_lock(&pool.mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (pool.sessionMap[i] == 1) {
            Session *session = pool.sessions[i];

            // Never reap a connection with in-flight (multiplexed) streams.
            if (session -> inflightCount > 0) {
                continue;
            }

            if (isAll == 1 // cleanup all sessions before stopping the process
                || session -> goingAway == 1 // GOAWAY / connection loss: not reusable
                || isConnecting(session) != 1 // cleanup closed sessions
                || (time(NULL) - session -> lastUsedTime) > (session -> expirationInMilliseconds) // cleanup expired sessions
            ) {
                LOG("DEBUG", "cleanup session %s//:%s:%s#%s//:%s:%s@%s", session -> scheme, session -> host, session -> port, session -> proxy.scheme, session -> proxy.host, session -> proxy.port, session -> proxy.authorization);
                toClose[closeCount++] = session;
                pool.sessions[i] = NULL;
                pool.sessionMap[i] = 0;
                pool.sessionCount--;
            }
        }
    }
    pthread_mutex_unlock(&pool.mutex);

    for (int i = 0; i < closeCount; i++) {
        closeSession(toClose[i], ERR_NONE);
    }

    LOG("DEBUG", "session count: %lu", pool.sessionCount);
}

void cleanupTargetSession(Basket *basket) {
    Session *target = NULL;

    // remove session from session pool under the pool lock
    pthread_mutex_lock(&pool.mutex);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (basket -> session == pool.sessions[i]) {
            target = pool.sessions[i];
            pool.sessions[i] = NULL;
            pool.sessionMap[i] = 0;
            pool.sessionCount--;
            break;
        }
    }
    pthread_mutex_unlock(&pool.mutex);

    // free session outside the pool lock (idempotent if not found in the pool)
    if (target != NULL) {
        closeSession(target, basket -> error);
    }
    basket -> session = NULL;
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

    // Stop the reader thread before tearing down the SSL object it reads from,
    // then wake any streams still waiting (defensive; cleanup skips inflight>0).
    stopReader(session);
    failAllStreams(session, 0);

    pthread_mutex_lock(&(session -> lock));

    // clear SSL app_data before freeing SSL (so callback won't access freed connInfo)
    if (session -> ssl) {
        SSL_set_app_data(session -> ssl, NULL);
    }
    if (session -> connInfo) {
        free(session -> connInfo);
        session -> connInfo = NULL;
    }

    // Prefer the recorded connection-level error (e.g. SETTINGS_TIMEOUT) so
    // freeSession can skip SSL_shutdown when it would crash.
    Error effectiveError = session -> connError.code != NULL ? session -> connError : error;

    // free(session -> id);
    freeSession(session -> ssl, session -> sslCtx, session -> sockfd, session -> hpackCtx, effectiveError);
    session -> isActive = SESSION_INACTIVE;

    pthread_mutex_unlock(&session -> lock);

    pthread_mutex_destroy(&session -> writeMutex);
    pthread_mutex_destroy(&session -> streamsMutex);

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

// Build the session identity key used to scope the establish lock. It must
// include exactly the fields getSessionInfo() matches on, so two requests share
// a lock iff they would share a session.
static void buildSessionKey(Basket *basket, char *key, size_t size) {
    snprintf(key, size, "%s://%s:%s#%s://%s:%s@%s",
             basket -> request.urlComponents.scheme,
             basket -> request.urlComponents.host,
             basket -> request.urlComponents.port,
             basket -> proxy.scheme,
             basket -> proxy.host,
             basket -> proxy.port,
             basket -> proxy.authorization);
}

// Find-or-create the per-target lock, take a reference, and lock it. Returns
// NULL only if the table is full, in which case the caller falls back to the
// global mutex. The reference taken under establishRegistryMutex guarantees the
// slot cannot be reclaimed while we block on (and then hold) its lock.
static EstablishLock* acquireEstablishLock(Basket *basket) {
    char key[2048];
    buildSessionKey(basket, key, sizeof(key));

    pthread_mutex_lock(&establishRegistryMutex);
    EstablishLock *slot = NULL;
    int freeIdx = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (establishLocks[i].inUse) {
            if (strcmp(establishLocks[i].key, key) == 0) {
                slot = &establishLocks[i];
                break;
            }
        } else if (freeIdx < 0) {
            freeIdx = i;
        }
    }
    if (slot == NULL && freeIdx >= 0) {
        slot = &establishLocks[freeIdx];
        strncpy(slot -> key, key, sizeof(slot -> key) - 1);
        slot -> key[sizeof(slot -> key) - 1] = '\0';
        pthread_mutex_init(&slot -> lock, NULL);
        slot -> refCount = 0;
        slot -> inUse = 1;
    }
    if (slot != NULL) {
        slot -> refCount++;
    }
    pthread_mutex_unlock(&establishRegistryMutex);

    if (slot != NULL) {
        // Held across the TLS handshake by design (same-target requests wait).
        pthread_mutex_lock(&slot -> lock);
    }
    return slot;
}

// Unlock the per-target lock and drop the reference, reclaiming the slot when
// no other request is using this target.
static void releaseEstablishLock(EstablishLock *slot) {
    pthread_mutex_unlock(&slot -> lock);

    pthread_mutex_lock(&establishRegistryMutex);
    slot -> refCount--;
    if (slot -> refCount == 0) {
        pthread_mutex_destroy(&slot -> lock);
        slot -> inUse = 0;
        slot -> key[0] = '\0';
    }
    pthread_mutex_unlock(&establishRegistryMutex);
}

static void getSessionInfo(Basket * basket) {
    pthread_mutex_lock(&pool.mutex);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (pool.sessionMap[i] == 1
            && pool.sessions[i] -> magic == SESSION_MAGIC
            && pool.sessions[i] -> goingAway == 0
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
                strncpy(basket -> proxy.response, pool.sessions[i] -> proxy.response, sizeof(basket -> proxy.response) - 1);
                basket -> proxy.response[sizeof(basket -> proxy.response) - 1] = '\0';
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
    // if a cached TLS session exists for this host:port, set it for session resumption (pre_shared_key).
    // Only offer it when the browser profile advertises pre_shared_key (41); iOS Chrome (CriOS) omits
    // the extension, so skipping SSL_set_session keeps it out of the ClientHello. psk_key_exchange_modes
    // (45) is unaffected as BoringSSL sends it independently for any TLS 1.3-capable client.
    const BrowserFingerprint *fp = getBrowserFingerprint(basket -> browserType);
    if (fp != NULL && fp -> enablePreSharedKey) {
        SSL_SESSION *cachedSession = lookupTLSSession(basket -> request.urlComponents.host,
                                                      basket -> request.urlComponents.port,
                                                      basket -> proxy.scheme,
                                                      basket -> proxy.host,
                                                      basket -> proxy.port,
                                                      basket -> proxy.authorization);
        if (cachedSession) {
            SSL_set_session(ssl, cachedSession);
            LOG("DEBUG", "resuming TLS session for %s:%s via proxy %s://%s:%s",
                basket -> request.urlComponents.host, basket -> request.urlComponents.port,
                basket -> proxy.scheme, basket -> proxy.host, basket -> proxy.port);
        }
    }

    // set conn info on SSL so the new-session callback can cache the session
    // the connInfo lives for the session's lifetime (TLS 1.3 NewSessionTicket is async)
    TLSConnInfo *connInfo = malloc(sizeof(TLSConnInfo));
    connInfo -> host = basket -> request.urlComponents.host;
    connInfo -> port = basket -> request.urlComponents.port;
    connInfo -> proxyScheme = basket -> proxy.scheme;
    connInfo -> proxyHost = basket -> proxy.host;
    connInfo -> proxyPort = basket -> proxy.port;
    connInfo -> proxyAuthorization = basket -> proxy.authorization;
    SSL_set_app_data(ssl, connInfo);

    int connect = SSL_connect(ssl);

    if (connect != 1) {
        // capture diagnostics BEFORE freeSession(): SSL_shutdown/SSL_free may drain the error queue,
        // which previously produced the meaningless "error:00000000:invalid library (0)"
        const int sslError = SSL_get_error(ssl, connect);
        const int savedErrno = errno;
        char errBuf[256] = "no error in queue";
        const unsigned long queuedError = ERR_get_error();
        if (queuedError != 0) {
            ERR_error_string_n(queuedError, errBuf, sizeof(errBuf));
        }

        SSL_set_app_data(ssl, NULL);
        free(connInfo);
        freeSession(ssl, sslCtx, sockfd, NULL, basket -> error);

        if (sslError == SSL_ERROR_SYSCALL) {
            // no entry in the error queue by design: connection reset / EOF during handshake
            LOG("ERROR", "SSL_connect failed: SSL_ERROR_SYSCALL, %s (errno: %d)", strerror(savedErrno), savedErrno);
        } else {
            LOG("ERROR", "SSL_connect failed: ssl error %d, %s", sslError, errBuf);
        }
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

    // Start the per-connection reader thread: it owns SSL_read and demultiplexes
    // inbound frames by stream id for all concurrent requests on this session.
    session -> readerRunning = 1;
    if (pthread_create(&session -> reader, NULL, readerLoop, session) != 0) {
        LOG("ERROR", "failed to start connection reader thread");
        session -> readerRunning = 0;
        basket -> error = ERR_RESPONSE_READING_CONNECTION_ERROR;
        closeSession(session, basket -> error); // session owns ssl/ctx/sockfd/connInfo
        return;
    }
    session -> readerStarted = 1;

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

    // Record the fingerprint profile this connection was established with.
    strncpy(session -> clientHelloId, basket -> clientHelloId, sizeof(session -> clientHelloId) - 1);
    session -> clientHelloId[sizeof(session -> clientHelloId) - 1] = '\0';

    session -> sockfd = sockfd;
    session -> sslCtx = sslCtx;
    session -> ssl = ssl;
    session -> isActive = SESSION_ACTIVE;
    session -> magic = SESSION_MAGIC;

    session -> creationTime = time(NULL);

    session -> expirationInMilliseconds = basket -> sessionExpirationInMilliseconds;

    session -> proxy = (Proxy) { 0, 0, 0, 0, 0 };
    strncpy(session -> proxy.scheme, basket -> proxy.scheme, sizeof(session -> proxy.scheme) - 1);
    session -> proxy.scheme[sizeof(session -> proxy.scheme) - 1] = '\0';
    strncpy(session -> proxy.host, basket -> proxy.host, sizeof(session -> proxy.host) - 1);
    session -> proxy.host[sizeof(session -> proxy.host) - 1] = '\0';
    strncpy(session -> proxy.port, basket -> proxy.port, sizeof(session -> proxy.port) - 1);
    session -> proxy.port[sizeof(session -> proxy.port) - 1] = '\0';
    strncpy(session -> proxy.authorization, basket -> proxy.authorization, sizeof(session -> proxy.authorization) - 1);
    session -> proxy.authorization[sizeof(session -> proxy.authorization) - 1] = '\0';
    strncpy(session -> proxy.response, basket -> proxy.response, sizeof(session -> proxy.response) - 1);
    session -> proxy.response[sizeof(session -> proxy.response) - 1] = '\0';

    session -> hpackCtx = initHpackContext(basket);
    session -> connInfo = connInfo; // ownership transferred from createSession

    atomic_init(&session -> streamId, 1);

    pthread_mutex_init(&(session -> lock), NULL);

    // ── HTTP/2 multiplexing / concurrency state ──
    session -> inflightCount = 0;
    session -> goingAway = 0;
    session -> connError = ERR_NONE;
    session -> readerRunning = 0;
    session -> readerStarted = 0;
    for (int i = 0; i < MAX_CONCURRENT_STREAMS_PER_SESSION; i++) {
        session -> streams[i] = NULL;
    }
    pthread_mutex_init(&(session -> writeMutex), NULL);
    pthread_mutex_init(&(session -> streamsMutex), NULL);

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

// ─── HTTP/2 multiplexing: reader thread & stream registry ───

// Caller must hold session->streamsMutex.
static Stream* findStream(Session *session, uint32_t streamId) {
    for (int i = 0; i < MAX_CONCURRENT_STREAMS_PER_SESSION; i++) {
        if (session -> streams[i] != NULL && session -> streams[i] -> streamId == streamId) {
            return session -> streams[i];
        }
    }
    return NULL;
}

// Wake up streams that will never complete. With onlyPending set, only streams
// that have not received any response yet are failed (used for a graceful
// GOAWAY, so streams already receiving data can finish); otherwise every
// unfinished stream is failed (used on connection loss).
static void failAllStreams(Session *session, int onlyPending) {
    pthread_mutex_lock(&session -> streamsMutex);
    for (int i = 0; i < MAX_CONCURRENT_STREAMS_PER_SESSION; i++) {
        Stream *stream = session -> streams[i];
        if (stream == NULL) { continue; }
        pthread_mutex_lock(&stream -> lock);
        if (!stream -> isEnded && (!onlyPending || stream -> numHeaders == 0)) {
            stream -> error = session -> connError.code != NULL
                              ? session -> connError
                              : ERR_RESPONSE_READING_CONNECTION_ERROR;
            stream -> isEnded = 1;
            pthread_cond_signal(&stream -> cond);
        }
        pthread_mutex_unlock(&stream -> lock);
    }
    pthread_mutex_unlock(&session -> streamsMutex);
}

// Route one parsed frame either to its target stream (locked) or, for
// connection-level frames and frames for unknown/closed streams, straight to
// the frame handler with a NULL stream. Runs only on the reader thread, so the
// shared HPACK context is touched by a single thread in wire-arrival order.
static void readerDispatch(Session *session, unsigned char *payload, uint32_t length,
                           uint8_t type, uint8_t flags, uint32_t streamId) {
    // DATA(0x0) / HEADERS(0x1) / RST_STREAM(0x3) are stream-scoped.
    if (type == 0x0 || type == 0x1 || type == 0x3) {
        pthread_mutex_lock(&session -> streamsMutex);
        Stream *stream = findStream(session, streamId);
        if (stream != NULL) {
            pthread_mutex_lock(&stream -> lock);
            pthread_mutex_unlock(&session -> streamsMutex);
            handleStreamFrame(session, stream, payload, length, type, flags, streamId);
            if (stream -> isEnded) {
                pthread_cond_signal(&stream -> cond);
            }
            pthread_mutex_unlock(&stream -> lock);
        } else {
            pthread_mutex_unlock(&session -> streamsMutex);
            // Unknown/closed stream: still process (HEADERS keep HPACK in sync).
            handleStreamFrame(session, NULL, payload, length, type, flags, streamId);
        }
    } else {
        // Connection-level frame (SETTINGS/WINDOW_UPDATE/GOAWAY/PING/...).
        handleStreamFrame(session, NULL, payload, length, type, flags, streamId);
        if (type == 0x4 && (flags & 0x1) == 0) {
            // Server SETTINGS (not an ACK): the peer must be acknowledged with
            // an empty SETTINGS frame carrying the ACK flag, or a strict server
            // (e.g. nghttp2) GOAWAYs with SETTINGS_TIMEOUT. Serialize the write
            // with the request threads via writeMutex.
            static const unsigned char settingsAck[9] = {0, 0, 0, 0x4, 0x1, 0, 0, 0, 0};
            pthread_mutex_lock(&session -> writeMutex);
            SSL_write(session -> ssl, settingsAck, sizeof(settingsAck));
            pthread_mutex_unlock(&session -> writeMutex);
        } else if (type == 0x6 && (flags & 0x1) == 0 && length == 8) {
            // PING (not an ACK): echo the 8-byte opaque payload back with the
            // ACK flag set so a keep-alive probe on a long-lived multiplexed
            // connection does not tear it down.
            unsigned char pingAck[17] = {0, 0, 8, 0x6, 0x1, 0, 0, 0, 0};
            memcpy(pingAck + 9, payload, 8);
            pthread_mutex_lock(&session -> writeMutex);
            SSL_write(session -> ssl, pingAck, sizeof(pingAck));
            pthread_mutex_unlock(&session -> writeMutex);
        }
        if (type == 0x7) { // GOAWAY: fail streams that have no response yet
            failAllStreams(session, 1);
        }
    }
}

static void* readerLoop(void *arg) {
    Session *session = (Session *) arg;
    SSL *ssl = session -> ssl;
    int fd = SSL_get_fd(ssl);

    // The socket stays in blocking mode so the request threads' SSL_write path
    // (which treats a short write as fatal) keeps working. Each blocking SSL_read
    // is gated behind select() with a short timeout so this loop can observe
    // readerRunning and exit promptly on shutdown. SSL_pending() is checked first:
    // one TLS record can hold several frames already decrypted and buffered inside
    // the SSL object, with nothing left readable at the socket layer.
    //
    // Known limitation: this SSL_read runs concurrently with the request threads'
    // SSL_write (serialized among themselves via writeMutex). Under TLS 1.3 a
    // post-handshake message (e.g. KeyUpdate) can make SSL_read emit a write from
    // inside this thread, bypassing writeMutex and racing a concurrent SSL_write.
    // It is rare for these short-lived connections and currently left unhandled.
    unsigned char *acc = NULL;
    size_t accSize = 0;

    while (session -> readerRunning) {
        if (SSL_pending(ssl) == 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms: bounds shutdown latency, not response latency

            int sr = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (sr == 0) {
                continue; // timeout: re-check readerRunning
            }
            if (sr < 0) {
#ifndef _WIN32
                if (errno == EINTR) { continue; }
#endif
                LOG("ERROR", "reader: select() failed: %s (errno: %d)", strerror(errno), errno);
                session -> connError = ERR_RESPONSE_READING_CONNECTION_ERROR;
                session -> goingAway = 1;
                failAllStreams(session, 0);
                break;
            }
        }

        unsigned char buffer[16384];
        int bytesRead = SSL_read(ssl, buffer, sizeof(buffer));

        if (bytesRead > 0) {
            unsigned char *newAcc = realloc(acc, accSize + bytesRead);
            if (newAcc == NULL) {
                LOG("ERROR", "reader: accumulation buffer allocation failed");
                session -> connError = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
                session -> goingAway = 1;
                failAllStreams(session, 0);
                break;
            }
            acc = newAcc;
            memcpy(acc + accSize, buffer, bytesRead);
            accSize += bytesRead;

            // Parse as many complete frames (9-byte header + payload) as buffered.
            size_t offset = 0;
            while (offset + 9 <= accSize) {
                uint32_t frameLength = (acc[offset] << 16) | (acc[offset + 1] << 8) | acc[offset + 2];
                uint8_t frameType = acc[offset + 3];
                uint8_t frameFlags = acc[offset + 4];
                uint32_t streamId = ((acc[offset + 5] & 0x7F) << 24) | (acc[offset + 6] << 16) | (acc[offset + 7] << 8) | acc[offset + 8];

                if (offset + 9 + frameLength > accSize) {
                    break; // wait for the rest of this frame
                }

                readerDispatch(session, acc + offset + 9, frameLength, frameType, frameFlags, streamId);
                offset += 9 + frameLength;
            }

            if (offset > 0) {
                size_t remaining = accSize - offset;
                if (remaining > 0) {
                    memmove(acc, acc + offset, remaining);
                }
                accSize = remaining;
            }
        } else {
            int err = SSL_get_error(ssl, bytesRead);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue; // not enough for a full record yet; re-select
            }
            // bytesRead == 0 (peer closed) or a fatal error: end all streams.
            LOG("DEBUG", "reader: connection closed/error (ret=%d, err=%d)", bytesRead, err);
            session -> goingAway = 1;
            failAllStreams(session, 0);
            break;
        }
    }

    if (acc) { free(acc); }
    return NULL;
}

static void stopReader(Session *session) {
    if (session -> readerStarted) {
        session -> readerRunning = 0;
        pthread_join(session -> reader, NULL);
        session -> readerStarted = 0;
    }
}

Stream* registerStream(Basket *basket) {
    Session *session = basket -> session;

    Stream *stream = calloc(1, sizeof(Stream));
    if (stream == NULL) {
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }
    stream -> headers = malloc(sizeof(ResponseHeader) * RESPONSE_HEADERS_MAX_SIZE);
    if (stream -> headers == NULL) {
        free(stream);
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return NULL;
    }
    stream -> numHeaders = 0;
    stream -> isEnded = 0;
    stream -> combinedPayload = NULL;
    stream -> combinedPayloadSize = 0;
    stream -> error = ERR_NONE;
    pthread_mutex_init(&stream -> lock, NULL);
    pthread_cond_init(&stream -> cond, NULL);

    // Assign an odd client-initiated stream id (1, 3, 5, ...).
    unsigned int id = atomic_fetch_add(&session -> streamId, 2);
    stream -> streamId = id;
    basket -> streamId = id; // consumed by the HEADERS/DATA send path

    pthread_mutex_lock(&session -> streamsMutex);
    int slot = -1;
    for (int i = 0; i < MAX_CONCURRENT_STREAMS_PER_SESSION; i++) {
        if (session -> streams[i] == NULL) { slot = i; break; }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&session -> streamsMutex);
        pthread_cond_destroy(&stream -> cond);
        pthread_mutex_destroy(&stream -> lock);
        free(stream -> headers);
        free(stream);
        LOG("ERROR", "no free stream slot on session (max %d concurrent)", MAX_CONCURRENT_STREAMS_PER_SESSION);
        basket -> error = ERR_RESPONSE_READING_CONNECTION_ERROR;
        return NULL;
    }
    session -> streams[slot] = stream;
    session -> inflightCount++;
    pthread_mutex_unlock(&session -> streamsMutex);

    return stream;
}

int awaitStream(Basket *basket, Stream *stream) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long ms = basket -> responseReadingTimeoutInMilliseconds;
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&stream -> lock);
    int rc = 0;
    while (!stream -> isEnded) {
        rc = pthread_cond_timedwait(&stream -> cond, &stream -> lock, &deadline);
        if (rc == ETIMEDOUT) { break; }
    }
    int ended = stream -> isEnded;
    int received = (stream -> numHeaders > 0 || stream -> combinedPayloadSize > 0);
    pthread_mutex_unlock(&stream -> lock);

    if (!ended) {
        LOG("ERROR", "no response after %dms timeout", basket -> responseReadingTimeoutInMilliseconds);
        basket -> error = received ? ERR_RESPONSE_NO_CONTENT_AFTER_READING_TIMEOUT
                                   : ERR_RESPONSE_PARTIAL_CONTENT_AFTER_READING_TIMEOUT;
        return -1;
    }
    return 0;
}

void unregisterStream(Basket *basket, Stream *stream) {
    if (stream == NULL) { return; }
    Session *session = basket -> session;

    // Detach from the registry first so the reader can no longer route new
    // frames to this stream.
    pthread_mutex_lock(&session -> streamsMutex);
    for (int i = 0; i < MAX_CONCURRENT_STREAMS_PER_SESSION; i++) {
        if (session -> streams[i] == stream) {
            session -> streams[i] = NULL;
            break;
        }
    }
    if (session -> inflightCount > 0) { session -> inflightCount--; }
    pthread_mutex_unlock(&session -> streamsMutex);

    // Barrier: ensure any in-progress reader access to this stream has finished
    // before we free it.
    pthread_mutex_lock(&stream -> lock);
    pthread_mutex_unlock(&stream -> lock);

    freeStreamBuffers(stream);
    pthread_cond_destroy(&stream -> cond);
    pthread_mutex_destroy(&stream -> lock);
    free(stream);
}

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

static SSL_SESSION* lookupTLSSession(const char *host, const char *port,
                                      const char *proxyScheme, const char *proxyHost,
                                      const char *proxyPort, const char *proxyAuthorization) {
    pthread_mutex_lock(&tlsSessionCacheMutex);
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session != NULL
            && strcmp(tlsSessionCache[i].host, host) == 0
            && strcmp(tlsSessionCache[i].port, port) == 0
            && strcmp(tlsSessionCache[i].proxyScheme, proxyScheme) == 0
            && strcmp(tlsSessionCache[i].proxyHost, proxyHost) == 0
            && strcmp(tlsSessionCache[i].proxyPort, proxyPort) == 0
            && strcmp(tlsSessionCache[i].proxyAuthorization, proxyAuthorization) == 0) {
            SSL_SESSION *session = tlsSessionCache[i].session;
            SSL_SESSION_up_ref(session);
            pthread_mutex_unlock(&tlsSessionCacheMutex);
            return session;
        }
    }
    pthread_mutex_unlock(&tlsSessionCacheMutex);
    return NULL;
}

// Fill a cache slot, copying the host:port + proxy identity key and taking a
// reference on the SSL_SESSION (the caller keeps its own reference).
static void cacheTLSSessionFillSlot(TLSSessionCacheEntry *slot,
                                    const char *host, const char *port,
                                    const char *proxyScheme, const char *proxyHost,
                                    const char *proxyPort, const char *proxyAuthorization,
                                    SSL_SESSION *session) {
    strncpy(slot -> host, host, sizeof(slot -> host) - 1);
    slot -> host[sizeof(slot -> host) - 1] = '\0';
    strncpy(slot -> port, port, sizeof(slot -> port) - 1);
    slot -> port[sizeof(slot -> port) - 1] = '\0';
    strncpy(slot -> proxyScheme, proxyScheme, sizeof(slot -> proxyScheme) - 1);
    slot -> proxyScheme[sizeof(slot -> proxyScheme) - 1] = '\0';
    strncpy(slot -> proxyHost, proxyHost, sizeof(slot -> proxyHost) - 1);
    slot -> proxyHost[sizeof(slot -> proxyHost) - 1] = '\0';
    strncpy(slot -> proxyPort, proxyPort, sizeof(slot -> proxyPort) - 1);
    slot -> proxyPort[sizeof(slot -> proxyPort) - 1] = '\0';
    strncpy(slot -> proxyAuthorization, proxyAuthorization, sizeof(slot -> proxyAuthorization) - 1);
    slot -> proxyAuthorization[sizeof(slot -> proxyAuthorization) - 1] = '\0';
    slot -> session = session;
    slot -> createdAt = time(NULL);
    SSL_SESSION_up_ref(session);
}

void cacheTLSSession(const char *host, const char *port, SSL_SESSION *session,
                     const char *proxyScheme, const char *proxyHost,
                     const char *proxyPort, const char *proxyAuthorization) {
    pthread_mutex_lock(&tlsSessionCacheMutex);

    // update existing entry
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session != NULL
            && strcmp(tlsSessionCache[i].host, host) == 0
            && strcmp(tlsSessionCache[i].port, port) == 0
            && strcmp(tlsSessionCache[i].proxyScheme, proxyScheme) == 0
            && strcmp(tlsSessionCache[i].proxyHost, proxyHost) == 0
            && strcmp(tlsSessionCache[i].proxyPort, proxyPort) == 0
            && strcmp(tlsSessionCache[i].proxyAuthorization, proxyAuthorization) == 0) {
            SSL_SESSION_free(tlsSessionCache[i].session);
            cacheTLSSessionFillSlot(&tlsSessionCache[i], host, port,
                                    proxyScheme, proxyHost, proxyPort, proxyAuthorization, session);
            LOG("DEBUG", "updated TLS session cache for %s:%s via proxy %s://%s:%s",
                host, port, proxyScheme, proxyHost, proxyPort);
            pthread_mutex_unlock(&tlsSessionCacheMutex);
            return;
        }
    }

    // find empty slot
    for (int i = 0; i < MAX_TLS_SESSION_CACHE; i++) {
        if (tlsSessionCache[i].session == NULL) {
            cacheTLSSessionFillSlot(&tlsSessionCache[i], host, port,
                                    proxyScheme, proxyHost, proxyPort, proxyAuthorization, session);
            LOG("DEBUG", "cached TLS session for %s:%s via proxy %s://%s:%s",
                host, port, proxyScheme, proxyHost, proxyPort);
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
    cacheTLSSessionFillSlot(&tlsSessionCache[oldestIdx], host, port,
                            proxyScheme, proxyHost, proxyPort, proxyAuthorization, session);
    LOG("DEBUG", "replaced oldest TLS session cache entry for %s:%s via proxy %s://%s:%s",
        host, port, proxyScheme, proxyHost, proxyPort);

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

// True when the basket's error is a proxy-level failure (e.g. 407 authorization
// failed, proxy socket/connect errors). These mean the proxy tunnel itself is
// unusable, so any session riding that proxy must stop being reused.
static int isProxyError(Basket *basket) {
    if (basket -> error.code == NULL) {
        return 0;
    }
    return strcmp(basket -> error.code, ERR_PROXY_AUTHORIZATION_FAILED.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SOCKET_NONBLOCK_SETTING_FAILED.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_FAILED.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SEND_CONNECT_REQUEST_FAILED.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_UNEXPECTED_RESPONSE.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_TIMEOUT.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR.code) == 0
        || strcmp(basket -> error.code, ERR_PROXY_SOCKET_CONNECTING_REFUSED.code) == 0;
}

// Mark every pooled session established through the same proxy as goingAway so
// it is no longer handed out by getSessionInfo(). Mirrors the connection-reuse
// key (scheme/host/port/authorization) so only the offending proxy is affected.
static void markProxySessionsGoingAway(Basket *basket) {
    pthread_mutex_lock(&pool.mutex);
    int invalidated = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = pool.sessions[i];
        if (s == NULL || s -> magic != SESSION_MAGIC || s -> goingAway == 1) {
            continue;
        }
        if (strlen(basket -> proxy.host) > 0) {
            if (strcmp(s -> proxy.scheme, basket -> proxy.scheme) == 0
                && strcmp(s -> proxy.host, basket -> proxy.host) == 0
                && strcmp(s -> proxy.port, basket -> proxy.port) == 0
                && strcmp(s -> proxy.authorization, basket -> proxy.authorization) == 0) {
                s -> goingAway = 1;
                invalidated++;
            }
        } else {
            // no proxy configured: only invalidate direct (proxy-less) sessions
            if (strlen(s -> proxy.host) == 0) {
                s -> goingAway = 1;
                invalidated++;
            }
        }
    }
    pthread_mutex_unlock(&pool.mutex);
    if (invalidated > 0) {
        LOG("WARN", "proxy %s://%s:%s unusable (407/auth/connect error); disabled %d pooled session(s) from reuse",
            basket -> proxy.scheme, basket -> proxy.host, basket -> proxy.port, invalidated);
    }
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
