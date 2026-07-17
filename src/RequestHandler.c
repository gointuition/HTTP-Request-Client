//
// Created by Intuition on 26-4-7.
//

#include "RequestHandler.h"

#include <string.h>
#include <netdb.h>

#include <arpa/inet.h>

#include <unistd.h>
#include <zlib.h>

#include "SSLHandler.h"
#include "Error.h"
#include "BrowserHandler.h"
#include "CompressHandler.h"
#include "Log.h"

// HTTP/2 connection preface
static const char HTTP2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

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
static size_t buildHeadersFrameBuffer_Chrome(unsigned char *buffer, size_t bufferSize, const char *method,
                                             const unsigned char *hpackBuffer, size_t hpackPayloadLen, uint32_t streamId);
static int buildHttp2HeadersFrame(Basket *basket, unsigned char *buffer, size_t bufferSize);

int establishTransport(Basket *basket, SSL *ssl) {
    // send HTTP/2 Preface frame
    if (SSL_write(ssl, HTTP2_PREFACE, strlen(HTTP2_PREFACE)) <= 0) {
        LOG("ERROR", "send HTTP/2 Preface frame fa");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_PREFACE_FRAME_FAILED;
        return -1;
    }

    if (basket -> browserType == BROWSER_CHROME) {
        // send HTTP/2 Settings frame
        if (SSL_write(ssl, SETTINGS_FRAME_CHROME, sizeof(SETTINGS_FRAME_CHROME)) <= 0) {
            LOG("ERROR", "send HTTP/2 settings frame fa");
            basket -> error = ERR_REQUEST_SENDING_HTTP2_SETTINGS_FRAME_FAILED;
            return -1;
        }

        // sned HTTP/2 Window Update frame
        if (SSL_write(ssl, WINDOW_UPDATE_FRAME_CHROME, sizeof(WINDOW_UPDATE_FRAME_CHROME)) <= 0) {
            LOG("ERROR", "send HTTP/2 window update frame");
            basket -> error = ERR_REQUEST_SENDING_HTTP2_WINDOW_UPDATE_FRAME_FAILED;
            return -1;
        }
    } else {
        // TODO
        LOG("ERROR", "unsupported user-agent");
        basket -> error = ERR_REQUEST_UNSUPPORTED_USERAGENT;
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

    // logHex("Sending HEADERS frame:", headersFrame, headersFrameLen);
    if (SSL_write(basket -> session -> ssl, headersFrame, headersFrameLen) <= 0) {
        LOG("ERROR", "SSL_write http headers frame failed");
        basket -> error = ERR_REQUEST_SENDING_HTTP2_HEADERS_FRAME_FAILED;
        return;
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

        if (SSL_write(basket -> session -> ssl, dataFrameBuffer, dataFrameLen) <= 0) {
            free(dataFrameBuffer);
            LOG("ERROR", "SSL_write http data frame failed");
            basket -> error = ERR_REQUEST_SENDING_HTTP2_DATA_FRAME_FAILED;
            return;
        }
        free(dataFrameBuffer);
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
        LOG("DEBUG", "Request Header: %s: %s", basket -> request.headers[i].name, basket -> request.headers[i].value);
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
    size_t totalPayloadLen = buildHeadersFrameBuffer_Chrome(buffer, bufferSize, basket -> method, hpackBuffer,
                                                            hpackPayloadLen, basket -> streamId);

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
        // static table lookup for method
        if (strcasecmp(value, "GET") == 0) {
            *hpackBufferPtr++ = 0x82; // Index 2
        } else if (strcasecmp(value, "POST") == 0) {
            *hpackBufferPtr++ = 0x83; // Index 3
        } else if (strcasecmp(value, "PUT") == 0) {
            *hpackBufferPtr++ = 0x84; // Index 4
        } else if (strcasecmp(value, "DELETE") == 0) {
            *hpackBufferPtr++ = 0x85; // Index 5
        } else {
            // fallback: literal header without indexing
            // not: pseudo-headers generally shouldn't be indexed dynamically in some implementations
            // but standard allows it. Using 0x00 (no indexing) is safer for pseudo-headers if not in static table
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
static size_t buildHeadersFrameBuffer_Chrome(unsigned char *buffer, const size_t bufferSize, const char *method,
                                      const unsigned char *hpackBuffer, size_t hpackPayloadLen,
                                      const uint32_t streamId) {
    /**
     * priority information (5 bytes)
    +---+-------------+-----------------------------------------------+
    |E|                 Stream Dependency (31)                       |
    +-+-------------+-----------------------------------------------+
    |   Weight (8)  |
    +-+-------------+
    */
    unsigned char priorityData[5];
    const int exclusive = 1; // Exclusive dependency
    const uint32_t streamDependency = 0; // TODO depends on root stream
    const uint8_t weight = 255; // weight 256 (256 - 1 = 255)
    // stream dependency (31 bits) with exclusive flag
    priorityData[0] = (exclusive << 7) | (streamDependency >> 24);
    priorityData[1] = (streamDependency >> 16) & 0xFF;
    priorityData[2] = (streamDependency >> 8) & 0xFF;
    priorityData[3] = streamDependency & 0xFF;
    priorityData[4] = weight;

    // total payload length = priority data (5) + HPACK payload
    size_t totalPayloadLen = 5 + hpackPayloadLen;
    if (totalPayloadLen + 9 > bufferSize) {
        return -1;
    }

    buffer[0] = (totalPayloadLen >> 16) & 0xFF;
    buffer[1] = (totalPayloadLen >> 8) & 0xFF;
    buffer[2] = totalPayloadLen & 0xFF;
    buffer[3] = 0x01; // HEADERS frame
    // TODO other methods
    if (strcasecmp(method, "POST") == 0) {
        buffer[4] = 0x24; // Flags: END_HEADERS | PRIORITY (because a DATA frame will follow)
    } else {
        buffer[4] = 0x25; // Flags: END_HEADERS | END_STREAM | PRIORITY
    }
    buffer[5] = (streamId >> 24) & 0x7F;
    buffer[6] = (streamId >> 16) & 0xFF;
    buffer[7] = (streamId >> 8) & 0xFF;
    buffer[8] = streamId & 0xFF;

    // copy priority data
    memcpy(buffer + 9, priorityData, sizeof(priorityData));
    // copy HPACK payload
    memcpy(buffer + 9 + sizeof(priorityData), hpackBuffer, hpackPayloadLen);

    return totalPayloadLen;
}
