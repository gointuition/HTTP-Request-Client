//
// Created by Intuition on 26-8-23.
//

#ifndef HTTP11REQUESTHANDLER_H
#define HTTP11REQUESTHANDLER_H

#include "Basket.h"

// async=0: run the whole exchange inline; async=1: start it on a background
// thread and return immediately (poll stream->isEnded for completion).
void handleHttp11Request(Basket *basket, Stream *stream, int async);

void sendHttp11Request(Basket *basket, Stream *stream);

#endif //HTTP11REQUESTHANDLER_H
