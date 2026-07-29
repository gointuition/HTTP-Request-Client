//
// Created by Intuition on 26-4-7.
//

#include "ResponseHandler.h"

#include <string.h>
#include <zlib.h>
#include <ctype.h>

#include "Compat.h"

#include "SSLHandler.h"
#include "Error.h"
#include "CompressHandler.h"
#include "Log.h"

#include "brotli/decode.h"

// SETTINGS id to name mapping
typedef  struct {
    uint16_t id;
    const char *name;
} Http2SettingsFrame;

static const Http2SettingsFrame http2SettingsFrame[] = {
        {0x1, "SETTINGS_HEADER_TABLE_SIZE"},
        {0x2, "SETTINGS_ENABLE_PUSH"},
        {0x3, "SETTINGS_MAX_CONCURRENT_STREAMS"},
        {0x4, "SETTINGS_INITIAL_WINDOW_SIZE"},
        {0x5, "SETTINGS_MAX_FRAME_SIZE"},
        {0x6, "SETTINGS_MAX_HEADER_LIST_SIZE"}
};

// static table (RFC 7541 Appendix A)
typedef struct {
    const char *name;
    const char *value;
} ResponseHeaderStaticTable;

static const ResponseHeaderStaticTable responseHeaderStaticTable[] = {
        {"", ""}, // index 0 is preserved
        {":authority", ""},
        {":method", "GET"},
        {":method", "POST"},
        {":path", "/"},
        {":path", "/index.html"},
        {":scheme", "http"},
        {":scheme", "https"},
        {":status", "200"},
        {":status", "204"},
        {":status", "206"},
        {":status", "304"},
        {":status", "400"},
        {":status", "404"},
        {":status", "500"},
        {"accept-charset", ""},
        {"accept-encoding", "gzip, deflate"},
        {"accept-language", ""},
        {"accept-ranges", ""},
        {"accept", ""},
        {"access-control-allow-origin", ""},
        {"age", ""},
        {"allow", ""},
        {"authorization", ""},
        {"cache-control", ""},
        {"content-disposition", ""},
        {"content-encoding", ""},
        {"content-language", ""},
        {"content-length", ""},
        {"content-location", ""},
        {"content-range", ""},
        {"content-type", ""},
        {"cookie", ""},
        {"date", ""},
        {"etag", ""},
        {"expect", ""},
        {"expires", ""},
        {"from", ""},
        {"host", ""},
        {"if-match", ""},
        {"if-modified-since", ""},
        {"if-none-match", ""},
        {"if-range", ""},
        {"if-unmodified-since", ""},
        {"last-modified", ""},
        {"link", ""},
        {"location", ""},
        {"max-forwards", ""},
        {"proxy-authenticate", ""},
        {"proxy-authorization", ""},
        {"range", ""},
        {"referer", ""},
        {"refresh", ""},
        {"retry-after", ""},
        {"server", ""},
        {"set-cookie", ""},
        {"strict-transport-security", ""},
        {"transfer-encoding", ""},
        {"user-agent", ""},
        {"vary", ""},
        {"via", ""},
        {"www-authenticate", ""}
};

typedef enum {
    ENCODING_IDENTITY = 0,
    ENCODING_GZIP = 1,
    ENCODING_DEFLATE = 2,
    ENCODING_BROTLI = 4,
    ENCODING_ZSTD = 8
} ContentEncoding;

static void handleDataFrame(Stream *stream, unsigned char *payload, uint32_t length);
static void handleHeadersFrame(Stream *stream, unsigned char *payload, uint32_t length, uint8_t flags, HpackContext *ctx);
static void handleRST_STREAMFrame(Stream *stream, unsigned char *payload, uint32_t length, uint32_t streamId);
static void handleSettingsFrame(unsigned char *payload, uint32_t length);
static void handleWindowUpdateFrame(unsigned char *payload, uint32_t length, uint32_t streamId);
static void handleGoAwayFrame(Session *session, unsigned char *payload, uint32_t length);
static void finalizeResponsePayload(Basket *basket, unsigned char *combinedPayload, size_t combinedPayloadSize);
// static HpackContext *initHpackContext(Basket *basket);
static void decodeHeadersFrame(Stream *stream, unsigned char *payload, size_t length, HpackContext *ctx);
static void getHeaderFromTable(size_t index, char **name, char **value, HpackContext *ctx);
static void freeResponseHeader(ResponseHeader *header);
static void addToDynamicTable(Stream *stream, HpackContext *ctx, const char *name, const char *value);
static const char* getSettingsName(uint16_t id);
static const char* getErrorName(uint32_t code);
static ContentEncoding detectContentEncoding(Basket *basket);

// Process a single fully-buffered HTTP/2 frame. Called from the connection
// reader thread with `stream` locked (or NULL for connection-level frames and
// frames targeting an unknown/closed stream). HEADERS frames are always decoded
// — into a scratch stream when the target is NULL — so the shared HPACK dynamic
// table stays in sync with the arrival order on the wire.
void handleStreamFrame(Session *session, Stream *stream,
                       unsigned char *payload, uint32_t length,
                       uint8_t type, uint8_t flags, uint32_t streamId) {

    LOG("DEBUG", "[Frame] Type: %u, Flags: 0x%02x, Stream: %u, Len: %u", type, flags, streamId, length);

    switch (type) {
        case 0x0: // DATA
            if (stream) { handleDataFrame(stream, payload, length); }
            break;
        case 0x1: { // HEADERS
            if (stream) {
                handleHeadersFrame(stream, payload, length, flags, session -> hpackCtx);
            } else {
                // Unknown/closed stream: decode into a scratch stream purely to
                // advance the shared HPACK dynamic table; discard the headers.
                Stream scratch;
                memset(&scratch, 0, sizeof(scratch));
                handleHeadersFrame(&scratch, payload, length, flags, session -> hpackCtx);
            }
            break;
        }
        case 0x3: // RST_STREAM
            if (stream) {
                handleRST_STREAMFrame(stream, payload, length, streamId);
                stream -> isEnded = 1;
            }
            break;
        case 0x4: // SETTINGS
            handleSettingsFrame(payload, length);
            break;
        case 0x8: // WINDOW_UPDATE
            handleWindowUpdateFrame(payload, length, streamId);
            break;
        case 0x7: // GOAWAY
            handleGoAwayFrame(session, payload, length);
            break;
        default:
            LOG("WARN", "Unknown Frame Type: %u", type);
            break;
    }

    // END_STREAM flag on DATA/HEADERS completes the stream.
    if (stream && (type == 0x0 || type == 0x1) && (flags & 0x1)) {
        stream -> isEnded = 1;
        LOG("DEBUG", "Stream %u ended.", streamId);
    }
}

void finalizeStreamIntoBasket(Basket *basket, Stream *stream) {
    pthread_mutex_lock(&stream -> lock);

    // On any stream/connection error, propagate it and leave the buffers for
    // freeStreamBuffers. Retryable errors (GOAWAY/SETTINGS_TIMEOUT) let the
    // caller retry with a clean basket.
    if (stream -> error.code != NULL) {
        basket -> error = stream -> error;
        pthread_mutex_unlock(&stream -> lock);
        return;
    }

    // Transfer ownership of headers and payload out of the stream.
    basket -> response.headers = stream -> headers;
    basket -> response.numHeaders = stream -> numHeaders;
    stream -> headers = NULL;
    stream -> numHeaders = 0;

    unsigned char *combinedPayload = stream -> combinedPayload;
    size_t combinedPayloadSize = stream -> combinedPayloadSize;
    stream -> combinedPayload = NULL;
    stream -> combinedPayloadSize = 0;

    pthread_mutex_unlock(&stream -> lock);

    // Decompression can be slow; run it without holding the stream lock.
    finalizeResponsePayload(basket, combinedPayload, combinedPayloadSize);
}

void freeStreamBuffers(Stream *stream) {
    if (!stream) { return; }
    if (stream -> headers) {
        for (size_t i = 0; i < stream -> numHeaders; i++) {
            freeResponseHeader(&stream -> headers[i]);
        }
        free(stream -> headers);
        stream -> headers = NULL;
        stream -> numHeaders = 0;
    }
    if (stream -> combinedPayload) {
        free(stream -> combinedPayload);
        stream -> combinedPayload = NULL;
        stream -> combinedPayloadSize = 0;
    }
}

static void handleDataFrame(Stream *stream, unsigned char *payload, uint32_t length) {
    if (length == 0) return;

    unsigned char *newPayload = realloc(stream -> combinedPayload, stream -> combinedPayloadSize + length);
    if (!newPayload) {
        stream -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return;
    }
    stream -> combinedPayload = newPayload;
    memcpy(stream -> combinedPayload + stream -> combinedPayloadSize, payload, length);
    stream -> combinedPayloadSize += length;
}

/**
Without Priority
+-----------------------------------------------+
|                 Length (24)                   |
+---------------+---------------+---------------+
|   Type (8)    |   Flags (8)   |
+-+-------------+---------------+---------------+
|R|                 Stream Identifier (31)      |
+=+=============================================+
|                   Header Block Fragment (*)   |
+---------------------------------------------------------------+

With priority
+-----------------------------------------------+
|                 Length (24)                   |
+---------------+---------------+---------------+
|   Type (8)    |   Flags (8)   |
+-+-------------+---------------+---------------+
|R|                 Stream Identifier (31)      |
+=+=============================================+
|Pad Length? (8)|
+-+-------------+-----------------------------------------------+
|E|                 Stream Dependency (31)                     |
+-+-------------+-----------------------------------------------+
|   Weight (8)  |
+-+-------------+-----------------------------------------------+
|                   Header Block Fragment (*)                   |
+---------------------------------------------------------------+

{
    0x00, 0x00, 0x0F,  // Length = 15
    0x01,              // Type = 1 (HEADERS)
    0x24,              // Flags = 0x24 (END_HEADERS | PRIORITY)
    0x00, 0x00, 0x00, 0x01,  // Stream ID = 1

    // PRIORITY (5字节)
    0x00, 0x00, 0x00, 0x00,  // Stream Dependency = 0 ()
    0x10,                     // Weight = 16

    // Header Block Fragment (10 bits)
    0x82, 0x84, 0x87, 0x41, 0x8a, 0x08, 0x9d, 0x5c, 0x0b, 0x81
}
 */
static void handleHeadersFrame(Stream *stream, unsigned char *payload, uint32_t length, uint8_t flags, HpackContext *ctx) {
    if (!ctx) return;

    unsigned char *payloadStart = payload;
    size_t payloadSize = length;

    // Skip Pad Length if PADDED flag (0x08) is set
    size_t padLength = 0;
    if (flags & 0x08) {
        if (payloadSize < 1) {
            stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
            return;
        }
        padLength = payloadStart[0];
        payloadStart += 1;
        payloadSize -= 1;
    }

    // Skip Priority if present (0x20)
    if (flags & 0x20) {
        if (payloadSize < 5) {
            stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
            return;
        }
        payloadStart += 5;
        payloadSize -= 5;
    }

    // Remove padding from the end
    if (padLength > 0) {
        if (padLength > payloadSize) {
            stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
            return;
        }
        payloadSize -= padLength;
    }

    // Call the existing decoder logic, passing ctx
    decodeHeadersFrame(stream, payloadStart, payloadSize, ctx);
}

static void handleRST_STREAMFrame(Stream *stream, unsigned char *payload, uint32_t length, uint32_t streamId) {
    // RST_STREAM frame payload is 4 bytes, including a 32 bits error code
    if (length == 4) {
        const uint32_t errorCode = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        const char *errorName = getErrorName(errorCode);
        LOG("WARN", "RST_STREAM received for Stream %u. Error Code: 0x%x (%s)",
            streamId, errorCode, errorName);
        stream -> error = ERR_RESPONSE_RST_STREAM_ERROR;
        stream -> error.msg = errorName;
    } else {
        LOG("ERROR", "Invalid RST_STREAM frame length: %u", length);
        stream -> error = ERR_RESPONSE_RST_STREAM_ERROR;
        stream -> error.msg = "Invalid RST_STREAM frame length";
    }
}

/**
+------------------+------------------+
|       Identifier (16)              |  --- 2 bytes
+------------------+------------------+
|                   Value (32)       |  --- 4 bytes
+-----------------------------------+
**/
static void handleSettingsFrame(unsigned char *payload, uint32_t length) {
    for (size_t i = 0; i + 6 <= length; i += 6) {
        // reading a 16-bit SETTINGS frame in big-endian format means the high-order byte comes first
        uint16_t id = (payload[i] << 8) | payload[i + 1];
        uint32_t value = (payload[i + 2] << 24) | (payload[i + 3] << 16) | (payload[i + 4] << 8) | payload[i + 5];
        const char *name = getSettingsName(id);
        LOG("DEBUG", "SETTINGS: %s (0x%04x) = %u", name ? name : "UNKNOWN", id, value);
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
|                   Window Size Increment (32)  |
+---------------------------------------------------------------+

The Window Size Increment feiled MUST be treated as unsigned 31-bit integer
The high bit (bit 31) must be ignored (& 0x7FFFFFFF)
 */
static void handleWindowUpdateFrame(unsigned char *payload, uint32_t length, uint32_t streamId) {
    if (length == 4) {
        const uint32_t increment = ((payload[0] & 0x7F) << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        LOG("DEBUG", "WINDOW_UPDATE: Stream %u, Increment %u", streamId, increment);
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
|                   Last-Stream-ID (31)         |
+-----------------------------------------------+
|                        Error Code (32)        |
+-----------------------------------------------+
|                  Additional Debug Data (*)     |
+---------------------------------------------------------------+
*/
static void handleGoAwayFrame(Session *session, unsigned char *payload, uint32_t length) {
    if (length >= 8) {
        uint32_t lastStreamId = ((payload[0] & 0x7F) << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        uint32_t errorCode = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
        LOG("DEBUG", "GOAWAY: Last Stream %u, Error 0x%x (%s)", lastStreamId, errorCode, getErrorName(errorCode));
        // Mark the connection so it is no longer reused; the reader then fails
        // any still-pending streams (those without a response yet) with this
        // error so their request threads can retry on a fresh connection.
        if (errorCode == 0x4) {
            session -> connError = ERR_SESSION_SETTINGS_TIMEOUT;
        } else {
            session -> connError = ERR_SESSION_GO_AWAY;
        }
        session -> goingAway = 1;
    }
}

static void finalizeResponsePayload(Basket *basket, unsigned char *combinedPayload, size_t combinedPayloadSize) {
    if (combinedPayloadSize == 0) {
        // TODO empty body
        LOG("DEBUG", "no response...");
        basket -> response.payload = NULL;
        return;
    }

    const ContentEncoding encoding = detectContentEncoding(basket);
    if (encoding == ENCODING_IDENTITY) {
        LOG("DEBUG", "plain text...");
        // ensure null-termination for json_string() in basketToString
        unsigned char *terminated = realloc(combinedPayload, combinedPayloadSize + 1);
        if (!terminated) {
            basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            free(combinedPayload);
            return;
        }
        terminated[combinedPayloadSize] = '\0';
        basket -> response.payload = terminated;
        basket -> response.payloadSize = combinedPayloadSize;
        return;
    }

    if ((basket -> decompress & encoding) == 0) {
        LOG("DEBUG", "not decompress...");
        basket -> response.payload = base64Encode(combinedPayload, combinedPayloadSize);
        basket -> response.payloadSize = combinedPayloadSize;
        free(combinedPayload);
        return;
    }

    DecompressedObj *decompressedObj = NULL;
    // // gzip magic number 0x1F 0x8B
    // if (size >= 2 && combinedPayload[0] == 0x1F && combinedPayload[1] == 0x8B) {
    if (encoding == ENCODING_GZIP) {
        decompressedObj = decompress_GZip(combinedPayload, combinedPayloadSize);
    } else if (encoding == ENCODING_BROTLI) {
        decompressedObj = decompress_Brotli(combinedPayload, combinedPayloadSize);
    } else if (encoding == ENCODING_DEFLATE) {
        decompressedObj = decompress_Deflate(combinedPayload, combinedPayloadSize);
    } else if (encoding == ENCODING_ZSTD) {
        decompressedObj = decompress_Zstd(combinedPayload, combinedPayloadSize);
    }

    if (decompressedObj == NULL || decompressedObj -> error.code != NULL) {
        basket -> error = ERR_RESPONSE_INFLATE_UNKNOWN_ERROR;
        free(combinedPayload);
        return;
    }
    if (decompressedObj -> error.code != NULL) {
        basket -> error = decompressedObj -> error;
        if (decompressedObj -> decompressedPayload != NULL) {
            free(decompressedObj -> decompressedPayload);
        }
        free(decompressedObj);
        free(combinedPayload);
        return;
    }

    basket -> response.payload = decompressedObj -> decompressedPayload;
    free(decompressedObj);
    free(combinedPayload);
}

static ContentEncoding detectContentEncoding(Basket *basket) {
    for (size_t i = 0; i < basket -> response.numHeaders; i++) {
        if (strcasecmp(basket -> response.headers[i].name, "content-encoding") == 0) {
            if (strcasecmp(basket -> response.headers[i].value, "gzip") == 0) { return ENCODING_GZIP; }
            if (strcasecmp(basket -> response.headers[i].value, "br") == 0) { return ENCODING_BROTLI; }
            if (strcasecmp(basket -> response.headers[i].value, "deflate") == 0) { return ENCODING_DEFLATE; }
            if (strcasecmp(basket -> response.headers[i].value, "zstd") == 0) { return ENCODING_ZSTD; }
            break;
        }
    }
    return ENCODING_IDENTITY;
}

static const char* getSettingsName(uint16_t id) {
    for (size_t i = 0; i < sizeof(http2SettingsFrame) / sizeof(http2SettingsFrame[0]); i++) {
        if (http2SettingsFrame[i].id == id) return http2SettingsFrame[i].name;
    }
    return NULL;
}

static void decodeHeadersFrame(Stream *stream, unsigned char *payload, size_t length, HpackContext *ctx) {
    if (!payload || length == 0) { return; }

    LOG("DEBUG", "Response HEADERS frame: raw payload (%zu bytes): ", length);
//    for (size_t i = 0; i < length && i < 32; i++) {
//        printf("%02x ", payload[i]);
//    }
//    if (length < 32) { printf("..."); }
//    printf("");

    stream -> numHeaders = 0;
    size_t pos = 0;
    while (pos < length) {
        if (pos + 1 > length) {
            LOG("ERROR", "Response HEADERS frame: incomplete header field at position %zu", pos);
            stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
            break;
        }

        /**
        +---+---+---+---+---+---+---+---+
        | 0 | 0 | 1 |   Max size (5+)   |
        +---+---+---+---+---+---+---+---+
         */
        uint8_t firstByte = payload[pos];
        // check if dynamic table size updates
        if ((firstByte & 0xE0) == 0x20) {
            // dynamic table size updates
            size_t maxSize = hpackDecodeInteger(payload, &pos, 5, length);
            ctx -> dynamicTableMaxSize = maxSize;
            LOG("DEBUG", "Response HEADERS frame: dynamic table size update %zu", maxSize);
            continue;
        }

        int isError = 0;
        int shouldAddToDynamicTable = 0;

        ResponseHeader resHeader = { NULL, NULL, 1, 1 };

        if (firstByte & 0x80) {
            /**
             * Hpack Indexed Headers Fields, 1 indicates this a indexed header field, 7+ indicates index value
            +---+---+---+---+---+---+---+---+
            | 1 |        Index (7+)          |
            +---+---+---+---+---+---+---+---+
             */
            // indexed header field (1xxx xxxx)
            size_t index = hpackDecodeInteger(payload, &pos, 7, length);
            if (index == 0) {
                LOG("ERROR", "Response HEADERS frame: invalid index 0");
                stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                continue;
            }
            // Validate dynamic table index (indices >= static table size refer to dynamic table)
            size_t staticTableCount = sizeof(responseHeaderStaticTable) / sizeof(responseHeaderStaticTable[0]);
            if (index >= staticTableCount) {
                size_t dynamicIndex = index - (staticTableCount - 1) - 1;
                if (dynamicIndex >= ctx -> dynamicTableSize) {
                    LOG("ERROR", "Response HEADERS frame: dynamic table index %zu out of range (table size %zu)",
                        dynamicIndex, ctx -> dynamicTableSize);
                    stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                    continue;
                }
            }

            getHeaderFromTable(index, &resHeader.name, &resHeader.value, ctx);
//            LOG("DEBUG", "Response HEADERS frame: %s: %s", resHeader.name ? resHeader.name : "name error", resHeader.value ? resHeader.value : "value error");
        } else if (firstByte & 0x40) {
            // literal header field with index (01xx, xxxx)
            size_t index = hpackDecodeInteger(payload, &pos, 6, length);

            if (index > 0) {
                // index name
                getHeaderFromTable(index, &resHeader.name, &resHeader.value, ctx);
                free(resHeader.value); // name only, value is not required
            } else {
                /**
                +---+---+---+---+---+---+---+---+
                | 0 | 1 | H |      Name Index (4+)   |
                +---+---+---+---+---+---+---+---+
                | H |     Name Length (7+)          |
                +---+---+---+---+---+---+---+---+
                |  Name String (Length octets)      |
                +---+---+---+---+---+---+---+---+
                | H |     Value Length (7+)         |
                +---+---+---+---+---+---+---+---+
                | Value String (Length octets)      |
                +---+---+---+---+---+---+---+---+
                 */
                // literal name
                resHeader.name = hpackDecodeString(payload, &pos, length);
                if (!resHeader.name) {
                    LOG("ERROR", "Failed to decode header name");
                    stream -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                    isError = 1;
                    break;
                }
            }

            // decode value (always literal for this type)
            resHeader.value = hpackDecodeString(payload, &pos, length);
            if (!resHeader.value) {
                LOG("ERROR", "Failed to decode header value");
                freeResponseHeader(&resHeader);
                stream->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                break;
            }

            // TODO handle special headers with binary data
            int isBinaryHeader = resHeader.name && (strcasecmp(resHeader.name, "content-security-policy") == 0
                                                    || strcasecmp(resHeader.name, "set-cookie") == 0
                                                    || strstr(resHeader.name, "binary") != NULL);

            // add name and value to dynamic table
            if (resHeader.name && resHeader.value) {
                shouldAddToDynamicTable = 1;
//                addToDynamicTable(basket, ctx, resHeader.name, resHeader.value);
            }
        } else {
            // literal header field, without index (000x xxxx)

            /**
             In HPACK specification:
             - The 5th bit (mask 0x10) of the first byte for this encoding type is the "never index" flag.
             - If neverIndex is non-zero (i.e., the bit is set, 1), it means this header field should not be added to the dynamic table (even if other conditions would normally allow it). This is often used for sensitive headers that shouldn't be cached/Indexed (e.g., Authorization).
             - If neverIndex is 0 (bit not set), the header might be eligible for addition to the dynamic table (depending on other rules).
             */
            uint8_t neverIndex = firstByte & 0x10;
            /**
             First Byte Format:
             0   0   0   N   x   x   x   x
             │   │   │   │   └───────────── 4-bit prefix for the Name Index (the 'xxxx' part)
             │   │   │   └───────────────── "Never Indexed" flag (N)
             │   │   └───────────────────── Reserved bit (must be 0)
             │   └───────────────────────── Reserved bit (must be 0)
             └───────────────────────────── Reserved bit (must be 0)
             */
            size_t index = hpackDecodeInteger(payload, &pos, 4, length);

            if (index > 0) {
                // indexed name
                getHeaderFromTable(index, &resHeader.name, &resHeader.value, ctx);
                free(resHeader.value);
            } else {
                // literal name
                resHeader.name = hpackDecodeString(payload, &pos, length);
                if (!resHeader.name) {
                    LOG("ERROR", "Failed to decode header name");
                    stream->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                    break;
                }
            }

            // literal value
            resHeader.value = hpackDecodeString(payload, &pos, length);
            if (!resHeader.value) {
                LOG("ERROR", "Failed to decode header value");
                freeResponseHeader(&resHeader);
                stream->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                break;
            }

            // never add to dynamic table if never-index flag is set
            if (!neverIndex) {
                shouldAddToDynamicTable = 1;
            }

            // TODO handle special headers with binary data
            int isBinaryHeader = resHeader.name && (strcasecmp(resHeader.name, "content-security-policy") == 0
                                                    || strcasecmp(resHeader.name, "set-cookie") == 0
                                                    || strstr(resHeader.name, "binary") != NULL);

        }

        // Debug print header
        if (resHeader.name && resHeader.value) {
            size_t valueLen = strlen(resHeader.value);
            int isPrintable = 1;
            int nonPrintCount = 0;

            for (size_t i = 0; i < valueLen; i++) {
                unsigned char c = (unsigned char)resHeader.value[i];
                if (!isprint(c) && !isspace(c)) {
                    nonPrintCount++;
                }
            }

            if (nonPrintCount > (int)(valueLen / 8)) {
                isPrintable = 0;
            }

            if (isPrintable) {
                LOG("DEBUG", "Response HEADERS frame: %s: %s", resHeader.name, resHeader.value);
            } else {
                LOG("DEBUG", "Response HEADERS frame: %s: (Hex, first 64 bytes) ", resHeader.name);
                size_t printLen = valueLen < 64 ? valueLen : 64;
                for (size_t i = 0; i < printLen; i++) {
                    LOG("DEBUG", "%02x", (unsigned char)resHeader.value[i]);
                }
            }
        }

        // Add to dynamic table if needed
        if (shouldAddToDynamicTable && resHeader.name && resHeader.value) {
            addToDynamicTable(stream, ctx, resHeader.name, resHeader.value);
        }

        // Store header in the stream (a NULL headers buffer means this is a
        // scratch decode for an unknown/closed stream: keep the HPACK table in
        // sync but discard the header itself).
        if (stream -> headers) {
            if (stream -> numHeaders < RESPONSE_HEADERS_MAX_SIZE) {
                stream -> headers[stream -> numHeaders++] = resHeader;
            } else {
                LOG("WARN", "Response headers limit reached (%d), discarding remaining",
                    RESPONSE_HEADERS_MAX_SIZE);
                freeResponseHeader(&resHeader);
                break;
            }
        } else {
            freeResponseHeader(&resHeader);
        }

        if (isError == 1) {
            LOG("ERROR", "Response HEADERS frame: error during header decoding at position %zu, skipping to next header", pos);
            pos++;
        }
    }
}

static void getHeaderFromTable(size_t index, char **name, char **value, HpackContext *ctx) {
    if (index == 0) {
        *name = strdup("");
        *value = strdup("");
        return;
    }
    if (index <= sizeof(responseHeaderStaticTable) / sizeof(responseHeaderStaticTable[0]) - 1) {
        // static table
        *name = strdup(responseHeaderStaticTable[index].name);
        *value = strdup(responseHeaderStaticTable[index].value);
    } else {
        // dynamic table
        size_t dynamicIndex = index - (sizeof(responseHeaderStaticTable) / sizeof(responseHeaderStaticTable[0]) - 1) - 1;
        if (dynamicIndex < ctx -> dynamicTableSize) {
            *name = strdup(ctx -> dynamicTable[dynamicIndex].name);
            *value = strdup(ctx -> dynamicTable[dynamicIndex].value);
        } else {
            *name = strdup("error");
            *value = strdup("invalid index");
        }
    }
}

static void freeResponseHeader(ResponseHeader *header) {
    if (!header) return;
    if (header->name && header->freeName) {
        free(header->name);
        header->name = NULL;
    }
    if (header->value && header->freeValue) {
        free(header->value);
        header->value = NULL;
    }
}

static void addToDynamicTable(Stream *stream, HpackContext *ctx, const char *name, const char *value) {
    size_t entrySize = strlen(name) + strlen(value) + 32; // 32 HPack overhead

    // remove oldest entry
    while (ctx -> dynamicTableSize > 0 && ctx -> dynamicTableMaxSize < entrySize) {
        const size_t lastIndex = ctx -> dynamicTableSize - 1;
        const size_t removedSize = strlen(ctx -> dynamicTable[lastIndex].name)
                                   + strlen(ctx -> dynamicTable[lastIndex].value) + 32;
        free(ctx -> dynamicTable[lastIndex].name);
        free(ctx -> dynamicTable[lastIndex].value);
        ctx -> dynamicTableSize --;
        ctx -> dynamicTableMaxSize += removedSize;
    }

    // resize
    if (ctx -> dynamicTableSize >= ctx -> dynamicTableCapacity) {
        size_t newCapacity = ctx -> dynamicTableCapacity ? 2 * ctx -> dynamicTableCapacity : 8;
        HpackTableEntry *newTable = realloc(ctx -> dynamicTable, newCapacity * sizeof(HpackTableEntry));
        if (!newTable) {
            LOG("ERROR", "Response HEADERS frame: dynamic table capacity realloc failed");
            stream -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
            return;
        }
        ctx -> dynamicTable = newTable;
        ctx -> dynamicTableCapacity = newCapacity;
    }

    // shift existing entries
    memmove(&ctx -> dynamicTable[1], &ctx -> dynamicTable[0], ctx -> dynamicTableSize * sizeof(HpackTableEntry));

    // store new entry
    ctx -> dynamicTable[0].name = strdup(name);
    ctx -> dynamicTable[0].value = strdup(value);
    ctx -> dynamicTableSize++;
    ctx -> dynamicTableMaxSize -= entrySize;
}

// HTTP/2 error code to name mapping
static const struct {
    uint32_t code;
    const char *name;
} HTTP2ErrorNames[] = {
        {0x0, "NO_ERROR"},
        {0x1, "PROTOCOL_ERROR"},
        {0x2, "INTERNAL_ERROR"},
        {0x3, "FLOW_CONTROL_ERROR"},
        {0x4, "SETTINGS_TIMEOUT"},
        {0x5, "STREAM_CLOSED"},
        {0x6, "FRAME_SIZE_ERROR"},
        {0x7, "REFUSED_STREAM"},
        {0x8, "CANCEL"},
        {0x9, "COMPRESSION_ERROR"},
        {0xa, "CONNECT_ERROR"},
        {0xb, "ENHANCE_YOUR_CALM"},
        {0xc, "INADEQUATE_SECURITY"},
        {0xd, "HTTP_1_1_REQUIRED"}
};

static const char* getErrorName(uint32_t code) {
    for (size_t i = 0; i < sizeof(HTTP2ErrorNames) / sizeof(HTTP2ErrorNames[0]); i++) {
        if (code == HTTP2ErrorNames[i].code) { return HTTP2ErrorNames[i].name; }
    }
    return "STREAM_UNKNOWN_ERROR";
}
