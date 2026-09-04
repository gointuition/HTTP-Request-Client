//
// Created by Intuition on 26-4-7.
//

#include "Http2RequestHandler.h"

#include <string.h>
#include <zlib.h>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include "Compat.h"

#include "Common.h"
#include "SSLHandler.h"
#include "Error.h"
#include "BrowserHandler.h"
#include "CompressHandler.h"
#include "Log.h"

// HTTP/2 connection preface
static const char HTTP2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// Write len bytes to ssl, driving the socket with select() and retrying on
// SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE when the session runs in
// non-blocking mode. On a blocking socket this degenerates to a single
// SSL_write. Returns 1 on success (all bytes written), -1 on failure (basket
// error is set by the caller). `timeoutInMillis` bounds the total time spent
// waiting for writability (connect timeout is reused for the transfer phase).
int sslWriteAllEx(SSL *ssl, int nonBlocking, int timeoutInMillis, const unsigned char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        const int rc = SSL_write(ssl, buf + written, (int) (len - written));
        if (rc > 0) {
            written += (size_t) rc;
            continue;
        }

        const int err = SSL_get_error(ssl, rc);
        if (!nonBlocking || (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)) {
            return -1; // blocking path error, or a real (non-WANT) error
        }

        // Non-blocking: wait for readability (WANT_READ) or writability
        // (WANT_WRITE) before retrying.
        const int fd = SSL_get_fd(ssl);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv;
        const int ms = timeoutInMillis;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;

        int sr;
        if (err == SSL_ERROR_WANT_READ) {
            sr = select(fd + 1, &fds, NULL, NULL, &tv);
        } else {
            sr = select(fd + 1, NULL, &fds, NULL, &tv);
        }
        if (sr <= 0) {
            return -1; // timeout or select() error
        }
    }
    return 1;
}

// Basket-aware convenience wrapper for the request send path.
static int sslWriteAll(Basket *basket, SSL *ssl, const unsigned char *buf, size_t len) {
    return sslWriteAllEx(ssl, basket -> nonBlocking, basket -> connectTimeoutInMilliseconds, buf, len);
}

typedef struct {
    const char *name;
    int index;
} RequestHeaderHpackStaticEntry;

// HPACK Static Table (Partial: Indices 15-61 relevant to your code)
// must be sorted by name in alphabetical order to support bsearch
static const RequestHeaderHpackStaticEntry requestHeaderHpackStaticTable[] = {
    {"accept", 19},
    {"accept-charset", 15},
    {"accept-encoding", 16},
    {"accept-language", 17},
    {"accept-ranges", 18},
    {"access-control-allow-origin", 20},
    {"age", 21},
    {"allow", 22},
    {"authorization", 23},
    {"cache-control", 24},
    {"content-disposition", 25},
    {"content-encoding", 26},
    {"content-language", 27},
    {"content-length", 28},
    {"content-location", 29},
    {"content-range", 30},
    {"content-type", 31},
    {"cookie", 32},
    {"date", 33},
    {"etag", 34},
    {"expect", 35},
    {"expires", 36},
    {"from", 37},
    {"host", 38},
    {"if-match", 39},
    {"if-modified-since", 40},
    {"if-none-match", 41},
    {"if-range", 42},
    {"if-unmodified-since", 43},
    {"last-modified", 44},
    {"link", 45},
    {"location", 46},
    {"max-forwards", 47},
    {"proxy-authenticate", 48},
    {"proxy-authorization", 49},
    {"range", 50},
    {"referer", 51},
    {"refresh", 52},
    {"retry-after", 53},
    {"server", 54},
    {"set-cookie", 55},
    {"strict-transport-security", 56},
    {"transfer-encoding", 57},
    {"user-agent", 58},
    {"vary", 59},
    {"via", 60},
    {"www-authenticate", 61}
};

static const size_t hpackStaticTableSize = sizeof(requestHeaderHpackStaticTable) / sizeof(requestHeaderHpackStaticTable[0]);

static size_t calculateHpackBufferSize(RequestHeader *headers, size_t numHeaders);
static unsigned char *hpackPseudoHeaders(Basket *basket, RequestHeader header, unsigned char *hpackBufferPtr);
static unsigned char *hpackHeaders(RequestHeader header, unsigned char *hpackBufferPtr);
static size_t buildHeadersFrameBuffer(unsigned char *buffer, size_t bufferSize, int hasPayload,
                                      const unsigned char *hpackBuffer, size_t hpackPayloadLen, uint32_t streamId,
                                      int includePriority);
static int buildHttp2HeadersFrame(Basket *basket, unsigned char *buffer, size_t bufferSize);

static void logHttp2Request(Basket *basket);
static char* buildHttp2RequestDebug(Basket *basket);
static void appendHttp2DebugRequestLine(StrBuf *sb, Basket *basket);
static void appendHttp2DebugHeaders(StrBuf *sb, Basket *basket);

int establishTransport(Basket *basket, SSL *ssl) {
    // send HTTP/2 Preface frame
    if (sslWriteAll(basket, ssl, (const unsigned char *) HTTP2_PREFACE, strlen(HTTP2_PREFACE)) != 1) {
        LOG("ERROR", "send HTTP/2 Preface frame fa");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_PREFACE_FRAME_FAILED;
        return -1;
    }

    const BrowserFingerprint *fp = basket -> fingerprint;
    if (fp == NULL) {
        // TODO
        LOG("ERROR", "unsupported user-agent");
        basket -> error = ERR_REQUEST_UNSUPPORTED_USERAGENT;
        return -1;
    }

    // send HTTP/2 Settings frame
    if (sslWriteAll(basket, ssl, fp -> settingsFrame, fp -> settingsFrameLen) != 1) {
        LOG("ERROR", "send HTTP/2 settings frame fa");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_SETTINGS_FRAME_FAILED;
        return -1;
    }

    // send HTTP/2 Window Update frame
    if (sslWriteAll(basket, ssl, fp -> windowUpdateFrame, fp -> windowUpdateFrameLen) != 1) {
        LOG("ERROR", "send HTTP/2 window update frame");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_WINDOW_UPDATE_FRAME_FAILED;
        return -1;
    }

    return 1;
}

void sendHeadersFrame(Basket *basket) {
    // TODO optimize headersFrame size
    unsigned char headersFrame[32768];
    const int headersFrameLen = buildHttp2HeadersFrame(basket, headersFrame, sizeof(headersFrame));
    if (basket -> error.code != NULL) {
        return;
    }

    logHttp2Request(basket);

    // logHex("Sending HEADERS frame:", headersFrame, headersFrameLen);
    if (sslWriteAll(basket, basket -> session -> ssl, headersFrame, headersFrameLen) != 1) {
        LOG("ERROR", "SSL_write http headers frame failed");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_HEADERS_FRAME_FAILED;
        return;
    }
}

// ─── request debug logging ───

// Same format as the http/1.1 request log: request line plus each
// HEADERS-frame header in wire order.
static void logHttp2Request(Basket *basket) {
    if (!getLogEnabled()) { return; }
    char *request = buildHttp2RequestDebug(basket);
    if (request == NULL) { return; } // debug aid only: never fail the request on log allocation
    LOG("DEBUG", "http/2 request:\n%s", request);
    free(request);
}

static char* buildHttp2RequestDebug(Basket *basket) {
    StrBuf sb = { NULL, 0, 0, 0 };
    appendHttp2DebugRequestLine(&sb, basket);
    appendHttp2DebugHeaders(&sb, basket);
    if (sb.failed) {
        free(sb.data);
        return NULL;
    }
    sb.data[sb.len] = '\0';
    return sb.data;
}

// "GET /path HTTP/2", values taken from the :method / :path pseudo headers
static void appendHttp2DebugRequestLine(StrBuf *sb, Basket *basket) {
    const char *method = "";
    const char *path = "";
    for (size_t i = 0; i < basket -> request.numHeaders; i++) {
        const RequestHeader *h = &basket -> request.headers[i];
        if (!h -> isPseudo) { continue; }
        if (strcasecmp(h -> name, ":method") == 0) { method = h -> value; }
        if (strcasecmp(h -> name, ":path") == 0) { path = h -> value; }
    }

    char requestLine[4608];
    const int lineLen = snprintf(requestLine, sizeof(requestLine), "%s %s HTTP/2\n", method, path);
    sbAppend(sb, requestLine, (size_t) lineLen);
}

// Every header in wire order, pseudo headers included; "connection" is
// dropped by the HPACK encoder.
static void appendHttp2DebugHeaders(StrBuf *sb, Basket *basket) {
    for (size_t i = 0; i < basket -> request.numHeaders; i++) {
        const RequestHeader *h = &basket -> request.headers[i];
        if (!h -> isPseudo && strcasecmp(h -> name, "connection") == 0) { continue; }

        sbAppendStr(sb, h -> name);
        sbAppendStr(sb, ": ");
        sbAppendStr(sb, h -> value);
        sbAppendStr(sb, "\n");
    }
}

void sendDataFrame(Basket *basket) {
    const size_t payloadLen = strlen(basket -> request.payload);
    if (payloadLen > 0) {
        const size_t dataFrameLen = 9 + payloadLen;
        unsigned char *dataFrameBuffer = malloc(dataFrameLen);
        if (!dataFrameBuffer) {
            LOG("ERROR", "Data frame buffer memory allocation failed");
            basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            return;
        }

        // 0~2 24 bits, Length Field, no reserved bit
        // 0XFF: preserves all 8 bits
        dataFrameBuffer[0] = (payloadLen >> 16) & 0xFF; // high 8 bits
        dataFrameBuffer[1] = (payloadLen >> 8) & 0xFF; // middle 8 bits
        dataFrameBuffer[2] = payloadLen & 0xFF; // low 8 bits
        dataFrameBuffer[3] = 0x00; // DATA Frame
        dataFrameBuffer[4] = 0x01; // END_STREAM
        // 5~8 31 bits, Stream ID Field, highest bit (bit 32) is reserved
        dataFrameBuffer[5] = (basket -> streamId >> 24) & 0x7F;
        // 0x7F, only preserves lower 7 bits, forces highest bit to 0 (Protocol Extensibility, Error Detection, Specification Compiliance)
        dataFrameBuffer[6] = (basket -> streamId >> 16) & 0xFF;
        dataFrameBuffer[7] = (basket -> streamId >> 8) & 0xFF;
        dataFrameBuffer[8] = basket -> streamId & 0xFF;

        memcpy(dataFrameBuffer + 9, basket -> request.payload, payloadLen);

        if (sslWriteAll(basket, basket -> session -> ssl, dataFrameBuffer, dataFrameLen) != 1) {
            free(dataFrameBuffer);
            LOG("ERROR", "SSL_write http data frame failed");
            basket -> error = ERR_REQUEST_SENDING_HTTP2_DATA_FRAME_FAILED;
            return;
        }
        free(dataFrameBuffer);
    }
}

/**
+-----------------------------------------------+
|                 Length (24)                   |
+---------------+---------------+---------------+
|   Type (8)    |   Flags (8)   |
+-+-------------+---------------+---------------+
|R|                 Stream Identifier (31)      |
+=+=============================================+
|                   Frame Payload (*)           |
+-----------------------------------------------+
*/
void sendControlFrame(Session *session, const unsigned char *frame, size_t len) {
    pthread_mutex_lock(&session -> writeMutex);
    const int written = sslWriteAllEx(session -> ssl, session -> nonBlocking, 5000, frame, len);
    pthread_mutex_unlock(&session -> writeMutex);

    if (written != 1) {
        LOG("WARN", "SSL_write HTTP/2 control frame 0x%02x failed", frame[3]);
    }
}

/*
WINDOW_UPDATE (type 0x8), payload is a 31-bit unsigned Window Size Increment
+-----------------------------------------------+
|                   Window Size Increment (31)  |
+-----------------------------------------------+
*/
static void fillWindowUpdateFrame(unsigned char *frame, uint32_t streamId, uint32_t increment) {
    const unsigned char header[5] = {0, 0, 4, 0x8, 0};
    memcpy(frame, header, sizeof(header));
    frame[5] = (streamId >> 24) & 0x7F;
    frame[6] = (streamId >> 16) & 0xFF;
    frame[7] = (streamId >> 8) & 0xFF;
    frame[8] = streamId & 0xFF;
    frame[9] = (increment >> 24) & 0x7F;
    frame[10] = (increment >> 16) & 0xFF;
    frame[11] = (increment >> 8) & 0xFF;
    frame[12] = increment & 0xFF;
}

void sendWindowUpdateFrame(Session *session, uint32_t streamId, uint32_t increment) {
    unsigned char frame[13];
    fillWindowUpdateFrame(frame, streamId, increment);
    sendControlFrame(session, frame, sizeof(frame));
}

/*
RST_STREAM (type 0x3), payload is a 32-bit error code (0x8 = CANCEL: the
client is no longer interested in the stream)
+-----------------------------------------------+
|                        Error Code (32)        |
+-----------------------------------------------+
*/
static void fillRSTStreamFrame(unsigned char *frame, uint32_t streamId) {
    const unsigned char header[9] = {0, 0, 4, 0x3, 0, 0, 0, 0, 0};
    memcpy(frame, header, sizeof(header));
    frame[5] = (streamId >> 24) & 0x7F;
    frame[6] = (streamId >> 16) & 0xFF;
    frame[7] = (streamId >> 8) & 0xFF;
    frame[8] = streamId & 0xFF;
    frame[9] = 0;
    frame[10] = 0;
    frame[11] = 0;
    frame[12] = 0x8;
}

void sendRSTStreamFrame(Session *session, uint32_t streamId) {
    unsigned char frame[13];
    fillRSTStreamFrame(frame, streamId);
    sendControlFrame(session, frame, sizeof(frame));
}

// ─── reader-thread write queue (coalesced batch-end writes) ───

// Appends one frame's raw wire bytes to the queue. On allocation failure the
// frame falls back to an immediate write so flow control never stalls.
void queueControlFrame(Session *session, const unsigned char *frame, size_t len) {
    if (session -> pendingWireSize + len > session -> pendingWireCapacity) {
        size_t capacity = session -> pendingWireCapacity ? session -> pendingWireCapacity : 256;
        while (capacity < session -> pendingWireSize + len) { capacity *= 2; }
        unsigned char *grown = realloc(session -> pendingWire, capacity);
        if (grown == NULL) {
            LOG("ERROR", "control frame queue allocation failed; writing immediately");
            sendControlFrame(session, frame, len);
            return;
        }
        session -> pendingWire = grown;
        session -> pendingWireCapacity = capacity;
    }
    memcpy(session -> pendingWire + session -> pendingWireSize, frame, len);
    session -> pendingWireSize += len;
}

void queueWindowUpdateFrame(Session *session, uint32_t streamId, uint32_t increment) {
    unsigned char frame[13];
    fillWindowUpdateFrame(frame, streamId, increment);
    queueControlFrame(session, frame, sizeof(frame));
}

void queueRSTStreamFrame(Session *session, uint32_t streamId) {
    unsigned char frame[13];
    fillRSTStreamFrame(frame, streamId);
    queueControlFrame(session, frame, sizeof(frame));
}

// Runs once per read batch on the reader thread: owed connection credit joins
// the queue first, then the whole queue leaves through a single serialized
// write (one burst instead of one write per frame).
void flushReaderWrites(Session *session) {
    if (session -> connWindowFlushDue) {
        session -> connWindowFlushDue = 0;
        if (session -> pendingConnWindow > 0) {
            queueWindowUpdateFrame(session, 0, session -> pendingConnWindow);
            session -> pendingConnWindow = 0;
        }
    }
    if (session -> pendingWireSize == 0) { return; }

    const size_t wireSize = session -> pendingWireSize;
    session -> pendingWireSize = 0;

    pthread_mutex_lock(&session -> writeMutex);
    const int written = sslWriteAllEx(session -> ssl, session -> nonBlocking, 5000, session -> pendingWire, wireSize);
    pthread_mutex_unlock(&session -> writeMutex);

    if (written != 1) {
        LOG("WARN", "SSL_write HTTP/2 queued control frames failed");
    }
}

/**
 * Streams identifiers
 * stream ids are 31-bit integers
 * streams initiated by the client use odd-numbered ids
 * once a stream is closed (by sending a frame with the END-STREAMS flag or receiving one), the stream id is considered closed
 * @param jsonRequest
 * @param urlComponents
 * @param buffer
 * @param bufferSize
 * @param streamId
 * @return
 */
static int buildHttp2HeadersFrame(Basket *basket, unsigned char *buffer, size_t bufferSize) {
    // HPACK encoding buffer
    unsigned char hpackBuffer[calculateHpackBufferSize(basket -> request.headers, basket -> request.numHeaders)];
    unsigned char *hpackBufferPtr = hpackBuffer;

    for (size_t i = 0; i < basket -> request.numHeaders; i++) {
        if (!basket -> request.headers[i].isPseudo &&
            strcasecmp(basket -> request.headers[i].name, "connection") == 0) {
            continue;
        }
        if (basket -> request.headers[i].isPseudo) {
            hpackBufferPtr = hpackPseudoHeaders(basket, basket -> request.headers[i], hpackBufferPtr);
        } else {
            hpackBufferPtr = hpackHeaders(basket -> request.headers[i], hpackBufferPtr);
        }
        if (basket -> error.code != NULL) {
            return -1;
        }
    }

    const size_t hpackPayloadLen = hpackBufferPtr - hpackBuffer;

    // TODO other browsers
    const int hasPayload = (basket -> request.payload != NULL) ? 1 : 0;
    const BrowserFingerprint *fp = basket -> fingerprint;
    const int includePriority = (fp != NULL) ? fp -> enableHeadersPriority : 0;
    size_t totalPayloadLen = buildHeadersFrameBuffer(buffer, bufferSize, hasPayload, hpackBuffer,
                                                     hpackPayloadLen, basket -> streamId, includePriority);

    return totalPayloadLen + 9;;
}

static int compareHpackEntry(const void *key, const void *element) {
    const char *name = (const char *) key;
    const RequestHeaderHpackStaticEntry *entry = (const RequestHeaderHpackStaticEntry *) element;
    return strcasecmp(name, entry -> name);
}

static size_t calculateHpackBufferSize(RequestHeader *headers, size_t numHeaders) {
    size_t totalSize = 0;

    for (size_t i = 0; i < numHeaders; i++) {
        // normally, after Hpack encoding and Huffman encoding, size = original size * 1.2
        size_t nameSize = strlen(headers[i].name);
        size_t valueSize = strlen(headers[i].value);
        size_t estimatedSize = (nameSize + valueSize) * 1.2 + 10; // +10 for overhead

        // for cookies, * 2
        if (strcasecmp(headers[i].name, "cookies") == 0) {
            estimatedSize *= 2;
        }

        totalSize += estimatedSize;
    }

    return totalSize + 512; // safety margin
}

/**
 *  Encoding pseudo-headers
    0x82
    binary 10000010
    1000 means an indexed header field
    0000010 indexed number, excluding the first 1
 * @param basket
 * @param header
 * @param hpackBufferPtr
 */
static unsigned char* hpackPseudoHeaders(Basket *basket, const RequestHeader header, unsigned char *hpackBufferPtr) {
    const char *name = header.name;
    const char *value = header.value;
    size_t valueLen = strlen(value);

    if (strcasecmp(name, ":method") == 0) {
        // HPACK static table only has GET (index 2) and POST (index 3)
        if (strcasecmp(value, "GET") == 0) {
            *hpackBufferPtr++ = 0x82; // Index 2: :method GET
        } else if (strcasecmp(value, "POST") == 0) {
            *hpackBufferPtr++ = 0x83; // Index 3: :method POST
        } else {
            // PUT, PATCH, DELETE, etc. are not in the static table,
            // use literal header field without indexing
            *hpackBufferPtr++ = 0x00;
            size_t nameLen = strlen(name);
            hpackEncodeInteger(nameLen, 7, 0x00, &hpackBufferPtr);
            memcpy(hpackBufferPtr, name, nameLen);
            hpackBufferPtr += nameLen;

            writeHuffmanValue(&hpackBufferPtr, value, valueLen);
        }
    } else if (strcasecmp(name, ":scheme") == 0) {
        if (strcasecmp(name, "https") == 0) {
            *hpackBufferPtr++ = 0x87; // index 7
        } else if (strcasecmp(name, "http") == 0) {
            // TODO test
            *hpackBufferPtr++ = 0x86; // index 6
        } else {
            // TODO test
            // fallback: literal header without indexing
            *hpackBufferPtr++ = 0x00;
            size_t nameLen = strlen(name);
            hpackEncodeInteger(nameLen, 7, 0x00, &hpackBufferPtr);
            memcpy(hpackBufferPtr, name, nameLen);
            hpackBufferPtr += nameLen;
            writeHuffmanValue(&hpackBufferPtr, value, valueLen);
        }
    } else if (strcasecmp(name, ":authority") == 0 || strcasecmp(name, ":path") == 0) {
        // literal header field with indexing - indexed name
        int staticIndex = -1;
        if (strcasecmp(name, ":authority") == 0) {
            staticIndex = 0x41;
        } else if (strcasecmp(name, ":path") == 0) {
            staticIndex = 0x44;
        }
        *hpackBufferPtr++ = staticIndex;
        writeHuffmanValue(&hpackBufferPtr, value, valueLen);
    } else {
        LOG("ERROR", "unknown pseudo header name: %s", header.name);
        basket -> error = ERR_REQUEST_UNKNOWN_PSEUDO_HEADER;
    }
    // } else if (strcasecmp(header.name, ":authority") == 0) {
    //     // literal header field with indexing - indexed name (:authority, static table index 1)
    //     *hpackBufferPtr++ = 0x41;
    //     size_t authorityLen = strlen(header.value);
    //     unsigned char huffAuthority[64];
    //     const size_t huffAuthorityLen = hpackHuffmanEncode(header.value, authorityLen, huffAuthority);
    //     // 0x80 is Huffman encoding flag, the 8th '1' meaning using Huffman encoding
    //     // if length <= 127, use length, or use the placeholder 0x7F 127
    //     *hpackBufferPtr++ = 0x80 | (huffAuthorityLen > 127 ? 0x7F : huffAuthorityLen);
    //     // if length > 127, use extra bytes
    //     if (huffAuthorityLen > 127) *hpackBufferPtr++ = huffAuthorityLen - 127;
    //     memcpy(hpackBufferPtr, huffAuthority, huffAuthorityLen);
    //     hpackBufferPtr += huffAuthorityLen;
    return hpackBufferPtr;
}

/**
 *
 * @param header
 * @param hpackBufferPtr
 */
static unsigned char *hpackHeaders(RequestHeader header, unsigned char *hpackBufferPtr) {
    const RequestHeaderHpackStaticEntry *found = bsearch(header.name, requestHeaderHpackStaticTable, hpackStaticTableSize, sizeof(RequestHeaderHpackStaticEntry), compareHpackEntry);
    int staticIndex = -1;
    if (found != NULL) { staticIndex = found -> index; }

    if (staticIndex != -1) {
        // use static table index for name
        *hpackBufferPtr++ = 0x40 | staticIndex;
    } else {
        // literal header field without indexing
        *hpackBufferPtr++ = 0x00;
        // header name
        const size_t nameLen = strlen(header.name);
        unsigned char huffName[128];
        size_t huffNameLen = hpackHuffmanEncode(header.name, nameLen, huffName);
        if (huffNameLen < 128) {
            *hpackBufferPtr++ = 0x80 | huffNameLen;
        } else {
            // TODO incorrect?
            *hpackBufferPtr++ = 0x80 | 0x7F;
            *hpackBufferPtr++ = huffNameLen - 127;
        }
        memcpy(hpackBufferPtr, huffName, huffNameLen);
        hpackBufferPtr += huffNameLen;
    }

    // header value
    const size_t valueLen = strlen(header.value);
    unsigned char *huffValueBuffer = malloc(valueLen * 2);
    size_t huffValueBufferLen = hpackHuffmanEncode(header.value, valueLen, huffValueBuffer);
    hpackEncodeInteger(huffValueBufferLen, 7, 0x80, &hpackBufferPtr);
    memcpy(hpackBufferPtr, huffValueBuffer, huffValueBufferLen);
    hpackBufferPtr += huffValueBufferLen;
    free(huffValueBuffer);
    return hpackBufferPtr;
}

/**
* +-----------------------------------------------+
|                 Length (24)                   |
+---------------+---------------+---------------+
|   Type (8)    |   Flags (8)   |
+-+-------------+---------------+---------------+
|R|                 Stream Identifier (31)      |  ← Current Stream ID
+=+=============================================+
|Pad Length? (8)|
+-+-------------+-----------------------------------------------+
|E|                 Stream Dependency? (31)                     |  ← Parent Stream ID (if with PRIORITY)
+-+-------------+-----------------------------------------------+
|   Weight? (8) |
+-+-------------+-----------------------------------------------+
|                   Header Block Fragment (*)                   |
+---------------------------------------------------------------+
 * @param buffer
 * @param bufferSize
 * @param method
 * @param hpackBuffer
 * @param hpackPayloadLen
 * @param streamId
 * @return
 */
static size_t buildHeadersFrameBuffer(unsigned char *buffer, const size_t bufferSize, const int hasPayload,
                                      const unsigned char *hpackBuffer, size_t hpackPayloadLen,
                                      const uint32_t streamId, const int includePriority) {
    /**
     * priority information (5 bytes), only present when includePriority is set
    +---+-------------+-----------------------------------------------+
    |E|                 Stream Dependency (31)                       |
    +-+-------------+-----------------------------------------------+
    |   Weight (8)  |
    +-+-------------+
    */
    unsigned char priorityData[5];
    size_t priorityLen = 0;
    if (includePriority) {
        const int exclusive = 1; // Exclusive dependency
        const uint32_t streamDependency = 0; // TODO depends on root stream
        const uint8_t weight = 255; // weight 256 (256 - 1 = 255)
        // stream dependency (31 bits) with exclusive flag
        priorityData[0] = (exclusive << 7) | (streamDependency >> 24);
        priorityData[1] = (streamDependency >> 16) & 0xFF;
        priorityData[2] = (streamDependency >> 8) & 0xFF;
        priorityData[3] = streamDependency & 0xFF;
        priorityData[4] = weight;
        priorityLen = sizeof(priorityData);
    }

    // total payload length = optional priority data + HPACK payload
    size_t totalPayloadLen = priorityLen + hpackPayloadLen;
    if (totalPayloadLen + 9 > bufferSize) {
        return -1;
    }

    buffer[0] = (totalPayloadLen >> 16) & 0xFF;
    buffer[1] = (totalPayloadLen >> 8) & 0xFF;
    buffer[2] = totalPayloadLen & 0xFF;
    buffer[3] = 0x01; // HEADERS frame

    uint8_t flags = 0x04; // END_HEADERS
    if (!hasPayload) {
        flags |= 0x01; // END_STREAM (no DATA frame will follow)
    }
    if (includePriority) {
        flags |= 0x20; // PRIORITY
    }
    buffer[4] = flags;

    buffer[5] = (streamId >> 24) & 0x7F;
    buffer[6] = (streamId >> 16) & 0xFF;
    buffer[7] = (streamId >> 8) & 0xFF;
    buffer[8] = streamId & 0xFF;

    // copy optional priority data
    if (priorityLen > 0) {
        memcpy(buffer + 9, priorityData, priorityLen);
    }
    // copy HPACK payload
    memcpy(buffer + 9 + priorityLen, hpackBuffer, hpackPayloadLen);

    return totalPayloadLen;
}
