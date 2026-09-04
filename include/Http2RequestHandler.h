//
// Created by Intuition on 26-4-7.
//

#ifndef HTTP2REQUESTHANDLER_H
#define HTTP2REQUESTHANDLER_H

#include "Basket.h"

#include "openssl/ssl.h"

int establishTransport(Basket *basket, SSL *ssl);

void sendHeadersFrame(Basket *basket);

void sendDataFrame(Basket *basket);

// Write len bytes to ssl. When nonBlocking is non-zero, the socket is driven
// with select() and SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE are retried up
// to timeoutInMillis. Returns 1 on success, -1 on failure.
int sslWriteAllEx(SSL *ssl, int nonBlocking, int timeoutInMillis, const unsigned char *buf, size_t len);

// Send a prebuilt control frame, serialized with the other writers of the
// session. Failures are logged only: a control frame never aborts a request.
void sendControlFrame(Session *session, const unsigned char *frame, size_t len);

// Flow-control credit for a streaming response body (streamId 0 = connection).
void sendWindowUpdateFrame(Session *session, uint32_t streamId, uint32_t increment);

// Cancel a stream the consumer stopped wanting (RST_STREAM / CANCEL).
void sendRSTStreamFrame(Session *session, uint32_t streamId);

// Reader-thread write queue: control frames produced while draining a read
// batch are appended as raw wire bytes and leave in one coalesced write at the
// batch end (flushReaderWrites), like a browser's write queue.
void queueControlFrame(Session *session, const unsigned char *frame, size_t len);

void queueWindowUpdateFrame(Session *session, uint32_t streamId, uint32_t increment);

void queueRSTStreamFrame(Session *session, uint32_t streamId);

void flushReaderWrites(Session *session);

#endif //HTTP2REQUESTHANDLER_H
