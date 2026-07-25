//
//  Version.c
//  HTTP2
//
//  Runtime version query implementation for libhttp2client.
//

#include "Version.h"

const char* http2client_version(void) {
    return HTTP2CLIENT_VERSION_STRING;
}

int http2client_version_major(void) {
    return HTTP2CLIENT_VERSION_MAJOR;
}

int http2client_version_minor(void) {
    return HTTP2CLIENT_VERSION_MINOR;
}

int http2client_version_patch(void) {
    return HTTP2CLIENT_VERSION_PATCH;
}
