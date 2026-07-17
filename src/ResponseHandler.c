//
// Created by Intuition on 26-4-7.
//

#include "ResponseHandler.h"

#include <string.h>
#include <netdb.h>

#include <arpa/inet.h>

#include <unistd.h>
#include <zlib.h>
#include <ctype.h>
#include <sys/fcntl.h>

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

static void processFrame(Basket *basket, unsigned char *payload, uint32_t length,
                         uint8_t type, uint8_t flags, uint32_t streamId,
                         HpackContext *ctx, int *isStreamEnded,
                         unsigned char **combinedPayload, size_t *combinedPayloadSize);
static void handleDataFrame(Basket *basket, unsigned char *payload, uint32_t length, unsigned char **combinedPayload, size_t *combinedPayloadSize);
static void handleHeadersFrame(Basket *basket, unsigned char *payload, uint32_t length, uint8_t flags, HpackContext *ctx);
static void handleRST_STREAMFrame(Basket *basket, unsigned char *payload, uint32_t length, uint32_t streamId);
static void handleSettingsFrame(unsigned char *payload, uint32_t length);
static void handleWindowUpdateFrame(unsigned char *payload, uint32_t length, uint32_t streamId);
static void handleGoAwayFrame(Basket *basket, unsigned char *payload, uint32_t length);
static void finalizeResponsePayload(Basket *basket, unsigned char *combinedPayload, size_t combinedPayloadSize);
// static HpackContext *initHpackContext(Basket *basket);
static void decodeHeadersFrame(Basket *basket, unsigned char *payload, size_t length, HpackContext *ctx);
static void getHeaderFromTable(size_t index, char **name, char **value, HpackContext *ctx);
static void freeResponseHeader(ResponseHeader *header);
static void addToDynamicTable(Basket *basket, HpackContext *ctx, const char *name, const char *value);
static const char* getSettingsName(uint16_t id);
static const char* getErrorName(uint32_t code);
static ContentEncoding detectContentEncoding(Basket *basket);

void receiveResponse(Basket *basket) {
    basket -> response.headers = (ResponseHeader *) malloc(sizeof(ResponseHeader) * RESPONSE_HEADERS_MAX_SIZE);
    if (!basket -> response.headers) {
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return;
    }

    // HpackContext *hpackCtx = initHpackContext(basket);
    HpackContext *hpackCtx = basket -> session -> hpackCtx;
    if (!hpackCtx && basket -> error.code != NULL) {
        return;
    }

    unsigned char *fullResponse = NULL;
    size_t fullResponseSize = 0;

    unsigned char *combinedPayload = NULL;
    size_t combinedPayloadSize = 0;

    int totalBytes = 0;

    int isStreamEnded = 0;

    // set socket to non-blocking
    int fd = SSL_get_fd(basket -> session -> ssl);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int timeout = 0;
    int maxTimeout = basket -> responseReadingTimeoutInMilliseconds;

    LOG("DEBUG", "waiting for response...");
    while (!isStreamEnded && basket -> error.code == NULL) {
        if (timeout >= maxTimeout) {
            LOG("ERROR", "no response after %dms timeout", maxTimeout);
            basket -> error = totalBytes > 0 ? ERR_RESPONSE_NO_CONTENT_AFTER_READING_TIMEOUT : ERR_RESPONSE_PARTIAL_CONTENT_AFTER_READING_TIMEOUT;
            basket -> response.payload = NULL;
            break;
        }
        unsigned char buffer[4096];
        int bytesRead = SSL_read(basket -> session -> ssl, buffer, sizeof(buffer));

        if (bytesRead > 0) {
            timeout = 0;

            // use realloc to extend dynamically
            unsigned char *newResponse = realloc(fullResponse, fullResponseSize + bytesRead);
            if (newResponse == NULL) {
                if (fullResponse != NULL) { free(fullResponse); }
                LOG("ERROR", "response content memory allocation failed");
                basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
                break;
            }

            fullResponse = newResponse;
            memcpy(fullResponse + fullResponseSize, buffer, bytesRead);
            fullResponseSize += bytesRead;
            totalBytes += bytesRead;

            LOG("DEBUG", "Received %d bytes, total: %d, consecutive_empty: %d", bytesRead, totalBytes, timeout);

            /**
             * frame header: 3 + 2 + 4 = 9
            +-----------------------------------------------+
            |                 Length (24)                   |   ← 3 bytes frameLength 24 bytes, max 2^24 - 1
            +---------------+---------------+---------------+
            |   Type (8)    |   Flags (8)   |                   ← 2 bytes
            +-+-------------+---------------+---------------+
            |R|                 Stream Identifier (31)      |   ← 4 bytes
            +=+================================== ===========+
            |                   Frame Payload (0...)            ←
            +---------------------------------------------------------------+

            unsigned char frame[] = {
                0x00, 0x00, 0x05,  // Length = 5 (payload 5 bytes)
                0x00,              // Type = 0 (DATA)
                0x01,              // Flags = 1 (END_STREAM)
                0x00, 0x00, 0x00, 0x01,  // Stream ID = 1
                0x48, 0x65, 0x6C, 0x6C, 0x6F  // Payload: "Hello" (5 bytes)
            };
            */
            // only handle new received data, but one frame may be chunked, after collecting all chunks, parse
            // size_t offset = fullResponseSize - bytesRead - frameChunkSize;
            size_t offset = 0;
            while (offset + 9 <= fullResponseSize) {
                // Parse Frame Header
                // Big-Endian parsing
                uint32_t frameLength = (fullResponse[offset] << 16) | (fullResponse[offset+1] << 8) | fullResponse[offset+2];
                uint8_t frameType = fullResponse[offset+3];
                uint8_t frameFlags = fullResponse[offset+4];
                uint32_t streamId = ((fullResponse[offset+5] & 0x7F) << 24) | (fullResponse[offset+6] << 16) | (fullResponse[offset+7] << 8) | fullResponse[offset+8];

                // Check if full frame is available TODO different from below in #receiveResponseS
                if (offset + 9 + frameLength > fullResponseSize) {
                    break; // Wait for more data
                }

                unsigned char *payload = fullResponse + offset + 9;

                // Process Frame
                processFrame(basket, payload, frameLength, frameType, frameFlags, streamId, hpackCtx, &isStreamEnded, &combinedPayload, &combinedPayloadSize);

                if (basket -> error.code != NULL) { break; }

                offset += 9 + frameLength;
            }

            // Remove processed bytes from buffer (optional optimization: use ring buffer or move remaining)
            if (offset > 0) {
                size_t remaining = fullResponseSize - offset;
                if (remaining > 0) {
                    memmove(fullResponse, fullResponse + offset, remaining);
                }
                fullResponseSize = remaining;
            }
        } else if (bytesRead == 0) {
            // Connection closed TODO handle after too long time, free memory
            LOG("ERROR", "connection closed");
            break;
        } else {
            int err = SSL_get_error(basket -> session -> ssl, bytesRead);
            if (err == SSL_ERROR_WANT_READ) {
                usleep(10000);
                LOG("DEBUG", "waited %dms", timeout);
                timeout += 10; // 10 ms
            } else {
                basket -> error = ERR_RESPONSE_READING_UNKNOWN_ERROR;
                break;
            }
        }
    }

    // decompress response
    if (basket -> error.code == NULL) {
        finalizeResponsePayload(basket, combinedPayload, combinedPayloadSize);
    }

    if (fullResponse) { free(fullResponse); }

    // if (hpackCtx) { freeHpackContext(hpackCtx); }
    // reset socket flag
    fcntl(fd, F_SETFL, flags);
}

static void processFrame(Basket *basket, unsigned char *payload, uint32_t length,
                         uint8_t type, uint8_t flags, uint32_t streamId,
                         HpackContext *ctx, int *isStreamEnded,
                         unsigned char **combinedPayload, size_t *combinedPayloadSize) {

    LOG("DEBUG", "[Frame] Type: %u, Flags: 0x%02x, Stream: %u, Len: %u", type, flags, streamId, length);

    switch (type) {
        case 0x0: // DATA
            handleDataFrame(basket, payload, length, combinedPayload, combinedPayloadSize);
            break;
        case 0x1: // HEADERS
            handleHeadersFrame(basket, payload, length, flags, ctx);
            break;
        case 0x3: // RST_STREAM
            *isStreamEnded = 1;
            handleRST_STREAMFrame(basket, payload, length, streamId);
            break;
        case 0x4: // SETTINGS
            handleSettingsFrame(payload, length);
            break;
        case 0x8: // WINDOW_UPDATE
            handleWindowUpdateFrame(payload, length, streamId);
            break;
        case 0x7: // GOAWAY
            handleGoAwayFrame(basket, payload, length);
            break;
        default:
            LOG("WARN", "Unknown Frame Type: %u", type);
            break;
    }

    // Check for Stream End
    if (streamId > 0 && streamId % 2 == 1) {
        if ((type == 0x0 || type == 0x1) && (flags & 0x1)) {
            *isStreamEnded = 1;
            LOG("DEBUG", "Stream %u ended.", streamId);
        }
        if (type == 0x3) { // RST_STREAM
            *isStreamEnded = 1;
            const uint32_t errorCode = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
            LOG("DEBUG", "RST_STREAM received. Error Code: 0x%x", errorCode);
        }
    }
}

static void handleDataFrame(Basket *basket, unsigned char *payload, uint32_t length,
                            unsigned char **combinedPayload, size_t *combinedPayloadSize) {
    if (length == 0) return;

    unsigned char *newPayload = realloc(*combinedPayload, *combinedPayloadSize + length);
    if (!newPayload) {
        basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
        return;
    }
    *combinedPayload = newPayload;
    memcpy(*combinedPayload + *combinedPayloadSize, payload, length);
    *combinedPayloadSize += length;
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
static void handleHeadersFrame(Basket *basket, unsigned char *payload, uint32_t length, uint8_t flags, HpackContext *ctx) {
    if (!ctx) return;

    unsigned char *payloadStart = payload;
    size_t payloadSize = length;

    // Skip Priority if present
    if (flags & 0x20) {
        if (payloadSize < 5) {
            basket -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
            return;
        }
        payloadStart += 5;
        payloadSize -= 5;
    }

    // Call the existing decoder logic, passing ctx
    decodeHeadersFrame(basket, payloadStart, payloadSize, ctx);
}

static void handleRST_STREAMFrame(Basket *basket, unsigned char *payload, uint32_t length, uint32_t streamId) {
    // RST_STREAM frame payload is 4 bytes, including a 32 bits error code
    if (length == 4) {
        const uint32_t errorCode = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        const char *errorName = getErrorName(errorCode);
        LOG("WARN", "RST_STREAM received for Stream %u. Error Code: 0x%x (%s)",
            streamId, errorCode, errorName);
        basket -> error = ERR_RESPONSE_RST_STREAM_ERROR;
        basket -> error.msg = errorName;
    } else {
        LOG("ERROR", "Invalid RST_STREAM frame length: %u", length);
        basket -> error = ERR_RESPONSE_RST_STREAM_ERROR;
        basket -> error.msg = "Invalid RST_STREAM frame length";
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
static void handleGoAwayFrame(Basket *basket, unsigned char *payload, uint32_t length) {
    if (length >= 8) {
        uint32_t lastStreamId = ((payload[0] & 0x7F) << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
        uint32_t errorCode = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
        LOG("DEBUG", "GOAWAY: Last Stream %u, Error 0x%x (%s)", lastStreamId, errorCode, getErrorName(errorCode));
        if (errorCode == 0x0 && basket -> response.numHeaders == 0) { // tls.peet.ws will close the connection every time
            basket -> error = ERR_SESSION_GO_AWAY;
        }
        if (errorCode == 0x4) {
            basket -> error = ERR_SESSION_SETTINGS_TIMEOUT;
        }
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

static void decodeHeadersFrame(Basket *basket, unsigned char *payload, size_t length, HpackContext *ctx) {
    if (!payload || length == 0) { return; }

    LOG("DEBUG", "Response HEADERS frame: raw payload (%zu bytes): ", length);
//    for (size_t i = 0; i < length && i < 32; i++) {
//        printf("%02x ", payload[i]);
//    }
//    if (length < 32) { printf("..."); }
//    printf("");

    basket -> response.numHeaders = 0;
    size_t pos = 0;
    while (pos < length) {
        if (pos + 1 > length) {
            LOG("ERROR", "Response HEADERS frame: incomplete header field at position %zu", pos);
            basket -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
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
            if (index == 0 || index >= sizeof(responseHeaderStaticTable) / sizeof(responseHeaderStaticTable[0])) {
                LOG("ERROR", "Response HEADERS frame: invalid index %zu", index);
                basket -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                continue;
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
                    basket -> error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                    isError = 1;
                    break;
                }
            }

            // decode value (always literal for this type)
            resHeader.value = hpackDecodeString(payload, &pos, length);
            if (!resHeader.value) {
                LOG("ERROR", "Failed to decode header value");
                freeResponseHeader(&resHeader);
                basket->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
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
                    basket->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
                    break;
                }
            }

            // literal value
            resHeader.value = hpackDecodeString(payload, &pos, length);
            if (!resHeader.value) {
                LOG("ERROR", "Failed to decode header value");
                freeResponseHeader(&resHeader);
                basket->error = ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED;
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
            addToDynamicTable(basket, ctx, resHeader.name, resHeader.value);
        }

        // Store header in response
        if (basket -> response.numHeaders < RESPONSE_HEADERS_MAX_SIZE) {
            basket -> response.headers[basket -> response.numHeaders++] = resHeader;
        } else {
            LOG("WARN", "Response headers limit reached (%d), discarding remaining",
                RESPONSE_HEADERS_MAX_SIZE);
            freeResponseHeader(&resHeader);
            break;
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

static void addToDynamicTable(Basket *basket, HpackContext *ctx, const char *name, const char *value) {
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
            basket -> error = ERR_SYSTEM_MEMORY_ALLOCATION_FAILED;
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
