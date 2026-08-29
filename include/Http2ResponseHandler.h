//
// Created by Intuition on 26-4-7.
//

#ifndef HTTP2RESPONSEHANDLER_H
#define HTTP2RESPONSEHANDLER_H

#include <stdint.h>

#include "Basket.h"

// Process a single fully-buffered HTTP/2 frame from the connection reader
// thread. `stream` is the routed target stream, or NULL for connection-level
// frames (SETTINGS/WINDOW_UPDATE/GOAWAY) and for frames targeting an
// unknown/closed stream. HEADERS frames are always HPACK-decoded (into a
// scratch buffer when `stream` is NULL) to keep the shared dynamic table in
// sync. On END_STREAM/RST_STREAM the function sets `stream->isEnded`.
void handleStreamFrame(Session *session, Stream *stream,
                       unsigned char *payload, uint32_t length,
                       uint8_t type, uint8_t flags, uint32_t streamId);

// Move a completed stream's headers/payload into `basket->response` and run
// decompression. On a retryable connection error (GOAWAY/SETTINGS_TIMEOUT) the
// stream buffers are left intact for freeStreamBuffers so the caller can retry
// with a clean basket.
void finalizeStreamIntoBasket(Basket *basket, Stream *stream);

// Free a stream's accumulated response headers and payload buffer (no-op for
// fields already transferred to a basket).
void freeStreamBuffers(Stream *stream);

// ─── Connection reader thread ───
// Per-connection reader thread entry (arg = Session*): owns SSL_read,
// accumulates bytes, parses frame headers and routes each frame via
// readerDispatch. Exits when session->readerRunning is cleared.
void* readerLoop(void *arg);

// Stop the reader thread (no-op when it was never started).
void stopReader(Session *session);

// Wake up streams that will never complete. With onlyPending set, only
// streams that have not received any response yet are failed (graceful
// GOAWAY); otherwise every unfinished stream is failed (connection loss).
void failAllStreams(Session *session, int onlyPending);

#endif //HTTP2RESPONSEHANDLER_H
