//
// Created by Intuition on 25-8-16.
//

#include <string.h>

#include "BrowserHandler.h"

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