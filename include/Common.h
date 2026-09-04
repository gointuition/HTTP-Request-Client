//
//  Common.h
//  HTTP
//  
//  Created by intuition on 2024/9/8.
//  Copyright © 2024. All rights reserved.
//  
    

#ifndef Common_h
#define Common_h

#include <stddef.h>
#include <stdio.h>

// Growable string buffer shared by request-building code paths

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} StrBuf;

int hash(const char *key, int capacity);

void sbReserve(StrBuf *sb, size_t extra);
void sbAppend(StrBuf *sb, const char *s, size_t n);
void sbAppendStr(StrBuf *sb, const char *s);

#endif /* Common_h */
