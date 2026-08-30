//
// Created by Intuition on 25-8-16.
//

#ifndef BROWSER_H
#define BROWSER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BROWSER_UNKNOWN = 0,
    BROWSER_CHROME,
    BROWSER_CHROME_IOS,
    BROWSER_FIREFOX,
    BROWSER_SAFARI,
    BROWSER_EDGE,
    BROWSER_OPERA,
    BROWSER_IE
} BrowserType;

// HTTP/2 request pseudo-headers. Browsers emit them in a distinct, stable
// order that forms part of their wire fingerprint (e.g. desktop Chrome uses
// :method/:authority/:scheme/:path, while iOS Chrome uses
// :method/:scheme/:authority/:path).
typedef enum {
    PSEUDO_METHOD = 0,
    PSEUDO_AUTHORITY,
    PSEUDO_SCHEME,
    PSEUDO_PATH
} PseudoHeaderType;

// A browser's complete wire fingerprint, gathered into a single profile:
// the HTTP/2 SETTINGS/WINDOW_UPDATE frames plus the TLS ClientHello shape.
typedef struct {
    const char *clientHelloId;

    // HTTP/2 fingerprint
    const unsigned char *settingsFrame;
    size_t settingsFrameLen;
    const unsigned char *windowUpdateFrame;
    size_t windowUpdateFrameLen;
    int headerValueMaxLength;

    // TLS (ClientHello) fingerprint
    const unsigned char *alpn;
    size_t alpnLen;
    const char *cipherList;
    const char *groups;
    const uint16_t *sigAlgs;
    size_t sigAlgsCount;
    int enableGrease;            // TLS GREASE (RFC 8701), Chrome-only
    int enableGreaseSigalgs;     // Chrome 152+ GREASE value in signature_algorithms
    int enablePermuteExtensions; // ClientHello extension permutation, Chrome-only
    int enableEchGrease;         // Chrome-only ECH GREASE
    int enableAlps;             // Chrome-only application settings (ALPS)
    int enableTrustAnchors;     // Chrome 152+ trust_anchors (51764/0xca34) ClientHello extension
    int certCompressionAlg;     // compress_certificate (27) algorithm: 0=off, 1=zlib, 2=brotli
    int enableSessionTicket;    // session_ticket extension (35); iOS Chrome omits it
    int enablePreSharedKey;     // offer TLS 1.3 resumption (pre_shared_key, 41); iOS Chrome omits it
    int enableHeadersPriority;  // include the PRIORITY block/flag in the HEADERS frame
    const PseudoHeaderType *pseudoHeaderOrder; // order of the 4 request pseudo-headers
} BrowserFingerprint;

// Detects the browser family from a user-agent (BROWSER_UNKNOWN when unsupported).
BrowserType detectBrowseType(const char *ua);

int isChromeUA(const char *ua);

// Resolves a clientHelloId to its fingerprint profile. An explicit "_<version>"
// id pins that profile; an "_auto" id follows the browser major version declared
// by `ua` when a profile exists for it, and falls back to the currently emulated
// version otherwise. `ua` may be NULL. Returns NULL for an unknown id.
const BrowserFingerprint* getBrowserFingerprintById(const char *clientHelloId, const char *ua);

BrowserType browserTypeFromClientHelloId(const char *clientHelloId);

// Returns the fingerprint profile for a browser type, or NULL if unsupported.
const BrowserFingerprint* getBrowserFingerprint(BrowserType type);

#endif //BROWSER_H
