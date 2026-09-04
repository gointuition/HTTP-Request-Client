//
// Created by Intuition on 26-8-23.
//
// HTTP/1.1 request side: build the wire request from the basket and send it on
// the session socket. One exchange at a time per connection, serialized via
// session->writeMutex.
//

#include "Http11RequestHandler.h"
#include "Http11ResponseHandler.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include "Compat.h"

#include "Http2RequestHandler.h"
#include "UrlParser.h"
#include "Common.h"
#include "Error.h"
#include "ResponseStream.h"
#include "Log.h"

typedef struct {
    Basket *basket;
    Stream *stream;
    bool logEnabled; // per-request log flag is thread-local; carry it over
} Http11Exchange;

static void handleHttp11RequestAsync(Basket *basket, Stream *stream);
static void handleHttp11RequestSync(Basket *basket, Stream *stream);
static void *http11ExchangeThread(void *arg);
static void endStream(Stream *stream);

static char* buildHttp11Request(Basket *basket, size_t *outLen);
static void appendRequestLine(StrBuf *sb, Basket *basket, const char *target);
static int appendHeaders(StrBuf *sb, Basket *basket, StrBuf *cookies);
static void appendHostFallback(StrBuf *sb, URLComponents *url);
static void appendCookieHeader(StrBuf *sb, StrBuf *cookies);
static void appendProxyAuthorization(StrBuf *sb, Basket *basket);

static int sessionWriteAll(Session *session, int nonBlocking, int timeoutInMillis,
                           const unsigned char *buf, size_t len);
static int plainWriteAll(Session *session, int nonBlocking, int timeoutInMillis,
                         const unsigned char *buf, size_t len);
static int isRetryableSocketError(int lastError);

// ─── orchestrator ───

// async=0 runs the whole exchange inline (sync handleRequest); async=1 moves it
// to a background thread and returns right after it is started, mirroring the
// HTTP/2 fire-and-forget flow (handleResponse reaps the result via stream->isEnded).
void handleHttp11Request(Basket *basket, Stream *stream, int async) {
    if (async) {
        handleHttp11RequestAsync(basket, stream);
    } else {
        handleHttp11RequestSync(basket, stream);
    }
}

// Start the exchange on a detached background thread and return immediately.
static void handleHttp11RequestAsync(Basket *basket, Stream *stream) {
    Http11Exchange *exchange = malloc(sizeof(Http11Exchange));
    if (exchange == NULL) {
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return;
    }
    exchange -> basket = basket;
    exchange -> stream = stream;
    exchange -> logEnabled = getLogEnabled();

    pthread_t thread;
    const int createFailed = pthread_create(&thread, NULL, http11ExchangeThread, exchange) != 0;
    if (createFailed) {
        free(exchange);
        stream -> error = ERR_REQUEST_SENDING_HTTP11_REQUEST_FAILED;
        basket -> error = stream -> error;
        endStream(stream); // let handleResponse() reap it as completed-with-error
        return;
    }
    pthread_detach(thread);
}

// One serialized HTTP/1.1 exchange, inline on the caller's thread (sync path)
// or on the async background thread: writeMutex spans send + response read so
// exchanges on the same connection never interleave (queued async exchanges
// wait here, one at a time).
static void handleHttp11RequestSync(Basket *basket, Stream *stream) {
    Session *session = basket -> session;
    pthread_mutex_lock(&session -> writeMutex);

    // after the lock: the reading timeout starts when this exchange actually runs
    struct timespec deadline;
    buildResponseDeadline(&deadline, basket -> responseReadingTimeoutInMilliseconds);

    sendHttp11Request(basket, stream);
    if (stream -> error.code == NULL) {
        receiveHttp11Response(basket, stream, &deadline);
    }

    endStream(stream);
    pthread_mutex_unlock(&session -> writeMutex);

    if (stream -> error.code != NULL) {
        basket -> error = stream -> error;
    }
}

static void *http11ExchangeThread(void *arg) {
    Http11Exchange *exchange = arg;
    setLogEnabled(exchange -> logEnabled);
    handleHttp11RequestSync(exchange -> basket, exchange -> stream);
    free(exchange);
    return NULL;
}

static void endStream(Stream *stream) {
    pthread_mutex_lock(&stream -> lock);
    stream -> isEnded = 1;
    pthread_cond_signal(&stream -> cond);
    completeResponseSink(stream);
    pthread_mutex_unlock(&stream -> lock);
}

// ─── request sending ───

void sendHttp11Request(Basket *basket, Stream *stream) {
    Session *session = basket -> session;

    size_t requestLen = 0;
    char *request = buildHttp11Request(basket, &requestLen);
    if (request == NULL) {
        stream -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return;
    }
    LOG("DEBUG", "http/1.1 request:\n%s", request);

    const int writeRc = sessionWriteAll(session, basket -> nonBlocking,
                                        basket -> connectTimeoutInMilliseconds,
                                        (const unsigned char *) request, requestLen);
    free(request);
    if (writeRc != 1) {
        LOG("ERROR", "http/1.1: failed to send request");
        stream -> error = ERR_REQUEST_SENDING_HTTP11_REQUEST_FAILED;
        session -> goingAway = 1;
    }
}

// ─── request building ───

static char* buildHttp11Request(Basket *basket, size_t *outLen) {
    Session *session = basket -> session;
    URLComponents *url = &basket -> request.urlComponents;

    // absolute-form through a CONNECT-less proxy, else origin-form
    char target[4352];
    if (session -> plainProxy) {
        snprintf(target, sizeof(target), "http://%s:%s%s", url -> host, url -> port, url -> path);
    } else {
        snprintf(target, sizeof(target), "%s", url -> path);
    }

    StrBuf sb = { NULL, 0, 0, 0 };
    StrBuf cookies = { NULL, 0, 0, 0 };

    appendRequestLine(&sb, basket, target);
    const int headersOk = appendHeaders(&sb, basket, &cookies);
    if (!headersOk) {
        appendHostFallback(&sb, url);
    }
    appendCookieHeader(&sb, &cookies);
    appendProxyAuthorization(&sb, basket);
    sbAppendStr(&sb, "\r\n");
    if (basket -> request.payload != NULL) {
        sbAppendStr(&sb, basket -> request.payload);
    }
    free(cookies.data);

    if (sb.failed) {
        free(sb.data);
        return NULL;
    }
    sb.data[sb.len] = '\0';
    *outLen = sb.len;
    return sb.data;
}

static void appendRequestLine(StrBuf *sb, Basket *basket, const char *target) {
    char requestLine[4608];
    const int lineLen = snprintf(requestLine, sizeof(requestLine), "%s %s HTTP/1.1\r\n",
                                 basket -> method, target);
    sbAppend(sb, requestLine, (size_t) lineLen);
}

// Emits real headers; ":authority" becomes Host in place (wire-order
// fingerprint), cookies are collected for a single merged Cookie header.
// Returns 1 when Host was emitted.
static int appendHeaders(StrBuf *sb, Basket *basket, StrBuf *cookies) {
    int hostEmitted = 0;
    for (size_t i = 0; i < basket -> request.numHeaders; i++) {
        const RequestHeader *h = &basket -> request.headers[i];

        if (h -> isPseudo) {
            const int isAuthority = strcasecmp(h -> name, ":authority") == 0;
            if (isAuthority && !hostEmitted) {
                sbAppendStr(sb, "Host: ");
                sbAppendStr(sb, h -> value);
                sbAppendStr(sb, "\r\n");
                hostEmitted = 1;
            }
            continue; // :method / :scheme / :path are carried by the request line
        }

        const int isCookie = strcasecmp(h -> name, "cookie") == 0;
        if (isCookie) {
            if (cookies -> len > 0) {
                sbAppendStr(cookies, "; ");
            }
            sbAppendStr(cookies, h -> value);
            continue;
        }

        if (strcasecmp(h -> name, "host") == 0) {
            hostEmitted = 1;
        }

        sbAppendStr(sb, h -> name);
        sbAppendStr(sb, ": ");
        sbAppendStr(sb, h -> value);
        sbAppendStr(sb, "\r\n");
    }
    return hostEmitted;
}

static void appendHostFallback(StrBuf *sb, URLComponents *url) {
    char *authority = getHeaderAuthority(url -> host, url -> port);
    if (authority == NULL) { return; }
    sbAppendStr(sb, "Host: ");
    sbAppendStr(sb, authority);
    sbAppendStr(sb, "\r\n");
    free(authority);
}

static void appendCookieHeader(StrBuf *sb, StrBuf *cookies) {
    if (cookies -> len == 0) { return; }
    sbAppendStr(sb, "Cookie: ");
    sbAppend(sb, cookies -> data, cookies -> len);
    sbAppendStr(sb, "\r\n");
}

/**
 * For http, not https
 */
static void appendProxyAuthorization(StrBuf *sb, Basket *basket) {
    Session *session = basket -> session;
    const int hasCredentials = strlen(basket -> proxy.authorization) > 0;
    if (!session -> plainProxy || !hasCredentials) { return; }
    sbAppendStr(sb, "Proxy-Authorization: ");
    sbAppendStr(sb, basket -> proxy.authorization);
    sbAppendStr(sb, "\r\n");
}

// ─── request writing ───

static int sessionWriteAll(Session *session, int nonBlocking, int timeoutInMillis,
                           const unsigned char *buf, size_t len) {
    if (session -> ssl != NULL) {
        return sslWriteAllEx(session -> ssl, nonBlocking, timeoutInMillis, buf, len);
    }
    return plainWriteAll(session, nonBlocking, timeoutInMillis, buf, len);
}

static int plainWriteAll(Session *session, int nonBlocking, int timeoutInMillis,
                         const unsigned char *buf, size_t len) {
    const int fd = session -> sockfd;
    size_t written = 0;
    while (written < len) {
        const ssize_t rc = send(fd, (const char *) buf + written, len - written, 0);
        if (rc > 0) {
            written += (size_t) rc;
            continue;
        }
        const int lastError = SOCKET_LAST_ERROR;
        const int retryable = isRetryableSocketError(lastError);
        if (!nonBlocking || !retryable) {
            LOG("ERROR", "http/1.1: send() failed: %s (errno: %d)", strerror(lastError), lastError);
            return -1;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = timeoutInMillis / 1000;
        tv.tv_usec = (timeoutInMillis % 1000) * 1000;
        const int selectRc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (selectRc <= 0) {
            LOG("ERROR", "http/1.1: send() timed out after %d ms", timeoutInMillis);
            return -1;
        }
    }
    return 1;
}

static int isRetryableSocketError(int lastError) {
#ifndef _WIN32
    return lastError == EINTR || lastError == EAGAIN || lastError == EWOULDBLOCK;
#else
    return lastError == WSAEINTR || lastError == WSAEWOULDBLOCK;
#endif
}
