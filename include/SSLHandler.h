//
// Created by Intuition on 25-7-14.
//

#ifndef SSLHANDLER_H
#define SSLHANDLER_H

#include "Basket.h"

#include "openssl/ssl.h"

SSL_CTX* createSSLContext(Basket *basket);

SSL* createSSL(Basket *basket, SSL_CTX *ctx, int sockfd);

int configureSSLSettings(Basket *basket, SSL *ssl);

#endif //SSLHANDLER_H
