//
//  Version.c
//  HTTP
//
//  Runtime version query implementation for libhttpclient.
//

#include "Version.h"

const char* httpclient_version(void) {
    return HTTPCLIENT_VERSION_STRING;
}

int httpclient_version_major(void) {
    return HTTPCLIENT_VERSION_MAJOR;
}

int httpclient_version_minor(void) {
    return HTTPCLIENT_VERSION_MINOR;
}

int httpclient_version_patch(void) {
    return HTTPCLIENT_VERSION_PATCH;
}
