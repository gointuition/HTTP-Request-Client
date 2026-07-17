//
// Created by Intuition on 25-7-6.
//

#ifndef SOCKETHANDLER_H
#define SOCKETHANDLER_H

#include "Basket.h"

int createSocketThroughProxy(Basket * basket);

int createSocket(Basket *basket, const char* host, const char* port, int isProxy);

#endif //SOCKETHANDLER_H
