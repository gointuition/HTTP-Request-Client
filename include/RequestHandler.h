//
// Created by Intuition on 26-4-7.
//

#ifndef REQUESTHANDLER_H
#define REQUESTHANDLER_H

#include "Basket.h"

#include "openssl/ssl.h"

int establishTransport(Basket *basket, SSL *ssl);

void sendHeadersFrame(Basket *basket);

void sendDataFrame(Basket *basket);

// Write len bytes to ssl. When nonBlocking is non-zero, the socket is driven
// with select() and SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE are retried up
// to timeoutInMillis. Returns 1 on success, -1 on failure.
int sslWriteAllEx(SSL *ssl, int nonBlocking, int timeoutInMillis, const unsigned char *buf, size_t len);

#endif //REQUESTHANDLER_H
