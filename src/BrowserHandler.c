//
// Created by Intuition on 25-8-16.
//

#include <string.h>

#include "BrowserHandler.h"

// ===========================================================================
// Chrome (desktop) fingerprint
// ===========================================================================

static const unsigned char CHROME_SETTINGS_FRAME[] = {
    0x00, 0x00, 0x18,           // Length: 24 bytes (0x18)
    0x04,                       // Type: SETTINGS (4)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0

    // Settings payload (akamai: 1:65536;2:0;4:6291456;6:262144)
    0x00, 0x01,             // HEADER_TABLE_SIZE
    0x00, 0x01, 0x00, 0x00, // 65536

    0x00, 0x02,             // ENABLE_PUSH
    0x00, 0x00, 0x00, 0x00, // 0

    0x00, 0x04,             // INITIAL_WINDOW_SIZE
    0x00, 0x60, 0x00, 0x00, // 6291456

    0x00, 0x06,             // MAX_HEADER_LIST_SIZE
    0x00, 0x04, 0x00, 0x00, // 262144
};

static const unsigned char CHROME_WINDOW_UPDATE_FRAME[] = {
    0x00, 0x00, 0x04,           // Length: 4
    0x08,                       // Type: WINDOW_UPDATE (8)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0
    0x00, 0xEF, 0x00, 0x01      // Increment: 15663105
};

// ALPN: h2, http/1.1
static const unsigned char CHROME_ALPN[] = {
    0x02, 'h','2',
    0x08, 'h','t','t','p','/','1','.','1'
};

static const char CHROME_CIPHERS[] =
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
    "AES256-SHA:";

// CHROME supported groups (peetprint: 4588-29-23-24, no P-521)
static const char CHROME_GROUPS[] =
    "X25519MLKEM768:"
    "X25519:"
    "P-256:"
    "P-384";

// CHROME signature algorithms (peetprint order:
// 2308-2309-2310-1027-2052-1025-1283-2053-1281-2054-1537)
static const uint16_t CHROME_SIGALGS[] = {
    0x0904,  // mldsa44 (ML-DSA-44)
    0x0905,  // mldsa65 (ML-DSA-65)
    0x0906,  // mldsa87 (ML-DSA-87)
    0x0403,  // ecdsa_secp256r1_sha256
    0x0804,  // rsa_pss_rsae_sha256
    0x0401,  // rsa_pkcs1_sha256
    0x0503,  // ecdsa_secp384r1_sha384
    0x0805,  // rsa_pss_rsae_sha384
    0x0501,  // rsa_pkcs1_sha384
    0x0806,  // rsa_pss_rsae_sha512
    0x0601   // rsa_pkcs1_sha512
};

// Chrome (desktop) request pseudo-header order: :method, :authority, :scheme, :path
static const PseudoHeaderType CHROME_PSEUDO_ORDER[] = {
    PSEUDO_METHOD, PSEUDO_AUTHORITY, PSEUDO_SCHEME, PSEUDO_PATH
};

static const BrowserFingerprint CHROME_FINGERPRINT_150 = {
    .settingsFrame = CHROME_SETTINGS_FRAME,
    .settingsFrameLen = sizeof(CHROME_SETTINGS_FRAME),
    .windowUpdateFrame = CHROME_WINDOW_UPDATE_FRAME,
    .windowUpdateFrameLen = sizeof(CHROME_WINDOW_UPDATE_FRAME),
    .headerValueMaxLength = 4096,
    .alpn = CHROME_ALPN,
    .alpnLen = sizeof(CHROME_ALPN),
    .cipherList = CHROME_CIPHERS,
    .groups = CHROME_GROUPS,
    .sigAlgs = CHROME_SIGALGS,
    .sigAlgsCount = sizeof(CHROME_SIGALGS) / sizeof(CHROME_SIGALGS[0]),
    .enableGrease = 1,
    .enablePermuteExtensions = 1,
    .enableEchGrease = 1,
    .enableAlps = 1,
    .certCompressionAlg = 2,   // brotli
    .enableSessionTicket = 1,
    .enablePreSharedKey = 1,
    .enableHeadersPriority = 1,
    .pseudoHeaderOrder = CHROME_PSEUDO_ORDER,
};

// Alias the currently emulated Chrome version to the generic Chrome profile.
#define CHROME_FINGERPRINT CHROME_FINGERPRINT_150

// ===========================================================================
// CriOS (Chrome on iOS) fingerprint.
// Recent Chrome on iOS (CriOS 1xx) ships Chrome's own network stack (Cronet
// over BoringSSL), so its wire fingerprint matches desktop Chrome rather than
// iOS Safari: Chrome-style HTTP/2 SETTINGS, GREASE, the post-quantum
// X25519MLKEM768 group and compress_certificate (zlib).
// ===========================================================================

static const unsigned char CRIOS_SETTINGS_FRAME[] = {
    0x00, 0x00, 0x18,           // Length: 24 bytes (4 settings)
    0x04,                       // Type: SETTINGS (4)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0

    // Settings payload
    0x00, 0x02,             // ENABLE_PUSH
    0x00, 0x00, 0x00, 0x00, // 0

    0x00, 0x03,             // MAX_CONCURRENT_STREAMS
    0x00, 0x00, 0x00, 0x64, // 100

    0x00, 0x04,             // INITIAL_WINDOW_SIZE
    0x00, 0x20, 0x00, 0x00, // 2097152

    0x00, 0x09,             // NO_RFC7540_PRIORITIES
    0x00, 0x00, 0x00, 0x01, // 1
};

static const unsigned char CRIOS_WINDOW_UPDATE_FRAME[] = {
    0x00, 0x00, 0x04,           // Length: 4
    0x08,                       // Type: WINDOW_UPDATE (8)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0
    0x00, 0x9F, 0x00, 0x01      // Increment: 10420225 (total window 10485760)
};

// ALPN: h2, http/1.1
static const unsigned char CRIOS_ALPN[] = {
    0x02, 'h','2',
    0x08, 'h','t','t','p','/','1','.','1'
};

// CriOS cipher suites (matches desktop Chrome order), including the three
// legacy 3DES suites (0xc008 / 0xc012 / 0x000a) at the tail, which this
// BoringSSL build re-enables so the wire fingerprint matches the reference.
static const char CRIOS_CIPHERS[] =
    "TLS_AES_256_GCM_SHA384:"
    "TLS_CHACHA20_POLY1305_SHA256:"
    "TLS_AES_128_GCM_SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:"
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES256-SHA:"
    "ECDHE-ECDSA-AES128-SHA:"
    "ECDHE-RSA-AES256-SHA:"
    "ECDHE-RSA-AES128-SHA:"
    "AES256-GCM-SHA384:"
    "AES128-GCM-SHA256:"
    "AES256-SHA:"
    "AES128-SHA:"
    "ECDHE-ECDSA-DES-CBC3-SHA:"
    "ECDHE-RSA-DES-CBC3-SHA:"
    "DES-CBC3-SHA";

// CriOS supported groups (post-quantum X25519MLKEM768 first)
static const char CRIOS_GROUPS[] =
    "X25519MLKEM768:"
    "X25519:"
    "P-256:"
    "P-384:"
    "P-521";

// CriOS signature algorithms (matches the reference peetprint order
// 1027-2052-1025-1283-2053-2053-1281-2054-1537-513)
static const uint16_t CRIOS_SIGALGS[] = {
    0x0403,  // ecdsa_secp256r1_sha256
    0x0804,  // rsa_pss_rsae_sha256
    0x0401,  // rsa_pkcs1_sha256
    0x0503,  // ecdsa_secp384r1_sha384
    0x0805,  // rsa_pss_rsae_sha384
    0x0805,  // rsa_pss_rsae_sha384
    0x0501,  // rsa_pkcs1_sha384
    0x0806,  // rsa_pss_rsae_sha512
    0x0601,  // rsa_pkcs1_sha512
    0x0201   // rsa_pkcs1_sha1
};

// CriOS (Chrome on iOS) request pseudo-header order: :method, :scheme, :authority, :path
static const PseudoHeaderType CRIOS_PSEUDO_ORDER[] = {
    PSEUDO_METHOD, PSEUDO_SCHEME, PSEUDO_AUTHORITY, PSEUDO_PATH
};

static const BrowserFingerprint CRIOS_FINGERPRINT_150 = {
    .settingsFrame = CRIOS_SETTINGS_FRAME,
    .settingsFrameLen = sizeof(CRIOS_SETTINGS_FRAME),
    .windowUpdateFrame = CRIOS_WINDOW_UPDATE_FRAME,
    .windowUpdateFrameLen = sizeof(CRIOS_WINDOW_UPDATE_FRAME),
    .headerValueMaxLength = 4096,
    .alpn = CRIOS_ALPN,
    .alpnLen = sizeof(CRIOS_ALPN),
    .cipherList = CRIOS_CIPHERS,
    .groups = CRIOS_GROUPS,
    .sigAlgs = CRIOS_SIGALGS,
    .sigAlgsCount = sizeof(CRIOS_SIGALGS) / sizeof(CRIOS_SIGALGS[0]),
    // CriOS uses GREASE and compress_certificate (zlib), but keeps a fixed
    // (non-permuted) extension order and no ECH GREASE / ALPS.
    .enableGrease = 1,
    .enablePermuteExtensions = 0,
    .enableEchGrease = 0,
    .enableAlps = 0,
    .certCompressionAlg = 1,   // zlib
    // iOS Chrome does not advertise the TLS 1.2 session_ticket (35) extension,
    // nor does it offer TLS 1.3 resumption via pre_shared_key (41).
    .enableSessionTicket = 0,
    .enablePreSharedKey = 0,
    .enableHeadersPriority = 0,
    .pseudoHeaderOrder = CRIOS_PSEUDO_ORDER,
};

// Alias the currently emulated CriOS version to the generic CriOS profile.
#define CRIOS_FINGERPRINT CRIOS_FINGERPRINT_150

const BrowserFingerprint* getBrowserFingerprint(BrowserType type) {
    switch (type) {
        case BROWSER_CHROME:     return &CHROME_FINGERPRINT;
        case BROWSER_CHROME_IOS: return &CRIOS_FINGERPRINT;
        default:                 return NULL;
    }
}

int isChromeUA(const char *ua) {
    const char *chromePos = strstr(ua, "Chrome");
    if (chromePos == NULL) {
        return 0;
    }
    // TODO Google Chrome only
    if (strstr(ua, "Edge") || strstr(ua, "OPR")) {
        return 0;
    }
    return 1;
}

BrowserType detectBrowseType(const char *ua) {
    if (strstr(ua, "Opr/") != NULL || strstr(ua, "Opera") != NULL) {
        return BROWSER_OPERA;
    }
    if (strstr(ua, "Edg") != NULL) {
        return BROWSER_EDGE;
    }
    // CriOS (iOS Chrome) is WebKit-based, must be checked before "Chrome"
    if (strstr(ua, "CriOS") != NULL) {
        return BROWSER_CHROME_IOS;
    }
    if (strstr(ua, "Chrome") != NULL) {
        return BROWSER_CHROME;
    }
    if (strstr(ua, "Firefox") != NULL) {
        return BROWSER_FIREFOX;
    }
    if (strstr(ua, "Safari") != NULL) {
        return BROWSER_SAFARI;
        // } else if (strstr(ua, "Msie") != NULL || strstr(ua, "Trident") != NULL) {
        //     return BROWSER_IE;
    }

    return BROWSER_UNKNOWN;
}