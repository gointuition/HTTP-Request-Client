//
// Created by Intuition on 25-7-11.
//

#include "Error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// System errors
const Error ERR_NONE                                                = { NULL, NULL };

const Error ERR_SYSTEM_MEMORY_ALLOCATION_FAILED                     = { "0-0000", "System memory allocation failed."};
const Error ERR_SYSTEM_SHARED_MEMORY_NOT_EXIT                       = { "0-0001", "Shared memory may not exist (Daemon)."};
const Error ERR_SYSTEM_SHARED_MEMORY_MMAP_FAILED                    = { "0-0002", "Shared memory mmap failed."};

// Session errors
const Error ERR_SESSION_SSL_CONNECT_FAILED                          = { "1-0001", "SSL_connect failed."};
const Error ERR_SESSION_SSL_ALPN_NEGOTIATION_FAILED                 = { "1-0002", "SSL ALPN negotiation failed."};
const Error ERR_SESSION_MAGIC_INCORRECT                             = { "1-0003", "Session magic incorrect."};
const Error ERR_SESSION_INACTIVE                                    = { "1-0004", "Session is inactive."};
const Error ERR_SESSION_HOST_RESOLVE_FAILED                         = { "1-0005", "Session host resolve failed."};
const Error ERR_SESSION_SOCKET_CREATION_FAILED                      = { "1-0006", "Session socket creation failed."};
const Error ERR_SESSION_SOCKET_NONBLOCK_SETTING_FAILED              = { "1-0007", "Failed to set session socket non-block."};
const Error ERR_SESSION_SOCKET_CONNECTING_FAILED                    = { "1-0008", "Session socket connecting failed."};
const Error ERR_SESSION_SOCKET_INVALID_FILE_DESCRIPTOR              = { "1-0009", "Session socket invalid file descriptor."};
const Error ERR_SESSION_SOCKET_CONNECTING_TIMEOUT                   = { "1-0010", "Session socket connecting timeout."};
const Error ERR_SESSION_SOCKET_NO_AVAILABLE_ADDRESS                 = { "1-0011", "Session socket no available address."};
const Error ERR_SESSION_SOCKET_CONNECTING_UNKNOWN_ERROR             = { "1-0012", "Session socket connecting unknown error."};
const Error ERR_SESSION_SSL_CTX_CREATION_FAILED                     = { "1-0013", "Ssl context creation failed."};
const Error ERR_SESSION_SSL_OBJECT_CREATION_FAILED                  = { "1-0014", "Ssl object creation failed."};
const Error ERR_SESSION_SSL_FAILED_TO_ASSOCIATE_SOCKET              = { "1-0015", "Ssl failed to associate with a socket."};
const Error ERR_SESSION_FAILED_TO_CONNECT_TO_SHARED_MEMORY_POOL     = { "1-0016", "Failed to connect to shared session memory pool."};
const Error ERR_SESSION_SOCKET_CONNECTING_REFUSED                   = { "1-0017", "Session socket connection refused."};
const Error ERR_SESSION_SETTINGS_TIMEOUT                            = { "1-0018", "Session settings timeout."};
const Error ERR_SESSION_GO_AWAY                                     = { "1-0019", "Session goes away without error."};

// Request errors
const Error ERR_REQUEST_PARSING_STRING_TO_JSON_FAILED               = { "2-0001", "Parsing request string to json failed."};
const Error ERR_REQUEST_MISSING_HEADERS                             = { "2-0002", "Missing request headers."};
const Error ERR_REQUEST_PARSING_URL_FAILED                          = { "2-0003", "Parsing request URL failed."};
const Error ERR_REQUEST_PARSING_METHOD_FAILED                       = { "2-0004", "Parsing request method failed."};
const Error ERR_REQUEST_PARSING_USERAGENT_FAILED                    = { "2-0005", "Parsing request header user-agent failed."};
const Error ERR_REQUEST_PARSING_CONTENTLENGTH_FAILED                = { "2-0006", "Parsing request header content-length failed."};
const Error ERR_REQUEST_INCORRECT_CONTENTLENGTH_TYPE                = { "2-0007", "The value type of request header content-length must be string."};
const Error ERR_REQUEST_UNSUPPORTED_METHOD                          = { "2-0008", "Unsupported method, POST and GET supported only."};
const Error ERR_REQUEST_UNSUPPORTED_USERAGENT                       = { "2-0009", "Unsupported user-agent, Google Chrome supported only."};
const Error ERR_REQUEST_SENDING_HTTP2_HEADERS_FRAME_FAILED          = { "2-0010", "Sending http/2 headers frame failed."};
const Error ERR_REQUEST_SENDING_HTTP2_DATA_FRAME_FAILED             = { "2-0011", "Sending http/2 data frame failed."};
const Error ERR_REQUEST_SENDING_HTTP2_PREFACE_FRAME_FAILED          = { "2-0012", "Sending http/2 preface frame failed."};
const Error ERR_REQUEST_SENDING_HTTP2_SETTINGS_FRAME_FAILED         = { "2-0013", "Sending http/2 settings frame failed."};
const Error ERR_REQUEST_SENDING_HTTP2_WINDOW_UPDATE_FRAME_FAILED    = { "2-0014", "Sending http/2 window update frame failed."};
const Error ERR_REQUEST_UNKNOWN_PSEUDO_HEADER                       = { "2-0015", "Unknown pseudo header name."};

// Response errors
const Error ERR_RESPONSE_READING_CONNECTION_ERROR                   = { "3-0001", "Response reading connection error."};
const Error ERR_RESPONSE_READING_UNKNOWN_ERROR                      = { "3-0002", "Response reading unknown error."};
const Error ERR_RESPONSE_NO_CONTENT_AFTER_READING_TIMEOUT           = { "3-0003", "Response reading timeout, no content received."};
const Error ERR_RESPONSE_PARTIAL_CONTENT_AFTER_READING_TIMEOUT      = { "3-0004", "Response reading timeout, partial content received."};
const Error ERR_RESPONSE_DECODING_HEADERS_FRAME_FAILED              = { "3-0005", "Decoding response HEADERS frame failed."};
const Error ERR_RESPONSE_DECODING_GOAWAY_FRAME_FAILED               = { "3-0006", "Decoding response GOAWAY frame failed."};
const Error ERR_RESPONSE_GZIP_INFLATE_FAILED                        = { "3-0007", "Response (gzip) Inflate failed."};
const Error ERR_RESPONSE_BROTLI_INFLATE_FAILED                      = { "3-0008", "Response (brotli) Inflate failed."};
const Error ERR_RESPONSE_DEFLATE_INFLATE_FAILED                     = { "3-0009", "Response (deflate) Inflate failed."};
const Error ERR_RESPONSE_ZSTD_INFLATE_FAILED                        = { "3-0010", "Response (zstd) Inflate failed."};
const Error ERR_RESPONSE_INFLATE_UNKNOWN_ERROR                      = { "3-0011", "Response inflate unknown error."};
const Error ERR_RESPONSE_RST_STREAM_ERROR                           = { "3-0012", "Response RST_STREAM unknown error."};

// Proxy errors
const Error ERR_PROXY_HOST_RESOLVE_FAILED                           = { "4-0001", "Proxy host resolve failed."};
const Error ERR_PROXY_SOCKET_CREATION_FAILED                        = { "4-0002", "Proxy socket creation failed."};
const Error ERR_PROXY_SOCKET_NONBLOCK_SETTING_FAILED                = { "4-0003", "Failed to set proxy socket non-block."};
const Error ERR_PROXY_SOCKET_CONNECTING_FAILED                      = { "4-0004", "Proxy socket connecting failed."};
const Error ERR_PROXY_SOCKET_INVALID_FILE_DESCRIPTOR                = { "4-0005", "Proxy socket invalid file descriptor."};
const Error ERR_PROXY_SOCKET_CONNECTING_TIMEOUT                     = { "4-0006", "Proxy socket connecting timeout."};
const Error ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR               = { "4-0007", "Proxy socket connecting unknown error."};
const Error ERR_PROXY_SEND_CONNECT_REQUEST_FAILED                   = { "4-0008", "Proxy send connect request failed."};
const Error ERR_PROXY_AUTHORIZATION_FAILED                          = { "4-0009", "Proxy authorization failed (407)."};
const Error ERR_PROXY_UNEXPECTED_RESPONSE                           = { "4-0010", "Proxy unexpected CONNECT response."};
const Error ERR_PROXY_SOCKET_CONNECTING_REFUSED                     = { "4-0011", "Proxy socket connection refused."};
