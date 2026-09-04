//
// Created by Intuition on 26-8-31.
//
// Streaming response: the body is decoded and handed to the caller chunk by
// chunk instead of being buffered in the basket. One funnel
// (appendStreamPayload) is shared by the HTTP/2 and HTTP/1.1 response readers.
//

#ifndef RESPONSESTREAM_H
#define RESPONSESTREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

#include "Basket.h"
#include "CompressHandler.h"

// ─── caller-owned contract ───
// The contract is all or nothing: all three callbacks must be given, since a
// missing onData would leave the body with neither a consumer nor a place in the
// basket. Passing NULL for the whole contract is how a caller asks for a
// buffered response; a partial one is rejected with
// ERR_RESPONSE_STREAM_INCOMPLETE_CONTRACT before the request is sent.
//
// Callbacks run on the thread that receives the bytes (the HTTP/2 connection
// reader thread, or the thread running the HTTP/1.1 exchange): they must not
// block for long and must not call back into this library. A request that
// fails before a stream is registered reports the error through
// handleResponse() only, without any callback.
typedef struct {
    // Response headers (":status" first), delivered once, before the body.
    void (*onHeaders)(void *userData, const ResponseHeader *headers, size_t numHeaders);
    // One decoded body chunk; returning non-zero stops the response.
    int  (*onData)(void *userData, const unsigned char *data, size_t len);
    // Delivered once per attempt; `error` is ERR_NONE when the body ended cleanly.
    void (*onComplete)(void *userData, Error error);
    void *userData;
} ResponseStream;

// ─── library-owned state behind a ResponseStream ───
struct ResponseSink {
    ResponseStream  contract;
    int             collecting;         // library-side collector: no caller contract
    int             decompressMask;     // snapshot of Basket.decompress
    ContentEncoding encoding;
    StreamDecompressor *decompressor;   // NULL = nothing to decode
    size_t          delivered;          // decoded bytes handed to the consumer
    Error           error;              // failure reported to onComplete
    int             headersDelivered;
    int             completed;
    int             aborted;            // the consumer asked to stop
    int             cancelled;          // the stream must be torn down (RST/close)
    int             cancellationTaken;  // cancellation already handed to the writer
};

// A NULL `stream` builds the library's own collector: the body still travels
// this funnel, but accumulates into the basket payload. NULL on allocation
// failure.
ResponseSink* buildResponseSink(Basket *basket, const ResponseStream *stream);
void freeResponseSink(ResponseSink *sink);

// 1 when every callback of the contract is given; 0 for a partial one, which no
// request may run with.
int isCompleteResponseStream(const ResponseStream *stream);

// A retried attempt replays the callback sequence from scratch.
void prepareStreamRetry(Basket *basket);

// ─── response reading hooks (called with the stream owned by the reader) ───
int appendStreamPayload(Stream *stream, const unsigned char *data, size_t n);
void deliverResponseHeaders(Stream *stream);
void completeResponseSink(Stream *stream);

int isResponseStreaming(const Stream *stream);
// 1 when the body went to the caller's callbacks instead of the basket payload.
int isContractStreamed(const Basket *basket);
// 1 the first time the teardown request is taken, so it is sent exactly once.
int takeResponseCancellation(Stream *stream);

// ─── reading wait ───
// Absolute CLOCK_REALTIME instant `timeoutInMilliseconds` from now.
void buildResponseDeadline(struct timespec *deadline, int timeoutInMilliseconds);
// A streaming response is bounded by its idle time: while body bytes keep
// arriving, the deadline moves forward. 1 when it was extended.
int renewStreamDeadline(const Stream *stream, int timeoutInMilliseconds, struct timespec *deadline);

#ifdef __cplusplus
}
#endif

#endif //RESPONSESTREAM_H
