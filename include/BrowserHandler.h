//
// Created by Intuition on 25-8-16.
//

#ifndef BROWSER_H
#define BROWSER_H

static const unsigned char SETTINGS_FRAME_CHROME[] = {
    0x00, 0x00, 0x18,           // Length: 24 bytes (0x18)
    0x04,                       // Type: SETTINGS (4)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0

    // Settings payload
    0x00, 0x01,             // HEADER_TABLE_SIZE
    0x00, 0x01, 0x00, 0x00, // 65536

    0x00, 0x02,             // ENABLE_PUSH
    0x00, 0x00, 0x00, 0x00, // 0 disable

    0x00, 0x04,             // INITIAL_WINDOW_SIZE
    0x00, 0x60, 0x00, 0x00, // 6291456

    0x00, 0x06,             // MAX_HEADER_LIST_SIZE
    0x00, 0x04, 0x00, 0x00, // 262144
};

static const int HEADER_VALUE_MAX_LENGTH_CHROME = 4096;

static const unsigned char WINDOW_UPDATE_FRAME_CHROME[] = {
    0x00, 0x00, 0x04,           // Length: 4
    0x08,                       // Type: WINDOW_UPDATE (8)
    0x00,                       // Flags: none
    0x00, 0x00, 0x00, 0x00,     // Stream identifier: 0
    0x00, 0xEF, 0x00, 0x01      // Increment: 15663105
};

typedef enum {
    BROWSER_UNKNOWN = 0,
    BROWSER_CHROME,
    BROWSER_FIREFOX,
    BROWSER_SAFARI,
    BROWSER_EDGE,
    BROWSER_OPERA,
    BROWSER_IE
} BrowserType;

int isChromeUA(const char *ua);

BrowserType detectBrowseType(const char *ua);

#endif //BROWSER_H
