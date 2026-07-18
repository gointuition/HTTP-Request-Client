//
// Created by Intuition on 25-7-6.
//

#include "SocketHandler.h"

#include "Error.h"
#include "Log.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

static char* getAddressIPPort(const struct sockaddr *sa);
static int connectWithTimeout(Basket *basket, int sockfd, const struct sockaddr *addr, socklen_t addrlen, int timeoutInMillis, int isProxy);
static int connectToProxy(Basket *basket, int sockfd);

int createSocketThroughProxy(Basket *basket) {
    // create socket
    int sockfd = createSocket(basket, basket -> proxy.host, basket -> proxy.port, 1);
    if (sockfd < 0) {
        return -1;
    }
    LOG("DEBUG", "socket created to proxy server %s:%s", basket -> proxy.host, basket -> proxy.port);

    // connect to proxy
    int connected = connectToProxy(basket, sockfd);
    if (connected < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int createSocket(Basket *basket, const char *host, const char *port, int isProxy) {
    struct addrinfo hints = {0};
    struct addrinfo *result;
    struct addrinfo *rp;

    hints.ai_family = AF_UNSPEC; // return all available addresses
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        LOG("ERROR", "Host resolve failed: %s", host);
        basket -> error = isProxy == 1 ? ERR_PROXY_HOST_RESOLVE_FAILED : ERR_SESSION_HOST_RESOLVE_FAILED;
        return -1;
    }

    int sockfd;
    // loop all available addresses
    for (rp = result; rp != NULL; rp = rp -> ai_next) {
        // create socket
        sockfd = socket(rp -> ai_family, rp -> ai_socktype, rp -> ai_protocol);
        if (sockfd == -1) {
            LOG("ERROR", "Socket creation failed with host %s", host);
            basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CREATION_FAILED : ERR_SESSION_SOCKET_CREATION_FAILED;
            continue;
        }

        // if (connect(sockfd, rp -> ai_addr, rp -> ai_addrlen) == 0) break;
        if (connectWithTimeout(basket, sockfd, rp -> ai_addr, rp -> ai_addrlen, basket -> connectTimeoutInMilliseconds, isProxy) == 0) {
            break;
        }

        close(sockfd);
    }

    freeaddrinfo(result);

    if (rp == NULL) {
        if (basket -> error.code == NULL) {
            LOG("ERROR", "Socket no available address");
            basket -> error = ERR_SESSION_SOCKET_NO_AVAILABLE_ADDRESS;
        }
        return -1;
    }

    return sockfd;
}

static int connectToProxy(Basket *basket, int sockfd) {
    const char *userAgent = getUserAgent(basket);
    char connectRequest[1024];
    int requestLen;
    if (strlen(basket -> proxy.authorization) > 0) {
        requestLen = snprintf(connectRequest, sizeof(connectRequest),
                              "CONNECT %s:%s HTTP/1.1\r\n"
                              "Host: %s:%s\r\n"
                              "Proxy-Authorization: %s\r\n"
                              "User-Agent: %s\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n",
                              basket -> request.urlComponents.host,
                              basket -> request.urlComponents.port,
                              basket -> request.urlComponents.host,
                              basket -> request.urlComponents.port,
                              basket -> proxy.authorization,
                              userAgent
        );
    } else {
        requestLen = snprintf(connectRequest, sizeof(connectRequest),
                              "CONNECT %s:%s HTTP/1.1\r\n"
                              "Host: %s:%s\r\n"
                              "User-Agent: %s\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n",
                              basket -> request.urlComponents.host,
                              basket -> request.urlComponents.port,
                              basket -> request.urlComponents.host,
                              basket -> request.urlComponents.port,
                              userAgent
        );
    }

    // send connect request to proxy
    if (send(sockfd, connectRequest, requestLen, 0) != requestLen) {
        LOG("ERROR", "send() failed when sending CONNECT request to proxy");
        basket -> error = ERR_PROXY_SEND_CONNECT_REQUEST_FAILED;
        return -1;
    }

    // read proxy response
    char proxyResponse[1024];
    const size_t bytesRead = recv(sockfd, proxyResponse, sizeof(proxyResponse) - 1, 0);
    if (bytesRead <= 0) {
        LOG("ERROR", "recv() failed when sending CONNECT request to proxy");
        basket -> error = ERR_PROXY_SEND_CONNECT_REQUEST_FAILED;
        return -1;
    }

    proxyResponse[bytesRead] = '\0';
//    LOG("DEBUG", "proxy response: %s", proxyResponse);
    LOG("DEBUG", "proxy response size: %zu", bytesRead);

    // check proxy response
    if (strstr(proxyResponse, "HTTP/1.1 407") != NULL) {
        LOG("DEBUG", "407 proxy authorization failed");
        basket -> error = ERR_PROXY_AUTHORIZATION_FAILED;
        return -1;
    }
    if (strstr(proxyResponse, "HTTP/1.1 200") == NULL || strstr(proxyResponse, "HTTP/1.1 200") == NULL) {
        LOG("ERROR", "unexpected CONNECT response from proxy server");
        basket -> error = ERR_PROXY_UNEXPECTED_RESPONSE;
        return -1;
    }
    LOG("DEBUG", "proxy CONNECT succeeded");
    return 1;
}

static int connectWithTimeout(Basket *basket, int sockfd, const struct sockaddr *addr, socklen_t addrlen, int timeoutInMillis, int isProxy) {
    // set socket non-blocking mode
    const int flags = fcntl(sockfd, F_GETFL, 0);
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG("ERROR", "failed to set socket non-block");
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_NONBLOCK_SETTING_FAILED : ERR_SESSION_SOCKET_NONBLOCK_SETTING_FAILED;
        return -1;
    }

    // non-blocking connect
    const int rc = connect(sockfd, addr, addrlen);
    if (rc == 0) {
        // connected immediately, restore blocking mode
        fcntl(sockfd, F_SETFL, flags);
        return 0;
    }

    if (errno != EINPROGRESS) {
        LOG("ERROR", "socket connecting failed, errno != EINPROGRESS");
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_FAILED : ERR_SESSION_SOCKET_CONNECTING_FAILED;
        return -1;
    }

    // Validate socket file descriptor
    if (sockfd < 0 || sockfd >= FD_SETSIZE) {
        LOG("ERROR", "socket invalid file descriptor: %d (FD_SETSIZE: %d)", sockfd, FD_SETSIZE);
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_INVALID_FILE_DESCRIPTOR : ERR_SESSION_SOCKET_INVALID_FILE_DESCRIPTOR;
        return -1;
    }

    // connection in progress, wait with timeout
    fd_set writeFds;

    FD_ZERO(&writeFds);
    FD_SET(sockfd, &writeFds);

    struct timeval timeout;
    timeout.tv_sec = timeoutInMillis / 1000;
    // tv_usec cannot exceed 999 999, or select() failed： Invalid argument (errno: 22)
    timeout.tv_usec = timeoutInMillis % 1000 * 1000;

    const int sr = select(sockfd + 1, NULL, &writeFds, NULL, &timeout);
    if (sr == 0) {
        char *ip_port = getAddressIPPort(addr);
        LOG("ERROR", "%s socket connecting timeout after %ld s", ip_port, timeout.tv_sec);
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_TIMEOUT : ERR_SESSION_SOCKET_CONNECTING_TIMEOUT;
        free(ip_port);
        return -1;
    }
    if (sr < 0) {
        LOG("ERROR", "socket select() failed： %s (errno: %d)", strerror(errno), errno);
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR : ERR_SESSION_SOCKET_CONNECTING_UNKNOWN_ERROR;
        return -1;
    }

    // // poll until connected
    // struct pollfd pfd;
    // pfd.fd = sockfd;
    // pfd.events = POLLIN;
    //
    // rc = poll(&pfd, 1, timeoutInMillis);
    // if (rc == 0) {
    //     errno = ETIMEDOUT;
    //     setLastErrorMessage("Connection timed out after %d ms", timeoutInMillis);
    //     return -1;
    // }
    // if (rc < 0) {
    //     setLastErrorMessage("Connection unknown error after %d ms", timeoutInMillis);
    //     return -1;
    // }

    // check if socket has error
    int error = 0;
    socklen_t errorLen = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &errorLen) < 0) {
        // setLastErrorMessage("getsockopt failed when connecting");
        LOG("ERROR", "getsockopt failed when connecting");
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR : ERR_SESSION_SOCKET_CONNECTING_UNKNOWN_ERROR;
        return -1;
    }

    if (error == ECONNREFUSED) {
        LOG("ERROR", "socket connecting refused");
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_REFUSED : ERR_SESSION_SOCKET_CONNECTING_REFUSED;
        return -1;
    }

    if (error != 0) {
        errno = error;
        LOG("ERROR", "error != 0，unknown socket error when connecting");
        basket -> error = isProxy == 1 ? ERR_PROXY_SOCKET_CONNECTING_UNKNOWN_ERROR : ERR_SESSION_SOCKET_CONNECTING_UNKNOWN_ERROR;
        return -1;
    }

    // connected, restore to blocking mode
    fcntl(sockfd, F_SETFL, flags);
    return 0;
}

static char* getAddressIPPort(const struct sockaddr *sa) {
    char ip[INET6_ADDRSTRLEN];
    unsigned short port;
    char *ip_port;

    if (sa -> sa_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *) sa;
        inet_ntop(AF_INET, &(ipv4 -> sin_addr), ip, sizeof(ip));
        port = ntohs(ipv4 -> sin_port);

        // "IPV4:"(5) + IP(15) + ":"(1) + PORT(5) + null(1) =  26
        ip_port = malloc(64);
        if (ip_port == NULL) { return NULL; }
        snprintf(ip_port, 64, "IPV4:%s:%d", ip, port);
    } else if (sa -> sa_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *) sa;
        inet_ntop(AF_INET6, &(ipv6 -> sin6_addr), ip, sizeof(ip));
        port = ntohs(ipv6 -> sin6_port);

        // "IPv6:["(6) + IP(39) + "]:"(2) + port(5) + null(1) = 53
        ip_port = malloc(64);
        if (ip_port == NULL) { return NULL; }
        snprintf(ip_port, 64, "IPV6:[%s]:%d", ip, port);
    } else {
        return strdup("unknown address family");
    }
    return ip_port;
}
