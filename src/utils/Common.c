//
//  Common.c
//  HTTP2
//  
//  Created by intuition on 2024/9/8.
//  Copyright © 2024. All rights reserved.
//  
    

#include "Common.h"

int hash(const char *key, int capacity) {
    int hashValue = 0;
    for (int i = 0; key[i] != '\0'; i++) {
        hashValue += key[i];
        hashValue = (hashValue * 31) % capacity;
    }
    return hashValue;
}
