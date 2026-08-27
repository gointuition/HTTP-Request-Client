//
// Created by Intuition on 26-8-23.
//
// HTTP/1.1 response side: read and parse status line, headers and body
// (RFC 9110 framing: chunked > content-length > until-close) into the stream,
// in the same shape the HTTP/2 reader produces (":status" first).
//

#include "Http11ResponseHandler.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include "Compat.h"

#include "Error.h"
#include "Log.h"

#define HTTP11_MAX_HEADER_SECTION (64 * 1024)
#define HTTP11_READ_CHUNK 16384

typedef struct {
    Session *session;
    unsigned char *buf;
    size_t len;
    size_t pos;
    const struct timespec *deadline;
    int eof;
    int err; // 0 none, -1 error, -2 timeout
} Http11Reader;

static int readResponseHead(Stream *stream, Http11Reader *reader);
static int parseStatusLine(Stream *stream, const char *line);
static int isHttp11StatusLine(const char *line);
static size_t copyStatusCode(const char *digits, char *statusCode);
static int readHeaderSection(Stream *stream, Http11Reader *reader);
static int parseHeaderLine(Stream *stream, char *line);
static Error classifyHeadError(Stream *stream, const Http11Reader *reader, int statusLineRc);
static Error classifyTimeoutError(Stream *stream);

static int readBody(Basket *basket, Stream *stream, Http11Reader *reader,
                    int statusCode, int *untilClose);
static int classifyBodyError(Stream *stream, int rc);
static int isBodylessStatus(int statusCode);
static int readChunkedBody(Stream *stream, Http11Reader *reader);
static int isTrailerEnd(const char *line);
static int readContentLengthBody(Stream *stream, Http11Reader *reader, long long bodyLen);
static int readUntilCloseBody(Stream *stream, Http11Reader *reader);
static void closeSessionIfSingleUse(Session *session, Stream *stream, int untilClose);

static void readerFill(Http11Reader *reader);
static int readerEndCode(const Http11Reader *reader);
static int readerReadLine(Http11Reader *reader, char **line);
static int readerReadExact(Http11Reader *reader, unsigned char **out, size_t n);
static ssize_t sessionReadTimed(Session *session, unsigned char *buf, size_t len,
                                const struct timespec *deadline);

static int appendStreamPayload(Stream *stream, const unsigned char *data, size_t n);
static void addStreamHeader(Stream *stream, const char *name, const char *value);
static const char* streamHeaderValue(Stream *stream, const char *name);

static long deadlineRemainingMs(const struct timespec *deadline);
static const char* containsCI(const char *haystack, const char *needle);
static char* trim(char *s);
static int isSpaceChar(unsigned char c);
static int isRetryableSocketError(int lastError);
static int hasPartialResponse(Stream *stream);
static int connectionClosedByPeer(Stream *stream);

// ─── response reading ───

void receiveHttp11Response(Basket *basket, Stream *stream, const struct timespec *deadline) {
    Session *session = basket -> session;
    Http11Reader reader = { session, NULL, 0, 0, deadline, 0, 0 };

    const int status = readResponseHead(stream, &reader);
    if (stream -> error.code != NULL) {
        session -> goingAway = 1;
        free(reader.buf);
        return;
    }

    int untilClose = 0;
    readBody(basket, stream, &reader, status, &untilClose);
    free(reader.buf);

    closeSessionIfSingleUse(session, stream, untilClose);
}

// Reads status line + headers; on failure sets stream->error.
static int readResponseHead(Stream *stream, Http11Reader *reader) {
    char *statusLine = NULL;
    const int statusLineRc = readerReadLine(reader, &statusLine);
    int status = -1;
    int parseOk = statusLineRc == 1;
    if (parseOk) {
        status = parseStatusLine(stream, statusLine);
        free(statusLine);
        parseOk = status >= 0;
    }

    if (parseOk) {
        const int headerRc = readHeaderSection(stream, reader);
        parseOk = headerRc == 1;
    }

    if (!parseOk) {
        stream -> error = classifyHeadError(stream, reader, statusLineRc);
        return -1;
    }
    return status;
}

static void closeSessionIfSingleUse(Session *session, Stream *stream, int untilClose) {
    if (untilClose || connectionClosedByPeer(stream)) {
        session -> goingAway = 1;
    }
}

// ─── response parsing ───

static int parseStatusLine(Stream *stream, const char *line) {
    const int validPrefix = isHttp11StatusLine(line);
    if (!validPrefix) {
        LOG("ERROR", "http/1.1: malformed status line: %s", line);
        return -1;
    }
    const char *digits = line + 7;
    while (*digits != '\0' && !isSpaceChar((unsigned char) *digits)) { digits++; } // skip minor version
    while (isSpaceChar((unsigned char) *digits)) { digits++; }

    char statusCode[4] = {0};
    const size_t digitCount = copyStatusCode(digits, statusCode);
    if (digitCount != 3) {
        LOG("ERROR", "http/1.1: malformed status code in: %s", line);
        return -1;
    }
    addStreamHeader(stream, ":status", statusCode);
    return atoi(statusCode);
}

static int isHttp11StatusLine(const char *line) {
    return strncmp(line, "HTTP/1.", 7) == 0;
}

// Copies up to 3 consecutive digits; returns the count copied.
static size_t copyStatusCode(const char *digits, char *statusCode) {
    size_t i = 0;
    while (i < 3 && isdigit((unsigned char) digits[i])) {
        statusCode[i] = digits[i];
        i++;
    }
    return i;
}

// Reads header lines until the empty line; returns the raw reader rc
// (1 ok, 0 EOF, -1 error, -2 timeout).
static int readHeaderSection(Stream *stream, Http11Reader *reader) {
    for (;;) {
        char *line = NULL;
        const int lineRc = readerReadLine(reader, &line);
        if (lineRc != 1) {
            return lineRc;
        }
        if (line[0] == '\0') {
            free(line);
            return 1;
        }
        parseHeaderLine(stream, line);
        free(line);
    }
}

static int parseHeaderLine(Stream *stream, char *line) {
    char *colon = strchr(line, ':');
    if (colon == NULL) {
        LOG("WARN", "http/1.1: skipping malformed header line: %s", line);
        return -1;
    }
    *colon = '\0';
    char *name = trim(line);
    char *value = trim(colon + 1);
    addStreamHeader(stream, name, value);
    return 1;
}

// Body framing per RFC 9110: chunked > content-length > until-close.
// On failure sets stream->error (keeping errors set earlier, e.g. allocation
// failure) and returns -1; returns 0 on legal EOF (until-close), 1 on success.
static int readBody(Basket *basket, Stream *stream, Http11Reader *reader,
                    int statusCode, int *untilClose) {
    *untilClose = 0;

    const int noBody = strcasecmp(basket -> method, "HEAD") == 0 || isBodylessStatus(statusCode);
    if (noBody) {
        return 1;
    }

    const char *transferEncoding = streamHeaderValue(stream, "transfer-encoding");
    const int chunked = transferEncoding != NULL && containsCI(transferEncoding, "chunked") != NULL;
    if (chunked) {
        return classifyBodyError(stream, readChunkedBody(stream, reader));
    }

    const char *contentLength = streamHeaderValue(stream, "content-length");
    if (contentLength != NULL) {
        const long long bodyLen = strtoll(contentLength, NULL, 10);
        const int invalidLength = bodyLen < 0;
        if (invalidLength) {
            LOG("ERROR", "http/1.1: invalid content-length: %s", contentLength);
            return classifyBodyError(stream, -1);
        }
        return classifyBodyError(stream, readContentLengthBody(stream, reader, bodyLen));
    }

    *untilClose = 1;
    return classifyBodyError(stream, readUntilCloseBody(stream, reader));
}

// 0 = legal EOF in until-close framing; errors are reported only when the
// stream does not carry one already (e.g. allocation failure).
static int classifyBodyError(Stream *stream, int rc) {
    const int bodyOk = rc == 1 || rc == 0;
    if (bodyOk) {
        return rc;
    }
    if (stream -> error.code == NULL) {
        stream -> error = (rc == -2) ? classifyTimeoutError(stream)
                                     : ERR_RESPONSE_READING_HTTP11_FAILED;
    }
    return -1;
}

static int isBodylessStatus(int statusCode) {
    return statusCode == 204 || statusCode == 304
           || (statusCode >= 100 && statusCode < 200);
}

static int readChunkedBody(Stream *stream, Http11Reader *reader) {
    for (;;) {
        char *sizeLine = NULL;
        const int lineRc = readerReadLine(reader, &sizeLine);
        if (lineRc != 1) {
            return lineRc;
        }
        char *semi = strchr(sizeLine, ';'); // chunk extensions are ignored
        if (semi != NULL) { *semi = '\0'; }
        char *endPtr = NULL;
        const long chunkSize = strtol(trim(sizeLine), &endPtr, 16);
        free(sizeLine);
        const int malformed = chunkSize < 0 || endPtr == NULL;
        if (malformed) {
            LOG("ERROR", "http/1.1: malformed chunk size line");
            return -1;
        }
        if (chunkSize == 0) {
            // last-chunk: consume optional trailers until the empty line
            for (;;) {
                char *trailer = NULL;
                const int trailerRc = readerReadLine(reader, &trailer);
                if (trailerRc != 1) {
                    return trailerRc;
                }
                const int trailerEnd = isTrailerEnd(trailer);
                free(trailer);
                if (trailerEnd) { break; }
            }
            return 1;
        }

        unsigned char *chunk = NULL;
        const int dataRc = readerReadExact(reader, &chunk, (size_t) chunkSize);
        if (dataRc != 1) {
            return dataRc;
        }
        const int appendRc = appendStreamPayload(stream, chunk, (size_t) chunkSize);
        free(chunk);
        if (appendRc != 1) {
            return -1;
        }

        char *crlf = NULL;
        const int crlfRc = readerReadLine(reader, &crlf);
        if (crlfRc != 1) {
            return crlfRc;
        }
        free(crlf);
    }
}

static int isTrailerEnd(const char *line) {
    return line[0] == '\0';
}

static int readContentLengthBody(Stream *stream, Http11Reader *reader, long long bodyLen) {
    if (bodyLen == 0) { return 1; }
    unsigned char *body = NULL;
    const int readRc = readerReadExact(reader, &body, (size_t) bodyLen);
    if (readRc != 1) {
        return readRc;
    }
    const int appendRc = appendStreamPayload(stream, body, (size_t) bodyLen);
    free(body);
    return appendRc == 1 ? 1 : -1;
}

static int readUntilCloseBody(Stream *stream, Http11Reader *reader) {
    unsigned char chunk[HTTP11_READ_CHUNK];
    for (;;) {
        const ssize_t readRc = sessionReadTimed(reader -> session, chunk, sizeof(chunk),
                                                reader -> deadline);
        if (readRc > 0) {
            const int appendRc = appendStreamPayload(stream, chunk, (size_t) readRc);
            if (appendRc != 1) { return -1; }
            continue;
        }
        if (readRc == 0) { return 0; } // EOF is the legal terminator here
        return (int) readRc;
    }
}

// ─── error classification ───

static Error classifyTimeoutError(Stream *stream) {
    const int received = hasPartialResponse(stream);
    return received ? ERR_RESPONSE_PARTIAL_CONTENT_AFTER_READING_TIMEOUT
                    : ERR_RESPONSE_NO_CONTENT_AFTER_READING_TIMEOUT;
}

static Error classifyHeadError(Stream *stream, const Http11Reader *reader, int statusLineRc) {
    if (reader -> err == -2) {
        return classifyTimeoutError(stream);
    }
    const int cleanEofOnStatusLine = statusLineRc == 0 && stream -> numHeaders == 0;
    const int connectionLost = reader -> err == -1 || cleanEofOnStatusLine;
    return connectionLost ? ERR_RESPONSE_READING_CONNECTION_ERROR
                          : ERR_RESPONSE_READING_HTTP11_FAILED;
}

// ─── buffered transport reader ───

static void readerFill(Http11Reader *reader) {
    if (reader -> eof || reader -> err != 0) { return; }

    // compact so a long-lived body read does not grow without bound
    if (reader -> pos > 0) {
        memmove(reader -> buf, reader -> buf + reader -> pos, reader -> len - reader -> pos);
        reader -> len -= reader -> pos;
        reader -> pos = 0;
    }

    const size_t newCap = reader -> len + HTTP11_READ_CHUNK + 1;
    unsigned char *newBuf = realloc(reader -> buf, newCap);
    if (newBuf == NULL) {
        reader -> err = -1;
        return;
    }
    reader -> buf = newBuf;

    const ssize_t readRc = sessionReadTimed(reader -> session, reader -> buf + reader -> len,
                                            HTTP11_READ_CHUNK, reader -> deadline);
    if (readRc > 0) {
        reader -> len += (size_t) readRc;
    } else if (readRc == 0) {
        reader -> eof = 1;
    } else {
        reader -> err = (int) readRc;
    }
}

// Terminal rc at the current buffer state: the error code, or 0 for EOF.
static int readerEndCode(const Http11Reader *reader) {
    return reader -> err != 0 ? reader -> err : 0;
}

// Returns 1 with *line set, 0 on EOF, -1 on error, -2 on timeout.
static int readerReadLine(Http11Reader *reader, char **line) {
    *line = NULL;
    for (;;) {
        unsigned char *lf = memchr(reader -> buf + reader -> pos, '\n', reader -> len - reader -> pos);
        if (lf != NULL) {
            const size_t end = (size_t) (lf - reader -> buf);
            const size_t start = reader -> pos;
            size_t lineLen = end - start;
            const int hasCr = lineLen > 0 && reader -> buf[start + lineLen - 1] == '\r';
            if (hasCr) { lineLen--; }

            char *out = malloc(lineLen + 1);
            if (out == NULL) { return -1; }
            memcpy(out, reader -> buf + start, lineLen);
            out[lineLen] = '\0';
            reader -> pos = end + 1;
            *line = out;
            return 1;
        }
        if (reader -> eof || reader -> err != 0) {
            return readerEndCode(reader);
        }
        if (reader -> len >= HTTP11_MAX_HEADER_SECTION) {
            LOG("ERROR", "http/1.1: header section exceeds %d bytes", HTTP11_MAX_HEADER_SECTION);
            return -1;
        }
        readerFill(reader);
    }
}

// Returns 1 on success, 0 on premature EOF, -1 on error, -2 on timeout.
static int readerReadExact(Http11Reader *reader, unsigned char **out, size_t n) {
    unsigned char *data = malloc(n > 0 ? n : 1);
    if (data == NULL) { return -1; }

    size_t got = 0;
    while (got < n) {
        const size_t avail = reader -> len - reader -> pos;
        if (avail > 0) {
            const size_t take = (n - got) < avail ? (n - got) : avail;
            memcpy(data + got, reader -> buf + reader -> pos, take);
            reader -> pos += take;
            got += take;
            continue;
        }
        if (reader -> eof || reader -> err != 0) {
            free(data);
            return readerEndCode(reader);
        }
        readerFill(reader);
    }
    *out = data;
    return 1;
}

// Reads up to len bytes: >0 bytes read, 0 peer closed, -1 error, -2 timeout.
// SSL_pending() bypasses select(): decrypted data buffered inside the SSL
// object is invisible at the socket layer.
static ssize_t sessionReadTimed(Session *session, unsigned char *buf, size_t len,
                                const struct timespec *deadline) {
    const int fd = (session -> ssl != NULL) ? SSL_get_fd(session -> ssl) : session -> sockfd;
    int wantWrite = 0;

    for (;;) {
        const int needSelect = session -> ssl == NULL || SSL_pending(session -> ssl) == 0;
        if (needSelect) {
            const long ms = deadlineRemainingMs(deadline);
            if (ms <= 0) { return -2; }

            fd_set rfds, wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(fd, &rfds);
            if (wantWrite) { FD_SET(fd, &wfds); }

            struct timeval tv;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;

            const int selectRc = select(fd + 1, &rfds, wantWrite ? &wfds : NULL, NULL, &tv);
            if (selectRc == 0) { return -2; }
            if (selectRc < 0) {
#ifndef _WIN32
                if (errno == EINTR) { continue; }
#endif
                LOG("ERROR", "http/1.1: select() failed: %s (errno: %d)", strerror(errno), errno);
                return -1;
            }
        }

        if (session -> ssl != NULL) {
            const int readRc = SSL_read(session -> ssl, buf, (int) len);
            if (readRc > 0) { return readRc; }
            const int sslErr = SSL_get_error(session -> ssl, readRc);
            if (sslErr == SSL_ERROR_ZERO_RETURN) { return 0; }
            if (sslErr == SSL_ERROR_WANT_READ) { wantWrite = 0; continue; }
            if (sslErr == SSL_ERROR_WANT_WRITE) { wantWrite = 1; continue; }
            LOG("ERROR", "http/1.1: SSL_read failed (err=%d)", sslErr);
            return -1;
        }

        const ssize_t readRc = recv(fd, (char *) buf, len, 0);
        if (readRc > 0) { return readRc; }
        if (readRc == 0) { return 0; }
        const int lastError = SOCKET_LAST_ERROR;
        const int retryable = isRetryableSocketError(lastError);
        if (retryable) { continue; }
        LOG("ERROR", "http/1.1: recv() failed: %s (errno: %d)", strerror(lastError), lastError);
        return -1;
    }
}

// ─── stream helpers ───

static int appendStreamPayload(Stream *stream, const unsigned char *data, size_t n) {
    if (n == 0) { return 1; }
    unsigned char *newPayload = realloc(stream -> combinedPayload, stream -> combinedPayloadSize + n);
    if (newPayload == NULL) {
        stream -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return -1;
    }
    stream -> combinedPayload = newPayload;
    memcpy(stream -> combinedPayload + stream -> combinedPayloadSize, data, n);
    stream -> combinedPayloadSize += n;
    return 1;
}

static void addStreamHeader(Stream *stream, const char *name, const char *value) {
    if (stream -> numHeaders >= RESPONSE_HEADERS_MAX_SIZE) {
        LOG("WARN", "http/1.1: response headers limit reached (%d), discarding %s",
            RESPONSE_HEADERS_MAX_SIZE, name);
        return;
    }
    stream -> headers[stream -> numHeaders].name = strdup(name);
    stream -> headers[stream -> numHeaders].value = strdup(value);
    stream -> headers[stream -> numHeaders].freeName = 1;
    stream -> headers[stream -> numHeaders].freeValue = 1;
    stream -> numHeaders++;
}

static const char* streamHeaderValue(Stream *stream, const char *name) {
    for (size_t i = 0; i < stream -> numHeaders; i++) {
        const int match = strcasecmp(stream -> headers[i].name, name) == 0;
        if (match) {
            return stream -> headers[i].value;
        }
    }
    return NULL;
}
// ─── generic helpers ───

static long deadlineRemainingMs(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (long) (deadline -> tv_sec - now.tv_sec) * 1000
         + (long) (deadline -> tv_nsec - now.tv_nsec) / 1000000;
}

// strcasestr is not portable to Windows.
static const char* containsCI(const char *haystack, const char *needle) {
    const size_t needleLen = strlen(needle);
    if (needleLen == 0) { return haystack; }
    const size_t haystackLen = strlen(haystack);
    if (haystackLen < needleLen) { return NULL; }
    for (size_t i = 0; i + needleLen <= haystackLen; i++) {
        const int match = strncasecmp(haystack + i, needle, needleLen) == 0;
        if (match) { return haystack + i; }
    }
    return NULL;
}

static char* trim(char *s) {
    while (isSpaceChar((unsigned char) *s)) { s++; }
    size_t len = strlen(s);
    while (len > 0 && isSpaceChar((unsigned char) s[len - 1])) { s[--len] = '\0'; }
    return s;
}

static int isSpaceChar(unsigned char c) {
    return c != '\0' && isspace(c);
}

static int isRetryableSocketError(int lastError) {
#ifndef _WIN32
    return lastError == EINTR || lastError == EAGAIN || lastError == EWOULDBLOCK;
#else
    return lastError == WSAEINTR || lastError == WSAEWOULDBLOCK;
#endif
}

static int hasPartialResponse(Stream *stream) {
    return stream -> numHeaders > 0 || stream -> combinedPayloadSize > 0;
}

static int connectionClosedByPeer(Stream *stream) {
    const char *connection = streamHeaderValue(stream, "connection");
    return connection != NULL && containsCI(connection, "close") != NULL;
}
