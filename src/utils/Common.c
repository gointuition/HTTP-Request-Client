//
//  Common.c
//  HTTP
//  
//  Created by intuition on 2024/9/8.
//  Copyright © 2024. All rights reserved.
//  
    

#include "Common.h"

#include <stdlib.h>
#include <string.h>

int hash(const char *key, int capacity) {
    int hashValue = 0;
    for (int i = 0; key[i] != '\0'; i++) {
        hashValue += key[i];
        hashValue = (hashValue * 31) % capacity;
    }
    return hashValue;
}

void sbReserve(StrBuf *sb, size_t extra) {
    if (sb -> failed) { return; }
    if (sb -> len + extra + 1 <= sb -> cap) { return; }
    size_t newCap = sb -> cap ? sb -> cap : 1024;
    while (newCap < sb -> len + extra + 1) { newCap *= 2; }
    char *newData = realloc(sb -> data, newCap);
    if (newData == NULL) {
        sb -> failed = 1;
        return;
    }
    sb -> data = newData;
    sb -> cap = newCap;
}

void sbAppend(StrBuf *sb, const char *s, size_t n) {
    sbReserve(sb, n);
    if (sb -> failed) { return; }
    memcpy(sb -> data + sb -> len, s, n);
    sb -> len += n;
}

void sbAppendStr(StrBuf *sb, const char *s) {
    sbAppend(sb, s, strlen(s));
}
