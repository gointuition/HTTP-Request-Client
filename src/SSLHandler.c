//
// Created by Intuition on 25-7-14.
//

#include <string.h>
#include <stdint.h>
#include <zlib.h>

#include "SSLHandler.h"
#include "Error.h"
#include "BrowserHandler.h"
#include "Log.h"
#include "Session.h"

#include "brotli/decode.h"
#include "brotli/encode.h"

static void applyTlsFingerprint(const char *hostname, SSL *ssl, const BrowserFingerprint *fp);

int zlibCompressCb(SSL *ssl, CBB *out, const uint8_t *in, size_t inLen);
int zlibDecompressCb(SSL *ssl, CRYPTO_BUFFER **out, size_t uncompressedLen, const uint8_t *in, size_t inLen);
int brotliCompressCb(SSL *ssl, CBB *out, const uint8_t *in, size_t inLen);
int brotliDecompressCb(SSL *ssl, CRYPTO_BUFFER **out, size_t uncompressedLen, const uint8_t *in, size_t inLen);
int newSessionCallback(SSL *ssl, SSL_SESSION *session);

SSL_CTX* createSSLContext(Basket *basket) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        LOG("ERROR", "ssl context creation failed");
        basket -> error = ERR_SESSION_SSL_CTX_CREATION_FAILED;
        return NULL;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // GREASE (RFC 8701) can only be toggled at the SSL_CTX level, so gate it
    // here based on the browser fingerprint (Chrome uses it, Safari/CriOS does not).
    const BrowserFingerprint *fp = getBrowserFingerprint(basket -> browserType);
    if (fp != NULL && fp -> enableGrease) {
        SSL_CTX_set_grease_enabled(ctx, 1);
//        SSL_CTX_set_strict_cipher_list(ctx, fp -> cipherList);
    }

    // enable client-side session cache and register callback for TLS 1.3 session resumption (pre_shared_key)
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(ctx, newSessionCallback);


    return ctx;
}

SSL* createSSL(Basket *basket, SSL_CTX *ctx, int sockfd) {
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        LOG("ERROR", "ssl object creation failed");
        basket -> error = ERR_SESSION_SSL_OBJECT_CREATION_FAILED;
        return NULL;
    }
    // a function that associates an existing file descriptor (socket) with an SSL connection object
    if (!SSL_set_fd(ssl, sockfd)) {
        LOG("ERROR", "ssl failed to associate with a socket");
        basket -> error = ERR_SESSION_SSL_FAILED_TO_ASSOCIATE_SOCKET;

        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

int configureSSLSettings(Basket *basket, SSL *ssl) {
    const BrowserFingerprint *fp = getBrowserFingerprint(basket -> browserType);
    if (fp == NULL) {
        // TODO
        LOG("ERROR", "unsupported user-agent");
        basket -> error = ERR_REQUEST_UNSUPPORTED_USERAGENT;
        return -1;
    }
    applyTlsFingerprint(basket -> request.urlComponents.host, ssl, fp);
    return 1;
}

// status_request: empty responder-id list + empty request extensions
static const uint8_t emptySCTRequest[] = { 0x00, 0x00 };

// application settings (ALPS) payload advertising h2
static const uint8_t alpsSettings[] = {
    0x68, 0x32, // h2
    // 0x00, 0x00 // empty payload
};

// Applies a browser's TLS ClientHello fingerprint. All browser-specific data
// (cipher list, groups, sigalgs, extension toggles) comes from the profile,
// so this function stays browser-agnostic.
static void applyTlsFingerprint(const char *hostname, SSL *ssl, const BrowserFingerprint *fp) {
    // permute ClientHello extensions (Chrome only)
    if (fp -> enablePermuteExtensions) {
        SSL_set_permute_extensions(ssl, 1);
    }
    // suppress the session_ticket (35) extension when the profile omits it
    // (e.g. iOS Chrome). SSL_OP_NO_TICKET only drops the TLS 1.2 ticket
    // extension from the ClientHello; TLS 1.3 resumption via pre_shared_key is
    // a separate mechanism and stays enabled.
    if (!fp -> enableSessionTicket) {
        SSL_set_options(ssl, SSL_OP_NO_TICKET);
    }
    // set ALPN protocols
    SSL_set_alpn_protos(ssl, fp -> alpn, fp -> alpnLen);
    // set cipher suites
    SSL_set_strict_cipher_list(ssl, fp -> cipherList);
//    SSL_CTX_set_strict_cipher_list(ssl, fp -> cipherList);
    // set TLS 1.3 cipher order (SSL_set_strict_cipher_list only covers TLS 1.2
    // and below). Reuse the same cipher list string: BoringSSL picks out the
    // TLS 1.3 ciphers named in it and advertises them in that exact order,
    // instead of its default AES-hardware-based ordering.
    SSL_set_tls13_cipher_prefs(ssl, fp -> cipherList);
    // enable ech grease (Chrome only)
    if (fp -> enableEchGrease) {
        SSL_set_enable_ech_grease(ssl, 1);
    }
    // enable ECDH
    SSL_set_ecdh_auto(ssl, 1);
    // set SNI
    SSL_set_tlsext_host_name(ssl, hostname);
    // enable status_request (OCSP stapling)
    SSL_enable_ocsp_stapling(ssl);
    // enable signed cert timestamps
    SSL_enable_signed_cert_timestamps(ssl);
    SSL_set_signed_cert_timestamp_list(ssl, emptySCTRequest, sizeof(emptySCTRequest));
    // add application settings (ALPS, Chrome only)
    if (fp -> enableAlps) {
        SSL_add_application_settings(ssl, alpsSettings, sizeof(alpsSettings), NULL, 0);
    }
    // set supported groups
    SSL_set1_groups_list(ssl, fp -> groups);
    // set signature algorithms
    SSL_set_verify_algorithm_prefs(ssl, fp -> sigAlgs, fp -> sigAlgsCount);
    // add cert compression algorithm (compress_certificate extension). Desktop
    // Chrome advertises brotli (2); iOS Chrome advertises zlib (1).
    if (fp -> certCompressionAlg == 2) {
        SSL_CTX_add_cert_compression_alg(SSL_get_SSL_CTX(ssl), 2, brotliCompressCb, brotliDecompressCb); // brotli (2)
    } else if (fp -> certCompressionAlg == 1) {
        SSL_CTX_add_cert_compression_alg(SSL_get_SSL_CTX(ssl), 1, zlibCompressCb, zlibDecompressCb);     // zlib (1)
    }
}

int zlibCompressCb(SSL *ssl, CBB *out, const uint8_t *in, size_t inLen) {
    uLongf compressedSize = compressBound((uLong) inLen);
    uint8_t *compressed = (uint8_t *) OPENSSL_malloc(compressedSize);
    if (!compressed) { return 0; }

    if (compress(compressed, &compressedSize, in, (uLong) inLen) != Z_OK) {
        OPENSSL_free(compressed);
        return 0;
    }

    int ret = CBB_add_bytes(out, compressed, compressedSize);
    OPENSSL_free(compressed);
    return ret;
}

int zlibDecompressCb(SSL *ssl, CRYPTO_BUFFER **out, size_t uncompressedLen, const uint8_t *in, size_t inLen) {
    uint8_t *decompressed = (uint8_t *) OPENSSL_malloc(uncompressedLen);
    if (!decompressed) { return 0; }

    uLongf decompressedSize = (uLongf) uncompressedLen;
    if (uncompress(decompressed, &decompressedSize, in, (uLong) inLen) != Z_OK
        || decompressedSize != uncompressedLen) {
        OPENSSL_free(decompressed);
        return 0;
    }

    *out = CRYPTO_BUFFER_new(decompressed, decompressedSize, NULL);
    OPENSSL_free(decompressed);
    return *out != NULL;
}

int brotliCompressCb(SSL *ssl, CBB *out, const uint8_t *in, size_t inLen) {
    size_t maxCompressedSize = BrotliEncoderMaxCompressedSize(inLen);
    if (maxCompressedSize == 0) { return 0; }

    uint8_t *compressed = (uint8_t *) OPENSSL_malloc(maxCompressedSize);
    if (!compressed) { return 0; }

    size_t compressedSize = maxCompressedSize;
    BROTLI_BOOL result = BrotliEncoderCompress(
        BROTLI_DEFAULT_QUALITY,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        inLen, in,
        &compressedSize, compressed
    );

    if (result != BROTLI_TRUE) {
        OPENSSL_free(compressed);
        return 0;
    }

    int ret = CBB_add_bytes(out, compressed, compressedSize);
    OPENSSL_free(compressed);

    return ret;
}

int brotliDecompressCb(SSL *ssl, CRYPTO_BUFFER **out, size_t uncompressedLen, const uint8_t *in, size_t inLen) {
    uint8_t *decompressed = OPENSSL_malloc(uncompressedLen);
    if (!decompressed) {
        return 0;
    }

    size_t decompressedSize = uncompressedLen;

    if (BrotliDecoderDecompress(inLen, in, &decompressedSize, decompressed) != BROTLI_DECODER_RESULT_SUCCESS
        || decompressedSize != uncompressedLen) {
        OPENSSL_free(decompressed);
        return 0;
    }

    *out = CRYPTO_BUFFER_new(decompressed, decompressedSize, NULL);
    OPENSSL_free(decompressed);

    return *out != NULL;
}

// called by BoringSSL when a new TLS session is available (after handshake or after NewSessionTicket in TLS 1.3)
int newSessionCallback(SSL *ssl, SSL_SESSION *session) {
    TLSConnInfo *connInfo = (TLSConnInfo *) SSL_get_app_data(ssl);
    if (!connInfo || !connInfo->host || !connInfo->port) {
        LOG("WARN", "newSessionCallback: no conn info available, skipping session cache");
        return 0;
    }

    cacheTLSSession(connInfo->host, connInfo->port, session,
                    connInfo->proxyScheme ? connInfo->proxyScheme : "",
                    connInfo->proxyHost ? connInfo->proxyHost : "",
                    connInfo->proxyPort ? connInfo->proxyPort : "",
                    connInfo->proxyAuthorization ? connInfo->proxyAuthorization : "");
    LOG("DEBUG", "newSessionCallback: cached TLS session for %s:%s via proxy %s://%s:%s",
        connInfo->host, connInfo->port,
        connInfo->proxyScheme, connInfo->proxyHost, connInfo->proxyPort);
    return 1; // we take ownership (cacheTLSSession does SSL_SESSION_up_ref internally)
}
