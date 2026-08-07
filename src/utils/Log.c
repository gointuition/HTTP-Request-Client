//
// Created by Intuition on 2026/7/3.
//

#include "Log.h"

_Thread_local bool G_LOG_ENABLED = false;

void setLogEnabled(bool enable) {
    G_LOG_ENABLED = enable;
}

bool getLogEnabled(void) {
    return G_LOG_ENABLED;
}