//
// Created by Intuition on 2026/7/3.
//

#ifndef HTTP2_LOG_H
#define HTTP2_LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

extern bool G_LOG_ENABLED;

#define FILE_NAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG_PREFIX_WIDTH 30
#define LOG_LEVEL_WIDTH 5

#define LOG(level, fmt, ...) \
    do { \
        if (G_LOG_ENABLED) { \
            char time_str[32]; \
            char prefix[64]; \
            /* 跨平台获取时间 */ \
            time_t now = time(NULL); \
            struct tm *tm_info = localtime(&now); \
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info); \
            snprintf(prefix, sizeof(prefix), "[%s:%d]", FILE_NAME, __LINE__); \
            printf("[%s] [%-*s] %-*s " fmt "\n", \
                   time_str, \
                   LOG_LEVEL_WIDTH, level, \
                   LOG_PREFIX_WIDTH, prefix, \
                   ##__VA_ARGS__); \
        } \
    } while (0)

void setLogEnabled(bool enable);

#endif //HTTP2_LOG_H
