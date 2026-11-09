#pragma once

// Put private overrides in include/secrets.h (ignored by git).
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef STATION_CALL
#define STATION_CALL ""
#endif
#ifndef STATION_GRID
#define STATION_GRID ""
#endif
#ifndef WSJTX_PORT
#define WSJTX_PORT 2237
#endif
// Empty for unicast/broadcast; set to e.g. "239.255.0.1" for multicast.
#ifndef WSJTX_MULTICAST
#define WSJTX_MULTICAST ""
#endif
#ifndef RBN_HOST
#define RBN_HOST "telnet.reversebeacon.net"
#endif
#ifndef RBN_PORT
#define RBN_PORT 7000
#endif
#ifndef RBN_ENABLED
#define RBN_ENABLED false
#endif
#ifndef PANEL_SECONDS
#define PANEL_SECONDS 15
#endif

// Public, read-only XML; never send credentials to this endpoint.
#ifndef SOLAR_URL
#define SOLAR_URL "http://www.hamqsl.com/solarxml.php"
#endif

struct SkimmerLocation {
    const char *call;
    const char *grid;
};
// RBN sends no coordinates. Add verified receiver locations here.
static constexpr SkimmerLocation skimmers[] = {
    {"", ""}
};
