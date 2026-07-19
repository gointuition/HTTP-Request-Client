//
// Created by Intuition on 25-6-29.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "Compat.h"

#include "UrlParser.h"
#include "Log.h"

#define MAX_URL_LENGTH 2048

int parseUrl(const char *url, URLComponents *components) {
    if (!url || !components) {
        return -1;
    }

    if (!validateUrl(url)) {
        return -2;
    }

    // initialise components
    memset(components, 0, sizeof(URLComponents));

    char urlCopy[MAX_URL_LENGTH];
    strncpy(urlCopy, url, MAX_URL_LENGTH - 1);
    urlCopy[MAX_URL_LENGTH - 1] = '\0';

    char *current = urlCopy;

    // parse scheme
    parseUrlScheme(&current, components);
    // parse authority
    parseUrlAuthority(&current, components);
    // parse path
    parseUrlPath(&current, components);
    // parse query
    parseUrlQuery(&current, components);
    // parse fragment
    parseUrlFragment(&current, components);

    return 0;
}

bool validateUrl(const char* url) {
    if (!url || strlen(url) == 0) {
        return false;
    }
    if (!strstr(url, "://")) {
        return false;
    }
    for (int i = 0; url[i]; i++) {
        if (!isValidUrlChar(url[i])) {
            return false;
        }
    }
    return true;
}

bool isValidUrlChar(char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
           c == '!' || c == '*' || c == '\'' || c == '(' || c == ')' ||
           c == ';' || c == ':' || c == '@' || c == '&' || c == '=' ||
           c == '+' || c == '$' || c == ',' || c == '/' || c == '?' ||
           c == '#' || c == '[' || c == ']' || c == '%';
}

void parseUrlScheme(char **current, URLComponents *components) {
    // components -> scheme uses fixed array length, no need to free
    char *schemeEnd = strstr(*current, "://");
    if (schemeEnd) {
        int schemeLen = schemeEnd - *current;
        strncpy(components -> scheme, *current, schemeLen);
        components -> scheme[schemeLen] = '\0';
        *current = schemeEnd + 3; // skip ://
    }
}

void parseUrlAuthority(char **current, URLComponents *components) {
    // user:pass@host:port

    char *authorityEnd = strchr(*current, '/');
    if (!authorityEnd) {
        authorityEnd = strchr(*current, '?');
    }
    if (!authorityEnd) {
        authorityEnd = strchr(*current, '#');
    }

    char authority[256] = { 0 };
    if (authorityEnd) {
        int authLen = authorityEnd - *current;
        strncpy(authority, *current, authLen);
        authority[authLen] = '\0';
        *current = authorityEnd;
    } else {
        strcpy(authority, *current);
        *current += strlen(*current);
    }

    // parse username and password
    char *atSign = strchr(*current, '@');
    if (atSign) {
        char *colon = strchr(authority, ':');
        if (colon && colon < atSign) {
            int userLen = colon - authority;
            strncpy(components -> username, authority, userLen);
            components -> username[userLen] = '\0';
            int passLen = atSign - colon - 1;
            strncpy(components -> password, colon + 1, passLen);
            components -> password[passLen] = '\0';
        } else {
            int userLen = atSign - authority;
            strncpy(components -> username, authority, userLen);
            components -> username[userLen] = '\0';
        }
        memmove(authority, atSign + 1, strlen(atSign + 1) + 1);
    }

    // parse host and port
    char *portColon = strchr(authority, ':');
    if (portColon) {
        int hostLen = portColon - authority;
        strncpy(components -> host, authority, hostLen);
        components -> host[hostLen] = '\0';
        strcpy(components -> port, portColon + 1);
    } else {
        strcpy(components -> host, authority);
        if (strcmp(components -> scheme, "https") == 0) {
            strcpy(components -> port, "443");
        } else if (strcmp(components -> scheme, "http") == 0) {
            strcpy(components -> port, "80");
        } else if (strcmp(components -> scheme, "ftp") == 0) {
            strcpy(components -> port, "21");
        } else if (strcmp(components -> scheme, "ssh") == 0) {
            strcpy(components -> port, "22");
        }
    }
}

void parseUrlPath(char **current, URLComponents *components) {
    if (**current == '/') {
        char *pathEnd = strchr(*current, '?');
        if (!pathEnd) {
            pathEnd = strchr(*current, '#');
        }
        if (pathEnd) {
            int pathLen = pathEnd - *current;
            strncpy(components -> path, *current, pathLen);
            components -> path[pathLen] = '\0';
            *current = pathEnd;
        } else {
            strcpy(components -> path, *current);
            *current += strlen(*current);
        }
    }
}

void parseUrlQuery(char **current, URLComponents *components) {
    if (**current == '?') {
        (*current)++;
        char *queryEnd = strchr(*current, '#');
        if (queryEnd) {
            int queryLen = queryEnd - *current;
            strncpy(components -> query, *current, queryLen);
            components -> query[queryLen] = '\0';
            *current = queryEnd;
        } else {
            strcpy(components -> query, *current);
            *current += strlen(*current);
        }
    }
}

void parseUrlFragment(char **current, URLComponents *components) {
    if (**current == '#') {
        (*current)++;
        strcpy(components -> fragment, *current);
    }
}

void printUrlComponents(const URLComponents *components) {
    LOG("DEBUG", "=== URL Components ===");
    LOG("DEBUG", "Scheme: %s", components -> scheme);
    LOG("DEBUG", "Host: %s", components->host);
    LOG("DEBUG", "Port: %s", components->port);
    LOG("DEBUG", "Path: %s", components->path);
    LOG("DEBUG", "Query: %s", components->query);
    LOG("DEBUG", "Fragment: %s", components->fragment);
    if (strlen(components->username) > 0) {
        LOG("DEBUG", "Username: %s", components->username);
    }
    if (strlen(components->password) > 0) {
        LOG("DEBUG", "Password: %s", components->password);
    }
    LOG("DEBUG", "=====================");
}

char* getHeaderAuthority(const char *host, const char *port) {
    const size_t hostLen = strlen(host);
    const size_t portLen = (port && strlen(port) > 0) ? strlen(port) + 1 : 0;
    size_t totalLen = hostLen + portLen + 1;

    struct in_addr addr4;
    struct in6_addr addr6;
    if (inet_pton(AF_INET, host, &addr4) ==  1) {
        char *authority = malloc(totalLen + 1);
        if (authority == NULL) {
            return NULL;
        }
        snprintf(authority, totalLen, "%s:%s", host, port);
        return authority;
    }
    if (inet_pton(AF_INET6, host, &addr6) == 1) {
        char *authority = malloc(totalLen + 1);
        if (authority == NULL) {
            return NULL;
        }
        snprintf(authority, totalLen, "[%s]:%s", host, port);
        return authority;
    }
    return strdup(host);
}
