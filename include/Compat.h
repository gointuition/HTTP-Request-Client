//
// Cross-platform compatibility layer.
//
// Bridges the POSIX socket API used throughout this project with the Windows
// Winsock2 API, so the networking code compiles unchanged on macOS/Linux and
// on Windows (MinGW-w64). Include this header instead of the raw POSIX socket
// headers (<sys/socket.h>, <netdb.h>, <arpa/inet.h>, <unistd.h>, <fcntl.h>).
//

#ifndef COMPAT_H
#define COMPAT_H

#include <errno.h>

#ifdef _WIN32

    // winsock2.h must be included before windows.h
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    // POSIX close() on a socket maps to closesocket() on Windows
    #define closeSocket(fd)      closesocket(fd)

    // Winsock reports errors via WSAGetLastError() rather than errno; a
    // non-blocking connect() in progress yields WSAEWOULDBLOCK (not EINPROGRESS).
    #define SOCKET_LAST_ERROR    WSAGetLastError()
    #define SOCKET_EINPROGRESS   WSAEWOULDBLOCK
    #define SOCKET_ECONNREFUSED  WSAECONNREFUSED
    // Winsock reports a recv() that hit its SO_RCVTIMEO as WSAETIMEDOUT.
    #define SOCKET_EAGAIN        WSAETIMEDOUT

#else

    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>

    #define closeSocket(fd)      close(fd)

    #define SOCKET_LAST_ERROR    errno
    #define SOCKET_EINPROGRESS   EINPROGRESS
    #define SOCKET_ECONNREFUSED  ECONNREFUSED
    // POSIX reports a recv() that hit its SO_RCVTIMEO as EAGAIN/EWOULDBLOCK.
    #define SOCKET_EAGAIN        EAGAIN

#endif

// Toggle a socket into non-blocking mode. Returns 0 on success, -1 on error.
static inline int setSocketNonBlocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket((SOCKET) fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return -1; }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Toggle a socket back into blocking mode. Returns 0 on success, -1 on error.
static inline int setSocketBlocking(int fd) {
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket((SOCKET) fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return -1; }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

// Set a timeout (in milliseconds) on blocking recv() calls. A proxy CONNECT
// exchange must never block forever waiting for the proxy's response, so every
// socket that performs a blocking recv() during tunnel establishment gets a
// receive timeout. Returns 0 on success, -1 on error.
static inline int setSocketReceiveTimeout(int fd, int timeoutInMillis) {
#ifdef _WIN32
    DWORD tv = timeoutInMillis < 0 ? 0 : (DWORD) timeoutInMillis;
    return setsockopt((SOCKET) fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof(tv)) == 0 ? 0 : -1;
#else
    struct timeval tv;
    tv.tv_sec = timeoutInMillis / 1000;
    tv.tv_usec = timeoutInMillis % 1000 * 1000; // tv_usec must stay < 1e6
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 ? 0 : -1;
#endif
}

// Set a timeout (in milliseconds) on blocking send() calls, mirroring the
// receive-side helper above.
static inline int setSocketSendTimeout(int fd, int timeoutInMillis) {
#ifdef _WIN32
    DWORD tv = timeoutInMillis < 0 ? 0 : (DWORD) timeoutInMillis;
    return setsockopt((SOCKET) fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof(tv)) == 0 ? 0 : -1;
#else
    struct timeval tv;
    tv.tv_sec = timeoutInMillis / 1000;
    tv.tv_usec = timeoutInMillis % 1000 * 1000; // tv_usec must stay < 1e6
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 ? 0 : -1;
#endif
}

// Cross-platform sleep, in microseconds.
static inline void sleepMicroseconds(unsigned int usec) {
#ifdef _WIN32
    Sleep(usec / 1000);
#else
    usleep(usec);
#endif
}

#endif // COMPAT_H
