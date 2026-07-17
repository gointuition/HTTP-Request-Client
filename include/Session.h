//
// Created by Intuition on 26-2-7.
//

#ifndef SESSION_H
#define SESSION_H

#include "Basket.h"

#include "openssl/ssl.h"

void cacheTLSSession(const char *host, const char *port, SSL_SESSION *session);

#define MAX_SESSIONS 1024
#define SESSION_MAGIC 0X55AA1234
#define SESSION_ACTIVE 1
#define SESSION_INACTIVE 0

typedef struct {
    pthread_mutex_t mutex;
    size_t sessionCount;
    Session *sessions[MAX_SESSIONS];
    int sessionMap[MAX_SESSIONS]; // active session index map
    int running;    // daemon process running status
} SharedSessionPool;

SharedSessionPool * createSharedMemoryPool();

void initSharedSessionPool(void);

void cleanupTLSSessionCache(void);

void destroySharedMemoryPool(SharedSessionPool *pool);

void handleSession(Basket *basket);

void cleanupTargetSession(Basket *basket);

void cleanupSessions(int isAll);

int closeSession(Session *session, Error error);

void freeSession(SSL* ssl, SSL_CTX* ctx, int sockfd, HpackContext *hpackCtx, Error error);

int isConnecting(Session *session);

#endif //SESSION_H
