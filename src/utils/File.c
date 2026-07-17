//
// Created by Intuition on 2026/6/26.
//

#include "File.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int shouldRemoveLine(const char *line) {
    while (isspace((unsigned char)*line)) line++;
    if (*line == '\0') return 1;
    if (line[0] == '/' && line[1] == '/') return 1;
    return 0;
}
char* readLine(FILE *fp) {
    int ch;
    size_t len = 0, cap = 128;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while ((ch = fgetc(fp)) != EOF && ch != '\n' && ch != '\r') {
        if (len + 1 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) { free(buf); return NULL; }
            buf = new_buf;
        }
        buf[len++] = ch;
    }

    if (ch == '\r') {
        int next = fgetc(fp);
        if (next != '\n' && next != EOF) ungetc(next, fp);
    }

    if (len == 0 && ch == EOF) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}
char* readFromFile(const char *filePath) {
    FILE *fp = fopen(filePath, "r");
    if (!fp) {
        printf("Failed to open file\n");
        return NULL;
    }

    size_t capacity = 1024;
    size_t length = 0;
    char *result = malloc(capacity);
    if (!result) {
        fclose(fp);
        return NULL;
    }
    result[0] = '\0';

    char *line;
    while ((line = readLine(fp)) != NULL) {
        if (!shouldRemoveLine(line)) {
            size_t line_len = strlen(line);
            if (length + line_len + 2 > capacity) {
                while (length + line_len + 2 > capacity) {
                    capacity *= 2;
                }
                char *new_result = realloc(result, capacity);
                if (!new_result) {
                    free(result);
                    free(line);
                    fclose(fp);
                    return NULL;
                }
                result = new_result;
            }

            strcpy(result + length, line);
            length += line_len;
            result[length++] = '\n';
            result[length] = '\0';
        }
        free(line);
    }

    fclose(fp);
    return result;
}