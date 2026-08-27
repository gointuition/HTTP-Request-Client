//
// Created by Intuition on 26-8-23.
//

#ifndef HTTP11RESPONSEHANDLER_H
#define HTTP11RESPONSEHANDLER_H

#include "Basket.h"

// Read and parse the full HTTP/1.1 response (status line, headers, body per
// RFC 9110 framing) into the stream before the deadline. Failure is reported
// via stream->error (the session is marked goingAway on connection errors).
void receiveHttp11Response(Basket *basket, Stream *stream, const struct timespec *deadline);

#endif //HTTP11RESPONSEHANDLER_H
