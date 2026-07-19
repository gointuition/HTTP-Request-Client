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

#else

    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>

    #define closeSocket(fd)      close(fd)

    #define SOCKET_LAST_ERROR    errno
    #define SOCKET_EINPROGRESS   EINPROGRESS
    #define SOCKET_ECONNREFUSED  ECONNREFUSED

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

// Cross-platform sleep, in microseconds.
static inline void sleepMicroseconds(unsigned int usec) {
#ifdef _WIN32
    Sleep(usec / 1000);
#else
    usleep(usec);
#endif
}

#endif // COMPAT_H
