# Hamclock for ESP32-2432S028R

A standalone, compact ham-radio dashboard for the original **Cheap Yellow
Display**: ESP32-2432S028R, 320×240 ILI9341 TFT and XPT2046 resistive touch.
This is new firmware, not a port of the desktop HamClock application.
The capacitive-touch `C` variant and boards with ST7789 displays need different
drivers/pins and are not supported by this configuration.

## Build and flash

Install [PlatformIO](https://platformio.org/install), then create
`include/secrets.h` locally (it is git-ignored):

```cpp
#pragma once
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define STATION_CALL "YOUR_CALLSIGN"
#define STATION_GRID "YOUR_MAIDENHEAD_GRID"
#define RBN_ENABLED true
```

Use a valid station callsign and a four- or six-character Maidenhead locator.
The ESP32 requires 2.4 GHz WiFi. From the repository root:

```sh
pio run
pio run --target upload
pio device monitor
```

The USB serial driver/cable must support data. PlatformIO can auto-detect the
port; use `--upload-port` if necessary. Credentials are compiled into the device,
so do not distribute your configured firmware binary or commit `secrets.h`.
Defaults and optional overrides are in `include/config.h`.

TFT wiring is SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, reset -1, backlight 21.
Touch uses a separate SPI bus; this app only needs its IRQ (36) for tap-anywhere
page switching, with touch CS (33) held high. No touch calibration is required.

## Pages

Each page runs for 15 seconds (`PANEL_SECONDS`); tap anywhere to advance and
restart the timer. The header shows NTP-synchronized UTC. Cached information
remains visible when WiFi drops, with connection and age indicators.

1. **Band conditions** — SFI, A/K indices and day/night outlook for 80–40,
   30–20, 17–15 and 12–10 m. These are propagation estimates, not measurements
   of activity at your station. Data: [Paul N0NBH / HamQSL.com](https://www.hamqsl.com/solar.html).
   The published XML endpoint is `www.hamqsl.com/solarxml.php`, not
   `hamqsl.net`. Polling is hourly, including retries; last good data is retained
   and labelled stale after two hours without a successful fetch.
   Fetches run in a separate task so the display and UDP listener remain active.
   The default endpoint uses **unencrypted HTTP for public, read-only data**:
   it cannot authenticate the feed, so treat the outlook as advisory. No
   credentials are sent. Responses are capped at 16 KiB and malformed data
   cannot replace the cache. `SOLAR_URL` can point at a trusted local HTTP mirror;
   this firmware does not implement HTTPS trust configuration.
2. **WSJT-X world map** — cyan dots for received stations whose messages contain
   a grid, plus the current DX target from Status messages when its grid is
   known. A white cross marks your configured home grid. The latest station's
   callsign/grid appears below the schematic world map. Up to 64 stations are
   cached for 30 minutes. Stations without grids are deliberately not guessed.
   Historical/replayed Decode messages are ignored.
3. **Reverse beacon heatmap** — counts of reports **hearing your configured
   transmitting callsign**, grouped by receiver location in 20° cells. Yellow
   means 1–2 reports, orange 3–7, red 8+. This measures report density, not RF
   coverage or signal strength. The window contains at most the latest 128
   mapped reports, expiring after one hour. Unmapped/total counts are since boot.

### WSJT-X setup

In WSJT-X **Settings → Reporting → UDP Server**, set the server to the ESP32's
IP address (shown on the screen) and port **2237** (`WSJTX_PORT`). Enable UDP
messages. You can instead send LAN broadcasts if your network allows them, or
set the same multicast group in WSJT-X and `WSJTX_MULTICAST` in `secrets.h`,
for example `"239.255.0.1"`. Leave `WSJTX_MULTICAST` empty for unicast/broadcast.
The firmware is a passive listener, not a rig-control or reply service.

Schemas 2 and 3, Status (1) and Decode (2) are supported. Decode text such as
`CQ K1ABC FN42` or `W1AW K1ABC FN42` supplies the transmitting callsign/grid.
Signal reports, `RRR`, `RR73`, and messages without a locator do not create dots.
Use a DHCP reservation for a stable device IP. Keep UDP on your trusted LAN;
there is no authentication in the WSJT-X protocol. Do not forward it from the
Internet.

### Reverse Beacon Network setup

RBN is disabled by default. Set `RBN_ENABLED true` and a valid `STATION_CALL`.
The firmware logs in with that callsign and reconnects with a one-minute
backoff. The defaults are `telnet.reversebeacon.net:7000` (CW/RTTY); override
`RBN_PORT` to `7001` for FT8 or use a compatible downstream cluster via
`RBN_HOST` / `RBN_PORT`.

**RBN telnet reports contain no receiver coordinates.** Populate `skimmers[]`
in `include/config.h` with verified receiver callsigns and grids, for example:

```cpp
static constexpr SkimmerLocation skimmers[] = {
    {"W1ABC", "FN42"},  // Example only: replace with a verified receiver/grid.
    {"G1ABC", "IO91"}
};
```

The `-#` skimmer suffix is removed before matching. Only the heard callsign is
matched to `STATION_CALL`, not the receiver or cluster login. Reports from
unknown receivers count as unmapped and **do not get invented locations**.
No reports can appear until your station is actually heard by a receiver.
Direct RBN relays have no server-side filters; the app filters locally and
uses bounded buffers/work per loop. For busy feeds, a filtered downstream
cluster is preferable; excessive traffic may be dropped. See
[RBN telnet service guidance](https://www.reversebeacon.net/pages/Telnet+servers+30).
Telnet is unencrypted; use only your public radio callsign, never a password.

## Host protocol tests

The parsers are independent of Arduino and can be tested without a board or
third-party test framework. With GCC installed, from the repository root:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -g -I include test/test_radio_data.cpp \
    -o /tmp/hamclock-radio-tests
/tmp/hamclock-radio-tests
```

Tests cover locator bounds, real-format WSJT-X Status/Decode packets, truncated
and oversized fields, sign-offs, RBN spot filtering inputs, solar XML validation,
cache preservation, and timer wraparound.

Hardware smoke test: flash, confirm landscape display/tap cycling, WiFi IP and
UTC; send WSJT-X decodes with grids and verify map dots; configure a verified
skimmer and transmit where it can hear you, then verify RBN heat cells. Disconnect
WiFi to check offline/cache behaviour. The world outlines are deliberately
schematic, not detailed geographic boundaries. No account-based callsign
geolocation or discontinued HamClock backend is required.