//
// Created by Intuition on 25-6-29.
//

#ifndef URLPARSER_H
#define URLPARSER_H

#include <stdbool.h>

// #define MAX_URL_LENGTH 2048
// #define MAX_COMPONENT_LENGTH 512
// #define MAX_PARAMS 20
// #define MAX_PATH_SEGMENTS 50

typedef struct {
    char scheme[16];
    char host[256];
    char port[8];
    char path[2048];
    char query[2048];
    char fragment[2048];
    char username[64];
    char password[256];
} URLComponents;

typedef struct {
	char key[32];
	char value[256];
} QueryParam;

typedef struct {
	QueryParam* params[20];
	int count;
} QueryParams;

typedef struct {
	char segments[20][2048];
	int count;
} PathSegments;

int parseUrl(const char *url, URLComponents *components);
void parseUrlScheme(char **current, URLComponents *components);
void parseUrlAuthority(char **current, URLComponents *components);
void parseUrlPath(char **current, URLComponents *components);
void parseUrlQuery(char **current, URLComponents *components);
void parseUrlFragment(char **current, URLComponents *components);
QueryParams parseQueryParams(const char* queryString);
PathSegments parsePathSegments(const char* path);

bool validateUrl(const char* url);
// void urlEncode(const char* input, char* output);
// void urlDecode(const char* encoded, char* decoded);
// const char* getQueryParam(const QueryParams *params, const char *key);

void printUrlComponents(const URLComponents *components);
void printQueryParams(const QueryParams *params);
void printPathSegments(const PathSegments *segments);

bool isValidUrlChar(char c);

char* getHeaderAuthority(const char *host, const char *port);

#endif //URLPARSER_H
