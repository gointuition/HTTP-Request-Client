//
// Created by Intuition on 25-11-1.
//

#include "Basket.h"

#include <string.h>
#include <strings.h>

#include "Log.h"

#include "jansson.h"

static void initBasket(Basket * basket);
static void buildHttp2Headers(Basket *basket, json_t *jsonHeaders);
static size_t processCookies(RequestHeader *headers, size_t idx, const json_t *cookie, const int calculateCookieCount);

Basket* buildBasket(const char *requestString) {
//    printf("%s\n", requestString);

    Basket *basket = malloc(sizeof(Basket));
    if (basket == NULL) {
        return NULL;
    }

    initBasket(basket);

    json_error_t error;
    json_t *jsonRequest = json_loads(requestString, 0, &error);
    if (jsonRequest == NULL) {
//        setLogEnabled(true);
        LOG("ERROR", "failed to parse request string %s", error.text);
        basket -> error = ERR_REQUEST_PARSING_STRING_TO_JSON_FAILED;
    }

    if (basket -> error.code == NULL) {
        json_t *log = json_object_get(jsonRequest, "log");
        if (log != NULL && json_integer_value(log) == 1) {
            setLogEnabled(true);
        }
    }

    // parse url
    if (basket -> error.code == NULL) {
        // basket -> request = (Request *) malloc(sizeof(Request));
        basket -> url = strdup(json_string_value(json_object_get(jsonRequest, "url")));
        if (parseUrl(basket -> url, &(basket -> request.urlComponents)) != 0) {
            LOG("ERROR", "parsing url failed: %s", basket -> url);
            basket -> error = ERR_REQUEST_PARSING_URL_FAILED;
        } else {
            printUrlComponents(&(basket -> request.urlComponents));
        }
    }

    // parse method
    if (basket -> error.code == NULL) {
        const json_t *jsonMethod = json_object_get(jsonRequest, "method");
        if (jsonMethod == NULL) {
            LOG("ERROR", "parsing url failed: %s", basket -> url);
            basket -> error = ERR_REQUEST_PARSING_METHOD_FAILED;
        } else {
            const char *method = json_string_value(jsonMethod);
            if (strcasecmp(method, HTTP_METHOD_POST) == 0) {
                basket -> method = HTTP_METHOD_POST;
            } else if (strcasecmp(method, HTTP_METHOD_GET) == 0) {
                basket -> method = HTTP_METHOD_GET;
            } else {
                LOG("ERROR", "unsupported method: %s", method);
                basket -> error = ERR_REQUEST_UNSUPPORTED_METHOD;
            }
//            basket -> method = strdup(json_string_value(jsonMethod));
//            if (strcasecmp(basket -> method, "POST") != 0 && strcasecmp(basket -> method, "GET") != 0) {
//            }
        }
    }

    // parse headers
    if (basket -> error.code == NULL) {
        json_t *jsonHeaders = json_object_get(jsonRequest, "headers");
        if (jsonHeaders == NULL) {
            LOG("ERROR", "missing request headers");
            basket -> error = ERR_REQUEST_MISSING_HEADERS;
        } else {
            const json_t *jsonUA = json_object_get(jsonHeaders, "user-agent");
            if (jsonUA == NULL) {
                LOG("ERROR", "missing header user-agent");
                basket -> error = ERR_REQUEST_PARSING_USERAGENT_FAILED;
            } else {
                basket -> browserType = detectBrowseType(json_string_value(jsonUA));
                // TODO
                if (basket -> browserType != BROWSER_CHROME) {
                    LOG("ERROR", "unsupported user-agent: %s", json_string_value(jsonUA));
                    basket -> error = ERR_REQUEST_UNSUPPORTED_USERAGENT;
                }

                // compose headers
                buildHttp2Headers(basket, jsonHeaders);
            }
        }
    }

    // parse payload
    if (basket -> error.code == NULL) {
        const json_t *jsonPayload = json_object_get(jsonRequest, "payload");
        if (jsonPayload != NULL) {
            // TODO JSON_INDENT, JSON_ENSURE_ASCII, JSON_SORT_KEYS, JSON_PRESERVE_ORDER, JSON_ENCODE_ANY
            basket -> request.payload = json_dumps(jsonPayload, JSON_COMPACT);
            if (basket -> request.containsContentLength != 1) {
                LOG("ERROR", "missing header content-length");
                basket -> error = ERR_REQUEST_PARSING_CONTENTLENGTH_FAILED;
            }
        } else {
            basket -> request.payload = NULL;
        }
    }

    const json_t *connectTimeoutInMilliseconds = json_object_get(jsonRequest, "connectTimeoutInMilliseconds");
    if (connectTimeoutInMilliseconds != NULL) {
        basket -> connectTimeoutInMilliseconds = json_integer_value(connectTimeoutInMilliseconds);
    }
    const json_t *responseReadingTimeoutInMilliseconds = json_object_get(jsonRequest, "responseReadingTimeoutInMilliseconds");
    if (connectTimeoutInMilliseconds != NULL) {
        basket -> responseReadingTimeoutInMilliseconds = json_integer_value(responseReadingTimeoutInMilliseconds);
    }
    json_t *decompress = json_object_get(jsonRequest, "decompress");
    if (decompress != NULL) {
        basket -> decompress = json_integer_value(decompress);
    }

    if (basket -> error.code == NULL) {
        json_t *jsonProxy = json_object_get(jsonRequest, "proxy");
        if (jsonProxy != NULL) {
            strncpy(basket -> proxy.scheme, json_string_value(json_object_get(jsonProxy, "scheme")), sizeof(basket -> proxy.scheme) - 1);
            basket -> proxy.scheme[sizeof(basket -> proxy.scheme) - 1] = '\0';
            strncpy(basket -> proxy.host, json_string_value(json_object_get(jsonProxy, "host")), sizeof(basket -> proxy.host) - 1);
            basket -> proxy.host[sizeof(basket -> proxy.host) - 1] = '\0';
            strncpy(basket -> proxy.port, json_string_value(json_object_get(jsonProxy, "port")), sizeof(basket -> proxy.port) - 1);
            basket -> proxy.port[sizeof(basket -> proxy.port) - 1] = '\0';
            const json_t *authorization = json_object_get(jsonProxy, "authorization");
            if (authorization != NULL) {
                strncpy(basket -> proxy.authorization, json_string_value(json_object_get(jsonProxy, "authorization")), sizeof(basket -> proxy.authorization) - 1);
                basket -> proxy.authorization[sizeof(basket -> proxy.authorization) - 1] = '\0';
            }
        }
    }

    if (basket -> error.code == NULL) {
        json_t *jsonSession = json_object_get(jsonRequest, "session");
        if (jsonSession != NULL) {
            const json_t *expirationInMilliseconds = json_object_get(jsonSession, "expirationInMilliseconds");
            if (expirationInMilliseconds != NULL) {
                basket -> sessionExpirationInMilliseconds = json_integer_value(expirationInMilliseconds);
            }
        }
    }

    if (jsonRequest != NULL) {
        json_decref(jsonRequest);
    }

    return basket;
}

void initBasket(Basket * basket) {
    basket -> url = NULL;
    basket -> method = NULL;

    basket -> browserType = BROWSER_UNKNOWN;

    basket -> request.payload = NULL;
    basket -> request.containsContentLength = 0;
    basket -> request.headers = NULL;
    basket -> request.numHeaders = 0;
    basket -> request.urlComponents = (URLComponents) { 0, 0, 0, 0, 0 };

    basket -> response = (Response) { 0, 0,NULL, NULL };

    basket -> proxy = (Proxy) { 0, 0, 0, 0 };

    basket -> session = NULL;

    basket -> error = ERR_NONE;

    basket -> sessionExpirationInMilliseconds = 15000;
    basket -> connectTimeoutInMilliseconds = 5000;
    basket -> responseReadingTimeoutInMilliseconds = 20000;

    basket -> decompress = 1;
}

static void buildHttp2Headers(Basket *basket, json_t *jsonHeaders) {
    size_t idx = 0;

    const size_t cookieCount = processCookies(NULL, 0, json_object_get(jsonHeaders, "cookie"), 1);
    // +1: content-length ?
    // +4: pseudo headers
    basket -> request.headers = (RequestHeader *) malloc(sizeof(RequestHeader) * (json_object_size(jsonHeaders) + 1 + 4 + cookieCount));

    int containPseudoHeaders = 1;
    const json_t *pseudoMethod = json_object_get(jsonHeaders, ":method");
    const json_t *pseudoAuthority = json_object_get(jsonHeaders, ":authority");
    const json_t *pseudoScheme = json_object_get(jsonHeaders, ":scheme");
    const json_t *pseudoPath = json_object_get(jsonHeaders, ":path");
    if (pseudoMethod == NULL || pseudoAuthority == NULL || pseudoScheme == NULL || pseudoPath == NULL) {
        containPseudoHeaders = 0;
        if (basket -> browserType == BROWSER_CHROME) {
            basket -> request.headers[idx++] = (RequestHeader) { ":method", strdup(basket -> method), 1, 0, 1 };
            basket -> request.headers[idx++] = (RequestHeader) { ":authority",getHeaderAuthority(basket -> request.urlComponents.host, basket -> request.urlComponents.port), 1, 0, 1 };
            basket -> request.headers[idx++] = (RequestHeader) { ":scheme", basket -> request.urlComponents.scheme, 1, 0, 0 };
            basket -> request.headers[idx++] = (RequestHeader) { ":path", basket -> request.urlComponents.path, 1, 0, 0 };
        } else {
            // TODO other browsers, clients
            basket -> request.headers[idx++] = (RequestHeader) { ":method", strdup(basket -> method), 1, 0, 1 };
            basket -> request.headers[idx++] = (RequestHeader) { ":authority",getHeaderAuthority(basket -> request.urlComponents.host, basket -> request.urlComponents.port), 1, 0, 1 };
            basket -> request.headers[idx++] = (RequestHeader) { ":scheme", basket -> request.urlComponents.scheme, 1, 0, 0 };
            basket -> request.headers[idx++] = (RequestHeader) { ":path", basket -> request.urlComponents.path, 1, 0, 0 };
        }
    }

    basket -> request.containsContentLength = 0;

    void *iter = json_object_iter(jsonHeaders);
    while (iter) {
        const char *key = json_object_iter_key(iter);

        if (containPseudoHeaders == 1) {
            const json_t *value = json_object_iter_value(iter);
            if (value != NULL) {
                const int isPseudo = strcasecmp(":method", key) == 0 || strcasecmp(":authority", key) == 0 || strcasecmp(":scheme", key) == 0 || strcasecmp(":path", key) == 0 ? 1 : 0;
                basket -> request.headers[idx++] = (RequestHeader) { strdup(key), strdup(json_string_value(value)), isPseudo, 1, 1 };
            }
        } else {
            if (strcasecmp(":method", key) != 0
                && strcasecmp(":authority", key) != 0
                && strcasecmp(":scheme", key) != 0
                && strcasecmp(":path", key) != 0
                && strcasecmp("host", key) != 0
                && strcasecmp("connection", key) != 0
            ) {
                const json_t *value = json_object_iter_value(iter);
                if (value != NULL) {
                    if (strcasecmp("cookie", key) == 0) {
                        // one header max size not more than 4KB, total headers 8KB
                        idx = processCookies(basket -> request.headers, idx, value, 0);
                    } else {
                        if (strcasecmp("content-length", key) == 0) {
                            basket -> request.containsContentLength = 1;
                            int valueType = json_typeof(value);
                            if (valueType != JSON_STRING) {
                                LOG("ERROR", "incorrect value type of the header content-length");
                                basket -> error = ERR_REQUEST_INCORRECT_CONTENTLENGTH_TYPE;
                                break;
                            }
                        }
                        basket -> request.headers[idx++] = (RequestHeader) { strdup(key), strdup(json_string_value(value)), 0, 1, 1 };
                    }
                }
            }
        }

        iter = json_object_iter_next(jsonHeaders, iter);
    }

    basket -> request.numHeaders = idx;
}

static size_t processCookies(RequestHeader *headers, size_t idx, const json_t *cookie, const int calculateCookieCount) {
    if (cookie == NULL) { return 0; }

    const char *fullCookie = json_string_value(cookie);

    const char *p = fullCookie;
    while (*p) {
        const char *tokenStart = p;
        const char *tokenEnd = strchr(p, ';');
        if (tokenEnd == NULL) {
            tokenEnd = fullCookie + strlen(fullCookie);
        }
        // trim leading spaces
        while (tokenStart < tokenEnd && *tokenStart == ' ') { tokenStart++; }
        // trim trailing spaces
        const char *trimmedEnd = tokenEnd;
        while (trimmedEnd > tokenStart && *(trimmedEnd - 1) == ' ') { trimmedEnd--; }
        // find equal '=' from tokenStart to trimmedEnd
        const char *eq = NULL;
        for (const char *q = tokenStart; q < trimmedEnd; q++) {
            if (*q == '=') { eq = q; break;}
        }
        if (eq != NULL && eq + 1 <= trimmedEnd) {
            size_t nameLen = (size_t) (eq - tokenStart);
            const char *valueStart = eq + 1;
            size_t valueLen = (size_t) (trimmedEnd - valueStart);

            // TODO other browsers
            if (valueLen <= HEADER_VALUE_MAX_LENGTH_CHROME) {
                if (calculateCookieCount == 1) {
                    idx++;
                } else {
                    size_t kvLen = nameLen + 1 +  valueLen;
                    char *kv = malloc(kvLen + 1);
                    if (kv) {
                        memcpy(kv, tokenStart, nameLen);
                        kv[nameLen] = '=';
                        memcpy(kv + nameLen + 1, valueStart, valueLen);
                        kv[kvLen] = '\0';
                        headers[idx++] = (RequestHeader) { "cookie", kv, 0, 0, 1 };
                    }
                }
            } else {
                // split value into chunks
                size_t offset = 0;
                while (offset < valueLen) {
                    size_t chunkLen = valueLen - offset;
                    // TODO other browsers
                    if (chunkLen > HEADER_VALUE_MAX_LENGTH_CHROME) { chunkLen = HEADER_VALUE_MAX_LENGTH_CHROME; }

                    if (calculateCookieCount == 1) {
                        idx++;
                    } else {
                        size_t kvLen = nameLen + 1 +  chunkLen;
                        char *kv = malloc(kvLen + 1);
                        if (kv) {
                            memcpy(kv, tokenStart, nameLen);
                            kv[nameLen] = '=';
                            memcpy(kv + nameLen + 1, valueStart + offset, chunkLen);
                            kv[kvLen] = '\0';
                            headers[idx++] = (RequestHeader) { "cookie", kv, 0, 0, 1 };
                        }
                    }

                    offset += chunkLen;
                }
            }
        }

        // +1 is to skip ';'
        p = *tokenEnd == '\0' ? tokenEnd : tokenEnd + 1;
    }
    return idx;
}

void freeBasket(Basket *basket) {
    if (basket == NULL) {
        return;
    }
    // free url
    if (basket -> url != NULL) {
        LOG("DEBUG", "free basket -> url");
        free((void *) basket -> url);
    }
    // free method
//    if (basket -> method != NULL) {
//        LOG("DEBUG", "free basket -> method");
//        free((void *) basket -> method);
//    }
    // if (basket -> sessionId != NULL) {
    //     free((void *) basket -> sessionId);
    // }
    // free request headers
    if (basket -> request.headers != NULL) {
        for (size_t i = 0; i < basket -> request.numHeaders; i++) {
            if (basket -> request.headers[i].freeName == 1 && basket -> request.headers[i].name != NULL) {
                LOG("DEBUG", "free basket -> request.header[%zu].name  > %s", i, basket -> request.headers[i].name);
                free((void *) basket -> request.headers[i].name);
                basket -> request.headers[i].name = NULL;
            }
            if (basket -> request.headers[i].freeValue == 1 && basket -> request.headers[i].value != NULL) {
                LOG("DEBUG", "free basket -> request.header[%zu].value > %s", i, basket -> request.headers[i].value);
                free((void *) basket -> request.headers[i].value);
                basket -> request.headers[i].value = NULL;
            }
        }
        LOG("DEBUG", "free basket -> request.headers");
        free(basket -> request.headers);
    }
    // free request payload
    if (basket -> request.payload != NULL) {
        LOG("DEBUG", "free basket -> request.payload");
        free(basket -> request.payload);
    }
    // free response headers
    if (basket -> response.headers != NULL) {
        for (size_t i = 0; i < basket -> response.numHeaders; i++) {
            if (basket -> response.headers[i].freeName == 1 && basket -> response.headers[i].name != NULL) {
                LOG("DEBUG", "free basket -> response.header[%zu].name  > %s", i, basket -> response.headers[i].name);
                free(basket -> response.headers[i].name);
                basket -> response.headers[i].name = NULL;
            }
            if (basket -> response.headers[i].freeValue == 1 && basket -> response.headers[i].value != NULL) {
                LOG("DEBUG", "free basket -> response.header[%zu].value > %s", i, basket -> response.headers[i].value);
                free(basket -> response.headers[i].value);
                basket -> response.headers[i].value = NULL;
            }
        }
        LOG("DEBUG", "free basket -> response.headers");
        free(basket -> response.headers);
    }

    if (basket -> response.payload != NULL) {
        LOG("DEBUG", "free basket -> response.payload");
        free(basket -> response.payload);
    }
    free(basket);
    LOG("DEBUG", "free basket");
}

char* basketToString(Basket *basket, int *outLen) {
    json_t *root = json_object();

    // url
    json_object_set_new(root, "url", json_string(basket -> url));
    // method
    json_object_set_new(root, "method", json_string(basket -> method));
    json_object_set_new(root, "connectTimeoutInMilliseconds", json_integer(basket -> connectTimeoutInMilliseconds));
    json_object_set_new(root, "responseReadingTimeoutInMilliseconds", json_integer(basket -> responseReadingTimeoutInMilliseconds));
    json_object_set_new(root, "decompress", json_integer(basket -> decompress));

    // session
    json_t *session = json_object();
    if (basket -> session != NULL) {
        json_object_set_new(session, "creationTime", json_integer(basket -> session -> creationTime));
        json_object_set_new(session, "streamId", json_integer(basket -> streamId));
        json_object_set_new(session, "expirationInMilliseconds", json_integer(basket -> session -> expirationInMilliseconds));
    }
    json_object_set_new(root, "session", session);

    if (strlen(basket -> proxy.host) > 0) {
        json_t *proxy = json_object();
        json_object_set_new(proxy, "scheme", json_string(basket -> proxy.scheme));
        json_object_set_new(proxy, "host", json_string(basket -> proxy.host));
        json_object_set_new(proxy, "port", json_string(basket -> proxy.port));
        json_object_set_new(proxy, "authorization", json_string(basket -> proxy.authorization));
        json_object_set_new(root, "proxy", proxy);
    }

    // request
    json_t *request = json_object();

    json_t *requestHeaders = json_array();
    if (basket -> request.headers != NULL) {
        for (size_t i = 0; i < basket -> request.numHeaders; i++) {
            // + 3 because of ": "
            char headerStr[strlen(basket -> request.headers[i].name) + strlen(basket -> request.headers[i].value) + 3];
            snprintf(headerStr, sizeof(headerStr), "%s: %s", basket -> request.headers[i].name, basket -> request.headers[i].value);
            json_array_append_new(requestHeaders, json_string(headerStr));
        }
    }
    json_object_set_new(request, "headers", requestHeaders);

    if (basket -> request.payload != NULL) {
        json_object_set_new(request, "payload", json_string(basket -> request.payload));
    }

    json_object_set_new(root, "request", request);
    // response
    json_t *response = json_object();

    // json_t *responseHeaders = json_array();
    // if (basket -> response.headers != NULL) {
    //     for (size_t i = 0; i < basket -> response.numHeaders; i++) {
    //         // + 3 because of ": "
    //         char headerStr[strlen(basket -> response.headers[i].name) + strlen(basket -> response.headers[i].value) + 3];
    //         snprintf(headerStr, sizeof(headerStr), "%s: %s", basket -> response.headers[i].name, basket -> response.headers[i].value);
    //         json_array_append_new(responseHeaders, json_string(headerStr));
    //     }
    // }
    json_t *responseHeaders = json_object();
    if (basket->response.headers != NULL) {
        for (size_t i = 0; i < basket->response.numHeaders; i++) {
            const char *name = basket->response.headers[i].name;
            const char *value = basket->response.headers[i].value;

            if (strcasecmp(name, "set-cookie") == 0) {
                json_t *cookies = json_object_get(responseHeaders, "set-cookie");
                if (!cookies) {
                    cookies = json_array();
                    json_object_set_new(responseHeaders, "set-cookie", cookies);
                }
                json_array_append_new(cookies, json_string(value));
            } else {
                // append if existing (RFC 2616 §4.2）
                json_t *existing = json_object_get(responseHeaders, name);
                if (existing) {
                    const char *old = json_string_value(existing);
                    size_t newLen = strlen(old) + strlen(value) + 3; // ", " + '\0'
                    char merged[newLen];
                    snprintf(merged, sizeof(merged), "%s, %s", old, value);
                    json_object_set_new(responseHeaders, name, json_string(merged));
                } else {
                    json_object_set_new(responseHeaders, name, json_string(value));
                }
            }
        }
    }
    json_object_set_new(response, "headers", responseHeaders);

    if (basket -> response.payload != NULL) {
        json_object_set_new(response, "payload", json_string((const char*) basket -> response.payload));
    } else {
        json_object_set_new(response, "payload", json_string(""));
    }

    json_object_set_new(root, "response", response);
    // error
    json_t *error = json_object();
    if (basket -> error.code != NULL) {
        json_object_set_new(error, "code", json_string(basket -> error.code));
        json_object_set_new(error, "message", json_string(basket -> error.msg));
    }
    json_object_set_new(root, "error", error);

//    size_t flags = JSON_INDENT(4) | JSON_ENCODE_ANY;
    size_t flags = JSON_ENCODE_ANY;
    char *tempStr = json_dumps(root, flags);
    json_decref(root);

    if (outLen != NULL) {
        *outLen = (tempStr != NULL) ? (int) strlen(tempStr) : 0;
    }
    return tempStr;
}

const char * getUserAgent(Basket *basket) {
    for (size_t i = 0; i < basket -> request.numHeaders; i++) {
        if (strcasecmp(basket -> request.headers[i].name, "user-agent") == 0) {
            return basket -> request.headers[i].value;
        }
    }
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/144.0.0.0 Safari/537.36";
}