//
// Created by Intuition on 26-8-31.
//

#include "ResponseStream.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Error.h"
#include "Log.h"

// One delivery pass of the body funnel: accumulates what the consumer took.
typedef struct {
    Stream *stream;
    size_t delivered;
} BodyDelivery;

static void prepareStreamDecoder(Stream *stream, ResponseSink *sink);
static Error decodeError(ContentEncoding encoding);

static int bufferStreamPayload(Stream *stream, const unsigned char *data, size_t n);
static int deliverStreamPayload(Stream *stream, ResponseSink *sink, const unsigned char *data, size_t n);
static int consumeResponseBody(void *ctx, const unsigned char *data, size_t len);
static void abortResponseSink(Stream *stream, ResponseSink *sink, size_t delivered);

ResponseSink* buildResponseSink(Basket *basket, const ResponseStream *stream) {
    ResponseSink *sink = calloc(1, sizeof(ResponseSink));
    if (sink == NULL) { return NULL; }
    if (stream != NULL) { sink -> contract = *stream; }
    sink -> collecting = stream == NULL;
    if (sink -> collecting) {
        LOG("DEBUG", "no ResponseStream given, the library will collect the response body");
    }
    sink -> decompressMask = basket -> decompress;
    sink -> encoding = ENCODING_IDENTITY;
    sink -> error = ERR_NONE;
    return sink;
}

void freeResponseSink(ResponseSink *sink) {
    if (sink == NULL) { return; }
    freeStreamDecompressor(sink -> decompressor);
    free(sink);
}

int isCompleteResponseStream(const ResponseStream *stream) {
    return stream -> onHeaders != NULL && stream -> onData != NULL && stream -> onComplete != NULL;
}

void prepareStreamRetry(Basket *basket) {
    ResponseSink *sink = basket -> sink;
    if (sink == NULL) { return; }

    freeStreamDecompressor(sink -> decompressor);
    sink -> decompressor = NULL;
    sink -> encoding = ENCODING_IDENTITY;
    sink -> delivered = 0;
    sink -> headersDelivered = 0;
    sink -> completed = 0;
    sink -> aborted = 0;
    sink -> cancelled = 0;
    sink -> cancellationTaken = 0;
    sink -> error = ERR_NONE;
}

// Runs once per attempt as soon as the response headers are complete.
void deliverResponseHeaders(Stream *stream) {
    ResponseSink *sink = stream -> sink;
    if (sink == NULL || sink -> headersDelivered) { return; }
    sink -> headersDelivered = 1;

    prepareStreamDecoder(stream, sink);
    if (sink -> contract.onHeaders != NULL) {
        sink -> contract.onHeaders(sink -> contract.userData, stream -> headers, stream -> numHeaders);
    }
}

static void prepareStreamDecoder(Stream *stream, ResponseSink *sink) {
    sink -> encoding = detectContentEncoding(stream -> headers, stream -> numHeaders);

    const int decompressRequested = (sink -> decompressMask & sink -> encoding) != 0;
    if (!decompressRequested || sink -> encoding == ENCODING_IDENTITY) { return; }

    sink -> decompressor = buildStreamDecompressor(sink -> encoding);
    if (sink -> decompressor == NULL) {
        stream -> error = ERR_RESPONSE_INFLATE_UNKNOWN_ERROR;
        sink -> error = stream -> error;
        sink -> cancelled = 1;
        return;
    }
    // The funnel decodes as the body arrives, so finalize must not run the
    // one-shot decoder over the collected payload again.
    stream -> payloadDecoded = 1;
}

static Error decodeError(ContentEncoding encoding) {
    if (encoding == ENCODING_GZIP) { return ERR_RESPONSE_GZIP_INFLATE_FAILED; }
    if (encoding == ENCODING_BROTLI) { return ERR_RESPONSE_BROTLI_INFLATE_FAILED; }
    if (encoding == ENCODING_DEFLATE) { return ERR_RESPONSE_DEFLATE_INFLATE_FAILED; }
    if (encoding == ENCODING_ZSTD) { return ERR_RESPONSE_ZSTD_INFLATE_FAILED; }
    return ERR_RESPONSE_INFLATE_UNKNOWN_ERROR;
}

// Body funnel shared by both protocols: decode the chunk and hand it to the
// sink, which either delivers it to the caller's callbacks or accumulates it
// into the stream payload. Returns 1 to keep reading, -1 when no more body is
// wanted; failure is reported through stream->error.
int appendStreamPayload(Stream *stream, const unsigned char *data, size_t n) {
    if (n == 0) { return 1; }
    ResponseSink *sink = stream -> sink;
    if (sink == NULL) { return bufferStreamPayload(stream, data, n); }
    return deliverStreamPayload(stream, sink, data, n);
}

static int bufferStreamPayload(Stream *stream, const unsigned char *data, size_t n) {
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

static int deliverStreamPayload(Stream *stream, ResponseSink *sink, const unsigned char *data, size_t n) {
    if (sink -> aborted) { return -1; }

    BodyDelivery delivery = { stream, 0 };
    const int consumed = feedStreamDecompressor(sink -> decompressor, data, n, consumeResponseBody, &delivery);
    sink -> delivered += delivery.delivered;
    stream -> lastActivityTime = time(NULL);

    const int decodeFailed = consumed < 0 && sink -> error.code == NULL;
    if (decodeFailed) {
        sink -> error = decodeError(sink -> encoding);
        stream -> error = sink -> error;
        sink -> cancelled = 1;
        LOG("ERROR", "streaming response could not be decoded (%d)", sink -> encoding);
    }

    const int keepReading = consumed == 1 && !sink -> aborted;
    return keepReading ? 1 : -1;
}

static int consumeResponseBody(void *ctx, const unsigned char *data, size_t len) {
    BodyDelivery *delivery = (BodyDelivery *) ctx;
    Stream *stream = delivery -> stream;
    ResponseSink *sink = stream -> sink;

    // Library-side collector: the decoded chunk accumulates into the stream
    // payload, which is what a buffered response used to hold.
    if (sink -> collecting) {
        const int buffered = bufferStreamPayload(stream, data, len);
        if (buffered < 0) {
            sink -> error = stream -> error;
            sink -> cancelled = 1;
            return 1;
        }
        delivery -> delivered += len;
        return 0;
    }

    if (sink -> contract.onData == NULL) { return 0; }
    const int abortRequested = sink -> contract.onData(sink -> contract.userData, data, len);
    if (abortRequested == 0) {
        delivery -> delivered += len;
        return 0;
    }

    const size_t consumed = sink -> delivered + delivery -> delivered + len;
    abortResponseSink(stream, sink, consumed);
    return 1;
}

static void abortResponseSink(Stream *stream, ResponseSink *sink, size_t delivered) {
    sink -> aborted = 1;
    sink -> cancelled = 1;
    sink -> error = ERR_RESPONSE_STREAM_ABORTED_BY_CONSUMER;
    stream -> error = sink -> error;
    LOG("INFO", "streaming response aborted by the consumer after %zu bytes", delivered);
}

// Idempotent: only the first caller ends the callback sequence.
void completeResponseSink(Stream *stream) {
    ResponseSink *sink = stream -> sink;
    if (sink == NULL || sink -> completed) { return; }
    sink -> completed = 1;

    // No further chunk can arrive: release the decoder before the callback.
    freeStreamDecompressor(sink -> decompressor);
    sink -> decompressor = NULL;

    const Error error = sink -> error.code != NULL ? sink -> error : stream -> error;
    if (sink -> contract.onComplete != NULL) {
        sink -> contract.onComplete(sink -> contract.userData, error);
    }
}

int isResponseStreaming(const Stream *stream) {
    return stream -> sink != NULL;
}

int isContractStreamed(const Basket *basket) {
    return basket -> sink != NULL && !basket -> sink -> collecting;
}

int takeResponseCancellation(Stream *stream) {
    ResponseSink *sink = stream -> sink;
    if (sink == NULL || !sink -> cancelled || sink -> cancellationTaken) { return 0; }
    sink -> cancellationTaken = 1;
    return 1;
}

void buildResponseDeadline(struct timespec *deadline, int timeoutInMilliseconds) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline -> tv_sec += timeoutInMilliseconds / 1000;
    deadline -> tv_nsec += (long) (timeoutInMilliseconds % 1000) * 1000000L;
    if (deadline -> tv_nsec >= 1000000000L) {
        deadline -> tv_sec += 1;
        deadline -> tv_nsec -= 1000000000L;
    }
}

int renewStreamDeadline(const Stream *stream, int timeoutInMilliseconds, struct timespec *deadline) {
    const int streaming = isResponseStreaming(stream);
    if (!streaming) { return 0; }

    const time_t now = time(NULL);
    const int stillActive = (now - stream -> lastActivityTime) * 1000 < timeoutInMilliseconds;
    if (stillActive) {
        deadline -> tv_sec = stream -> lastActivityTime + timeoutInMilliseconds / 1000;
        deadline -> tv_nsec = (long) (timeoutInMilliseconds % 1000) * 1000000L;
    }
    return stillActive;
}
