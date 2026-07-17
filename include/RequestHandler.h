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

#endif //REQUESTHANDLER_H
