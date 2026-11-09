#include <Arduino.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <atomic>
#include <time.h>
#include "config.h"
#include "radio_data.h"

namespace {
constexpr uint32_t spotTTL = 30 * 60 * 1000;
constexpr uint32_t heatTTL = 60 * 60 * 1000;
constexpr uint32_t solarInterval = 60 * 60 * 1000;
constexpr int mapX = 10, mapY = 48, mapW = 300, mapH = 140;
TFT_eSPI tft;
TFT_eSprite ui(&tft);
WiFiUDP udp;
WiFiClient rbn;
SemaphoreHandle_t solarMutex;
QueueHandle_t beaconQueue = nullptr;
std::atomic<bool> rbnOnline{false};
struct BeaconLine { char text[256]; };
std::string pendingSolar;
radio::Solar solarData;
bool solarReady = false, udpReady = false, displayReady = false;
uint32_t solarReceived = 0, lastDraw = 0, lastPanel = 0, lastRbn = 0;
uint32_t lastWifi = 0, unmapped = 0, reports = 0;
int page = 0;
std::string rbnLine;
bool rbnOverflow = false, rbnLoggedIn = false;
bool rbnPromptSeen = false;
uint32_t rbnConnected = 0;

struct MapSpot {
    radio::Spot spot;
    uint32_t seen = 0;
    bool used = false;
};
MapSpot spots[64];
size_t nextSpot = 0;
struct HeatReport {
    float lat = 0, lon = 0;
    uint32_t seen = 0;
    bool used = false;
};
HeatReport heat[128];
size_t nextHeat = 0;

class SolarSink : public Stream {
public:
    std::string body;
    using Print::write;
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *data, size_t size) override {
        if (size > 16384 - body.size()) return 0;
        body.append(reinterpret_cast<const char *>(data), size);
        return size;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

void solarTask(void *) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(3000);
        http.setTimeout(3000);
        SolarSink sink;
        bool success = false;
        if (http.begin(client, SOLAR_URL)) {
            int status = http.GET();
            int length = http.getSize();
            if (status == HTTP_CODE_OK && length <= 16384 &&
                http.writeToStream(&sink) >= 0) {
                radio::Solar parsed;
                success = radio::solar(sink.body, parsed);
            }
        }
        http.end();
        if (success) {
            xSemaphoreTake(solarMutex, portMAX_DELAY);
            pendingSolar.swap(sink.body);
            xSemaphoreGive(solarMutex);
        }
        Serial.println(success ? "Solar feed updated" : "Solar fetch failed; retaining last data");
        // Honour the publisher's hourly polling policy, including failures.
        vTaskDelay(pdMS_TO_TICKS(solarInterval));
    }
}

void receiveSolar() {
    if (xSemaphoreTake(solarMutex, 0) != pdTRUE) return;
    if (!pendingSolar.empty()) {
        if (radio::solar(pendingSolar, solarData)) {
            solarReady = true;
            solarReceived = millis();
        }
        pendingSolar.clear();
    }
    xSemaphoreGive(solarMutex);
}

void receiveWsjtx() {
    for (int budget = 0; budget < 8; ++budget) {
        int size = udp.parsePacket();
        if (!size) break;
        uint8_t packet[1024];
        if (size > static_cast<int>(sizeof(packet))) { udp.flush(); continue; }
        int count = udp.read(packet, sizeof(packet));
        radio::Spot spot;
        if (count != size || !radio::wsjtx(packet, count, spot)) continue;
        MapSpot *target = nullptr;
        for (auto &entry : spots) {
            if (entry.used && entry.spot.call == spot.call) { target = &entry; break; }
        }
        if (!target) target = &spots[nextSpot++ % 64];
        target->spot = spot;
        target->seen = millis();
        target->used = true;
    }
}

void receiveBeacon(const std::string &line) {
    radio::Beacon report;
    if (!radio::beacon(line, report) || report.call != radio::upper(STATION_CALL)) return;
    ++reports;
    for (const auto &skimmer : skimmers) {
        float lat, lon;
        if (radio::upper(skimmer.call) == report.receiver &&
            radio::locator(skimmer.grid, lat, lon)) {
            HeatReport &entry = heat[nextHeat++ % 128];
            entry.lat = lat;
            entry.lon = lon;
            entry.seen = millis();
            entry.used = true;
            return;
        }
    }
    ++unmapped;
}

void serviceRbn() {
    if (!RBN_ENABLED || !radio::callsign(STATION_CALL)) return;
    uint32_t now = millis();
    if (!rbn.connected()) {
        if (!radio::expired(now, lastRbn, 60000)) return;
        lastRbn = now;
        rbn.stop();
        rbnLine.clear();
        rbnOverflow = rbnLoggedIn = rbnPromptSeen = false;
        if (rbn.connect(RBN_HOST, RBN_PORT, 250)) rbnConnected = now;
        return;
    }
    // Some clusters omit a newline after their login prompt.
    if (!rbnLoggedIn && (rbnPromptSeen || radio::expired(now, rbnConnected, 1500))) {
        rbn.print(STATION_CALL);
        rbn.print("\r\n");
        rbnLoggedIn = true;
    }
    for (int budget = 0; budget < 2048 && rbn.available(); ++budget) {
        char c = rbn.read();
        if (c == '\n') {
            if (!rbnOverflow) {
                BeaconLine line{};
                memcpy(line.text, rbnLine.c_str(), rbnLine.size());
                xQueueSend(beaconQueue, &line, 0);
            }
            rbnLine.clear();
            rbnOverflow = false;
        } else if (c != '\r' && static_cast<unsigned char>(c) >= 32 &&
                   static_cast<unsigned char>(c) < 127) {
            if (rbnLine.size() < 255) {
                rbnLine += c;
                if (rbnLine.find("call:") != std::string::npos ||
                    rbnLine.find("login:") != std::string::npos) rbnPromptSeen = true;
            } else {
                rbnOverflow = true;
            }
        }
    }
}

void rbnTask(void *) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) serviceRbn();
        else rbn.stop();
        rbnOnline.store(rbn.connected());
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void receiveRbn() {
    if (!beaconQueue) return;
    BeaconLine line;
    for (int budget = 0; budget < 8 && xQueueReceive(beaconQueue, &line, 0) == pdTRUE; ++budget)
        receiveBeacon(line.text);
}

int mapLon(float lon) {
    return mapX + constrain(static_cast<int>((lon + 180) * mapW / 360), 0, mapW - 1);
}
int mapLat(float lat) {
    return mapY + constrain(static_cast<int>((90 - lat) * mapH / 180), 0, mapH - 1);
}

struct Point { int16_t lon, lat; };
void outline(const Point *points, size_t count) {
    for (size_t i = 1; i < count; ++i)
        ui.drawLine(mapLon(points[i - 1].lon), mapLat(points[i - 1].lat),
                    mapLon(points[i].lon), mapLat(points[i].lat), TFT_DARKGREEN);
}

void worldMap() {
    ui.drawRect(mapX, mapY, mapW, mapH, TFT_DARKGREY);
    for (int lon = -120; lon <= 120; lon += 60)
        ui.drawFastVLine(mapLon(lon), mapY, mapH, TFT_DARKGREY);
    for (int lat = -60; lat <= 60; lat += 30)
        ui.drawFastHLine(mapX, mapLat(lat), mapW, TFT_DARKGREY);
    // Original schematic outlines; an equirectangular grid, not navigational cartography.
    static const Point north[] = {{-168,70},{-130,72},{-125,50},{-117,32},{-97,16},
        {-80,8},{-86,22},{-65,45},{-55,53},{-80,62},{-100,75},{-168,70}};
    static const Point south[] = {{-80,8},{-50,0},{-35,-8},{-45,-25},{-68,-55},
        {-76,-30},{-80,8}};
    static const Point eurasia[] = {{-10,36},{-10,60},{30,70},{100,77},{180,65},
        {150,50},{130,35},{120,20},{105,0},{80,8},{70,25},{45,30},{35,42},{-10,36}};
    static const Point africa[] = {{-17,35},{10,37},{35,30},{50,12},{40,-15},
        {20,-35},{10,-25},{0,5},{-17,15},{-17,35}};
    static const Point australia[] = {{113,-22},{130,-12},{145,-18},{153,-28},
        {145,-39},{115,-34},{113,-22}};
    static const Point greenland[] = {{-55,60},{-25,70},{-35,83},{-60,80},{-55,60}};
    outline(north, sizeof(north) / sizeof(Point));
    outline(south, sizeof(south) / sizeof(Point));
    outline(eurasia, sizeof(eurasia) / sizeof(Point));
    outline(africa, sizeof(africa) / sizeof(Point));
    outline(australia, sizeof(australia) / sizeof(Point));
    outline(greenland, sizeof(greenland) / sizeof(Point));
    float lat, lon;
    if (radio::locator(STATION_GRID, lat, lon)) {
        int x = mapLon(lon), y = mapLat(lat);
        ui.drawLine(x - 4, y, x + 4, y, TFT_WHITE);
        ui.drawLine(x, y - 4, x, y + 4, TFT_WHITE);
    }
}

void text(const String &value, int x, int y, uint16_t color = TFT_WHITE, int font = 2) {
    ui.setTextColor(color, TFT_BLACK);
    ui.drawString(value, x, y, font);
}

void drawSolar() {
    text("Band outlook: solar indices, not live QSOs", 8, 30, TFT_LIGHTGREY, 1);
    if (!solarReady) {
        text("Waiting for HamQSL solar data", 8, 65, TFT_YELLOW);
        text("Check WiFi / URL; retries hourly", 8, 90);
    } else {
        text("SFI " + String(solarData.flux.c_str()) + "   A " +
             solarData.a.c_str() + "   K " + solarData.k.c_str(), 8, 48);
        text("Band", 8, 74); text("Day", 145, 74); text("Night", 233, 74);
        const char *bands[] = {"80-40m", "30-20m", "17-15m", "12-10m"};
        for (int b = 0; b < 4; ++b) {
            text(bands[b], 8, 96 + b * 21);
            for (int t = 0; t < 2; ++t) {
                const auto &condition = solarData.bands[b][t];
                uint16_t color = condition == "Good" ? TFT_GREEN :
                                 condition == "Fair" ? TFT_YELLOW : TFT_RED;
                text(condition.c_str(), t ? 233 : 145, 96 + b * 21, color);
            }
        }
        text(String(solarData.updated.c_str()).substring(0, 49), 8, 184, TFT_LIGHTGREY, 1);
        uint32_t age = (millis() - solarReceived) / 60000;
        text("Fetched " + String(age) + "m ago" + (age >= 120 ? "  STALE" : ""),
             8, 198, age >= 120 ? TFT_RED : TFT_LIGHTGREY, 1);
    }
    text("Solar: Paul N0NBH / HamQSL.com", 8, 212, TFT_LIGHTGREY, 1);
}

void drawSpots() {
    worldMap();
    int count = 0;
    MapSpot *latest = nullptr;
    for (auto &entry : spots) {
        if (!entry.used || radio::expired(millis(), entry.seen, spotTTL)) continue;
        float lat, lon;
        if (!radio::locator(entry.spot.grid, lat, lon)) continue;
        ++count;
        ui.fillCircle(mapLon(lon), mapLat(lat), 3, TFT_CYAN);
        if (!latest || static_cast<uint32_t>(millis() - entry.seen) <
                       static_cast<uint32_t>(millis() - latest->seen)) latest = &entry;
    }
    text(String(count) + " stations with grids / last 30m", 8, 30, TFT_CYAN, 1);
    if (latest) {
        text(String(latest->spot.call.c_str()) + "  " + latest->spot.grid.c_str(), 8, 192);
        text("Cyan: received/DX target   White: home", 8, 212, TFT_LIGHTGREY, 1);
    } else {
        text("No locatable WSJT-X messages yet", 8, 194, TFT_YELLOW, 1);
        text("UDP " + String(WSJTX_PORT) + "  " + WiFi.localIP().toString(), 8, 210, TFT_LIGHTGREY, 1);
    }
}

void drawHeat() {
    uint16_t cells[9][18] = {};
    int mapped = 0;
    for (const auto &entry : heat) {
        if (!entry.used || radio::expired(millis(), entry.seen, heatTTL)) continue;
        int x = constrain(static_cast<int>((entry.lon + 180) / 20), 0, 17);
        int y = constrain(static_cast<int>((90 - entry.lat) / 20), 0, 8);
        ++cells[y][x];
        ++mapped;
    }
    worldMap();
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 18; ++x) {
        if (!cells[y][x]) continue;
        uint16_t color = cells[y][x] < 3 ? TFT_YELLOW : cells[y][x] < 8 ? TFT_ORANGE : TFT_RED;
        ui.fillRect(mapX + x * mapW / 18 + 1, mapY + y * mapH / 9 + 1,
                    mapW / 18 - 2, mapH / 9 - 2, color);
    }
    text("RBN: receivers hearing " + String(STATION_CALL), 8, 30, TFT_YELLOW, 1);
    if (!RBN_ENABLED) text("RBN disabled: configure call / skimmers", 8, 194, TFT_YELLOW, 1);
    else if (!radio::callsign(STATION_CALL)) text("Set a valid STATION_CALL to use RBN", 8, 194, TFT_RED, 1);
    else text(String(mapped) + " mapped/1h  " + (rbnOnline.load() ? "Connected" : "Reconnecting"),
              8, 194, TFT_LIGHTGREY, 1);
    text("Unmapped " + String(unmapped) + " / " + String(reports) + " total since boot", 8, 207, TFT_LIGHTGREY, 1);
    text("Cell reports: yellow 1-2 / orange 3-7 / red 8+", 8, 219, TFT_LIGHTGREY, 1);
}

void draw() {
    ui.fillSprite(TFT_BLACK);
    const char *titles[] = {"Band conditions", "WSJT-X world map", "Reverse beacon heatmap"};
    text(titles[page], 8, 4, TFT_WHITE);
    time_t now = time(nullptr);
    struct tm utc;
    char clock[12] = "--:--:--";
    if (now > 1700000000 && gmtime_r(&now, &utc)) strftime(clock, sizeof(clock), "%H:%M:%S", &utc);
    text(clock, 244, 7, TFT_CYAN, 1);
    if (page == 0) drawSolar();
    else if (page == 1) drawSpots();
    else drawHeat();
    text(String(page + 1) + "/3  Tap to cycle",
         8, 232, TFT_LIGHTGREY, 1);
    text(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "WiFi offline",
         208, 232, WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, 1);
    ui.pushSprite(0, 0);
}

void serviceWifi() {
    if (WiFi.status() != WL_CONNECTED) {
        if (udpReady) { udp.stop(); udpReady = false; }
        if (radio::expired(millis(), lastWifi, 30000)) {
            lastWifi = millis();
            WiFi.reconnect();
        }
        return;
    }
    if (!udpReady) {
        IPAddress group;
        if (strlen(WSJTX_MULTICAST)) {
            if (group.fromString(WSJTX_MULTICAST))
                udpReady = udp.beginMulticast(group, WSJTX_PORT);
        } else {
            udpReady = udp.begin(WSJTX_PORT);
        }
    }
    if (udpReady) receiveWsjtx();
}
} // namespace

void setup() {
    Serial.begin(115200);
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);
    // Touch IRQ alone suffices for a tap-anywhere action on the resistive variant.
    pinMode(33, OUTPUT);
    digitalWrite(33, HIGH);
    pinMode(36, INPUT);
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    ui.setColorDepth(8);
    displayReady = ui.createSprite(320, 240) != nullptr;
    if (!displayReady) {
        tft.setTextColor(TFT_RED);
        tft.drawString("Display allocation failed", 8, 8, 2);
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    solarMutex = xSemaphoreCreateMutex();
    if (!solarMutex || xTaskCreate(solarTask, "solar", 8192, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("Solar worker allocation failed");
        displayReady = false;
        tft.drawString("Solar worker allocation failed", 8, 8, 2);
        return;
    }
    lastRbn = millis() - 60000;
    if (RBN_ENABLED && radio::callsign(STATION_CALL)) {
        beaconQueue = xQueueCreate(8, sizeof(BeaconLine));
        if (!beaconQueue || xTaskCreate(rbnTask, "rbn", 4096, nullptr, 1, nullptr) != pdPASS) {
            Serial.println("RBN worker allocation failed");
            displayReady = false;
            tft.drawString("RBN worker allocation failed", 8, 8, 2);
            return;
        }
    }
    draw();
}

void loop() {
    if (!displayReady) { delay(1000); return; }
    serviceWifi();
    receiveSolar();
    receiveRbn();
    static bool touching = false;
    static uint32_t lastTap = 0;
    bool pressed = digitalRead(36) == LOW;
    uint32_t now = millis();
    if (pressed && !touching && radio::expired(now, lastTap, 300)) {
        page = (page + 1) % 3;
        lastTap = lastPanel = now;
        draw();
    } else if (radio::expired(now, lastPanel, static_cast<uint32_t>(PANEL_SECONDS) * 1000)) {
        page = (page + 1) % 3;
        lastPanel = now;
        draw();
    }
    touching = pressed;
    if (radio::expired(now, lastDraw, 1000)) {
        lastDraw = now;
        draw();
    }
    delay(5);
}
