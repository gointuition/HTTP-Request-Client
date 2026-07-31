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
    int enablePermuteExtensions; // ClientHello extension permutation, Chrome-only
    int enableEchGrease;         // Chrome-only ECH GREASE
    int enableAlps;             // Chrome-only application settings (ALPS)
    int certCompressionAlg;     // compress_certificate (27) algorithm: 0=off, 1=zlib, 2=brotli
    int enableSessionTicket;    // session_ticket extension (35); iOS Chrome omits it
    int enablePreSharedKey;     // offer TLS 1.3 resumption (pre_shared_key, 41); iOS Chrome omits it
    int enableHeadersPriority;  // include the PRIORITY block/flag in the HEADERS frame
    const PseudoHeaderType *pseudoHeaderOrder; // order of the 4 request pseudo-headers
} BrowserFingerprint;

int isChromeUA(const char *ua);

BrowserType detectBrowseType(const char *ua);

// Returns the fingerprint profile for a browser type, or NULL if unsupported.
const BrowserFingerprint* getBrowserFingerprint(BrowserType type);

const BrowserFingerprint* getBrowserFingerprintById(const char *clientHelloId);

BrowserType browserTypeFromClientHelloId(const char *clientHelloId);

#endif //BROWSER_H
