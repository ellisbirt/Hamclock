#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace radio {

inline std::string upper(std::string text) {
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

inline bool locator(const std::string &input, float &lat, float &lon) {
    const std::string s = upper(input);
    if (s.size() != 4 && s.size() != 6) return false;
    if (s[0] < 'A' || s[0] > 'R' || s[1] < 'A' || s[1] > 'R' ||
        s[2] < '0' || s[2] > '9' || s[3] < '0' || s[3] > '9') return false;
    lon = -180 + (s[0] - 'A') * 20 + (s[2] - '0') * 2;
    lat = -90 + (s[1] - 'A') * 10 + (s[3] - '0');
    if (s.size() == 6) {
        if (s[4] < 'A' || s[4] > 'X' || s[5] < 'A' || s[5] > 'X') return false;
        lon += (s[4] - 'A' + 0.5f) / 12;
        lat += (s[5] - 'A' + 0.5f) / 24;
    } else {
        lon += 1;
        lat += 0.5f;
    }
    return true;
}

inline bool callsign(const std::string &s) {
    if (s.size() < 3 || s.size() > 15) return false;
    bool letter = false, digit = false;
    for (unsigned char c : s) {
        if (std::isalpha(c)) letter = true;
        else if (std::isdigit(c)) digit = true;
        else if (c != '/') return false;
    }
    return letter && digit;
}

struct Spot {
    std::string call;
    std::string grid;
    int snr = 0;
    uint64_t frequency = 0;
};

class Reader {
    const uint8_t *data_;
    size_t size_, offset_ = 0;
public:
    bool ok = true;
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    uint64_t number(size_t count) {
        if (!ok || count > 8 || count > size_ - offset_) { ok = false; return 0; }
        uint64_t value = 0;
        while (count--) value = (value << 8) | data_[offset_++];
        return value;
    }
    std::string text() {
        uint32_t count = static_cast<uint32_t>(number(4));
        if (!ok || count == UINT32_MAX) return {};
        if (count > 512 || count > size_ - offset_) { ok = false; return {}; }
        std::string value(reinterpret_cast<const char *>(data_ + offset_), count);
        offset_ += count;
        return value;
    }
};

inline bool decodedSpot(const std::string &message, Spot &spot) {
    char buffer[513];
    if (message.size() > 512) return false;
    std::memcpy(buffer, message.c_str(), message.size() + 1);
    char *save = nullptr;
    std::string previous;
    for (char *word = strtok_r(buffer, " \t\r\n", &save); word;
         word = strtok_r(nullptr, " \t\r\n", &save)) {
        float lat, lon;
        if (upper(word) != "RR73" && locator(word, lat, lon) && callsign(previous)) {
            spot.call = upper(previous);
            spot.grid = upper(word);
            return true;
        }
        previous = word;
    }
    return false;
}

// Accept the stable prefix; newer WSJT-X versions append optional fields.
inline bool wsjtx(const uint8_t *data, size_t size, Spot &output) {
    Reader r(data, size);
    if (r.number(4) != 0xADBCCBDA) return false;
    uint32_t schema = r.number(4);
    if (schema != 2 && schema != 3) return false;
    uint32_t type = r.number(4);
    r.text(); // Instance ID.
    Spot spot;
    if (type == 1) {
        spot.frequency = r.number(8);
        r.text(); // Mode.
        spot.call = upper(r.text());
        r.text(); r.text(); // Report, TX mode.
        r.number(3); // TX enabled, transmitting, decoding.
        r.number(4); r.number(4); // RX/TX offsets.
        r.text(); r.text(); // DE call/grid.
        spot.grid = upper(r.text());
    } else if (type == 2) {
        bool isNew = r.number(1) != 0;
        r.number(4); // QTime, milliseconds since midnight.
        uint32_t snr = r.number(4);
        spot.snr = snr <= INT32_MAX ? static_cast<int>(snr)
                                  : -1 - static_cast<int>(UINT32_MAX - snr);
        r.number(8); r.number(4); // Delta time (double), frequency offset.
        r.text(); // Mode.
        std::string message = r.text();
        r.number(1); r.number(1); // Low confidence, off air.
        if (!isNew || !decodedSpot(message, spot)) return false;
    } else {
        return false;
    }
    float lat, lon;
    if (!r.ok || !callsign(spot.call) || !locator(spot.grid, lat, lon)) return false;
    output = spot;
    return true;
}

struct Beacon {
    std::string receiver;
    std::string call;
    float khz = 0;
    int snr = 0;
};

inline bool beacon(const std::string &line, Beacon &output) {
    char receiver[32], call[32], mode[16], frequency[32], signal[16];
    int consumed = 0;
    if (line.size() > 255 ||
        std::sscanf(line.c_str(), "DX de %31s %31s %31s %15s %15s dB%n",
                    receiver, frequency, call, mode, signal, &consumed) != 5 || !consumed) return false;
    char *end;
    float khz = std::strtof(frequency, &end);
    if (end == frequency || *end) return false;
    long snr = std::strtol(signal, &end, 10);
    if (end == signal || *end) return false;
    std::string rx = upper(receiver);
    if (!rx.empty() && rx.back() == ':') rx.pop_back();
    if (rx.size() >= 2 && rx.substr(rx.size() - 2) == "-#") rx.resize(rx.size() - 2);
    if (!callsign(rx) || !callsign(call) || !std::isfinite(khz) || khz <= 0 ||
        khz > 1000000 || snr < -100 || snr > 100) return false;
    output.receiver = rx;
    output.call = upper(call);
    output.khz = khz;
    output.snr = static_cast<int>(snr);
    return true;
}

inline std::string xmlText(const std::string &xml, const std::string &tag) {
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t start = xml.find(open);
    if (start == std::string::npos) return {};
    start += open.size();
    size_t end = xml.find(close, start);
    if (end == std::string::npos || end - start > 80) return {};
    return xml.substr(start, end - start);
}

struct Solar {
    std::string updated, flux, a, k;
    std::string bands[4][2];
};

inline bool solar(const std::string &xml, Solar &output) {
    if (xml.size() > 16384) return false;
    Solar value;
    value.updated = xmlText(xml, "updated");
    value.flux = xmlText(xml, "solarflux");
    value.a = xmlText(xml, "aindex");
    value.k = xmlText(xml, "kindex");
    const char *names[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};
    for (int b = 0; b < 4; ++b) {
        for (int t = 0; t < 2; ++t) {
            size_t pos = 0;
            while ((pos = xml.find("<band ", pos)) != std::string::npos) {
                size_t end = xml.find('>', pos);
                if (end == std::string::npos) return false;
                std::string attributes = xml.substr(pos, end - pos);
                size_t close = xml.find("</band>", end);
                if (close == std::string::npos) return false;
                if (attributes.find(std::string("name=\"") + names[b] + "\"") != std::string::npos &&
                    attributes.find(t == 0 ? "time=\"day\"" : "time=\"night\"") != std::string::npos) {
                    std::string condition = xml.substr(end + 1, close - end - 1);
                    if (condition != "Good" && condition != "Fair" && condition != "Poor") return false;
                    value.bands[b][t] = condition;
                    break;
                }
                pos = close + 7;
            }
            if (value.bands[b][t].empty()) return false;
        }
    }
    if (value.updated.empty() || value.flux.empty() || value.a.empty() || value.k.empty()) return false;
    output = value;
    return true;
}

inline bool expired(uint32_t now, uint32_t then, uint32_t ttl) {
    return static_cast<uint32_t>(now - then) >= ttl;
}

} // namespace radio
