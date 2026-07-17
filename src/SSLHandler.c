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

void configureSSLSettings_Chrome(const char *hostname, SSL *ssl);

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
    SSL_CTX_set_grease_enabled(ctx, 1);
    SSL_CTX_set_permute_extensions(ctx, 1);

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
    if (basket -> browserType == BROWSER_CHROME) {
        configureSSLSettings_Chrome(basket -> request.urlComponents.host, ssl);
    } else {
        // TODO
        LOG("ERROR", "unsupported user-agent");
        basket -> error = ERR_REQUEST_UNSUPPORTED_USERAGENT;
        return -1;
    }
    return 1;
}

static const uint8_t emptySCTRequest[] = { 0x00, 0x00 };

static const uint8_t alpsSettings[] = {
    0x68, 0x32, // h2
    // 0x00, 0x00 // empty payload
};

// Chrome-like ALPN
static const uint8_t ALPN_CHROME[] = {
    0x02, // binary length prefix
    'h','2', // ASCII protocol ID
    0x08,
    'h','t','t','p','/','1','.','1'
};

// Chrome-like cipher suites
static const char *CIPHERS_CHROME =
    "TLS_AES_128_GCM_SHA256:"
    "TLS_AES_256_GCM_SHA384:"
    "TLS_CHACHA20_POLY1305_SHA256:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:"
    "ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-RSA-AES128-SHA:"
    "ECDHE-RSA-AES256-SHA:"
    "AES128-GCM-SHA256:"
    "AES256-GCM-SHA384:"
    "AES128-SHA:"
    "AES256-SHA";

static const char *GROUPS_CHROME =
    "X25519MLKEM768:"
    "X25519:"
    "P-256:"
    "P-384";

static const uint16_t SIGALGS_CHROME[] = {
    0x904,
    0x905,
    0x906,
    0x0403,  // ecdsa_secp256r1_sha256
    0x0804,  // rsa_pss_rsae_sha256
    0x0401,  // rsa_pkcs1_sha256
    0x0503,  // ecdsa_secp384r1_sha384
    0x0805,  // rsa_pss_rsae_sha384
    0x0501,  // rsa_pkcs1_sha384
    0x0806,  // rsa_pss_rsae_sha512
    0x0601   // rsa_pkcs1_sha512
};

void configureSSLSettings_Chrome(const char *hostname, SSL *ssl) {
    // set ALPN protocols
    SSL_set_alpn_protos(ssl, ALPN_CHROME, sizeof(ALPN_CHROME));
    // set cipher suites
    SSL_set_cipher_list(ssl, CIPHERS_CHROME);
    // enable ech grease
    SSL_set_enable_ech_grease(ssl, 1);
    // enable ECDH
    SSL_set_ecdh_auto(ssl, 1);
    // set SNI
    SSL_set_tlsext_host_name(ssl, hostname);
    // enable status_request (OCSP stapling)
    SSL_enable_ocsp_stapling(ssl);
    // enable signed cert timestamps
    SSL_enable_signed_cert_timestamps(ssl);
    SSL_set_signed_cert_timestamp_list(ssl, emptySCTRequest, sizeof(emptySCTRequest));
    // add application settings
    SSL_add_application_settings(ssl, alpsSettings, sizeof(alpsSettings), NULL, 0);
    // set supported groups
    SSL_set1_groups_list(ssl, GROUPS_CHROME);
    // set signature algorithms
    SSL_set_verify_algorithm_prefs(ssl, SIGALGS_CHROME, sizeof(SIGALGS_CHROME) / sizeof(SIGALGS_CHROME[0]));
    // add cert compression algorithms
    SSL_CTX_add_cert_compression_alg(SSL_get_SSL_CTX(ssl), 2, brotliCompressCb, brotliDecompressCb);
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

    cacheTLSSession(connInfo->host, connInfo->port, session);
    LOG("DEBUG", "newSessionCallback: cached TLS session for %s:%s", connInfo->host, connInfo->port);
    return 1; // we take ownership (cacheTLSSession does SSL_SESSION_up_ref internally)
}
