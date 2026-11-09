#include "radio_data.h"
#include <cassert>
#include <iostream>
#include <vector>

using Bytes = std::vector<uint8_t>;
void number(Bytes &b, uint64_t value, int size) {
    for (int i = size - 1; i >= 0; --i) b.push_back((value >> (i * 8)) & 255);
}
void text(Bytes &b, const std::string &value) {
    number(b, value.size(), 4);
    b.insert(b.end(), value.begin(), value.end());
}
Bytes header(int type, int schema = 3) {
    Bytes b;
    number(b, 0xADBCCBDA, 4); number(b, schema, 4); number(b, type, 4);
    text(b, "WSJT-X");
    return b;
}
Bytes decode(const std::string &message, bool isNew = true) {
    Bytes b = header(2);
    number(b, isNew, 1); number(b, 12000, 4); number(b, uint32_t(-17), 4);
    number(b, 0, 8); number(b, 1000, 4);
    text(b, "~"); text(b, message); number(b, 0, 1); number(b, 0, 1);
    return b;
}

int main() {
    float lat, lon;
    assert(radio::locator("IO91wm", lat, lon));
    assert(std::abs(lat - 51.52083f) < 0.001f && std::abs(lon + 0.125f) < 0.001f);
    assert(radio::locator("AA00", lat, lon) && lat == -89.5f && lon == -179);
    assert(radio::locator("RR99xx", lat, lon) && lat < 90 && lon < 180);
    for (auto invalid : {"", "IO", "SS00", "IO9A", "IO91yy", "IO91w", "IO91wm00"})
        assert(!radio::locator(invalid, lat, lon));
    assert(radio::expired(50, UINT32_MAX - 49, 100));
    assert(!radio::expired(50, UINT32_MAX - 49, 101));

    radio::Spot spot;
    Bytes b = decode("CQ DX K1ABC FN42");
    assert(radio::wsjtx(b.data(), b.size(), spot));
    assert(spot.call == "K1ABC" && spot.grid == "FN42" && spot.snr == -17);
    for (size_t n = 0; n < b.size(); ++n) assert(!radio::wsjtx(b.data(), n, spot));
    b = decode("W1AW K1ABC IO91wm");
    assert(radio::wsjtx(b.data(), b.size(), spot) && spot.call == "K1ABC");
    for (auto message : {"CQ K1ABC", "K1ABC W1AW -17", "CQ K1ABC RR73",
                         "CQ K1ABC RRR", "CQ K1ABC SS00", "CQ DX FN42"}) {
        b = decode(message);
        assert(!radio::wsjtx(b.data(), b.size(), spot));
    }
    b = decode("CQ K1ABC FN42", false);
    assert(!radio::wsjtx(b.data(), b.size(), spot));
    b = header(1, 2);
    number(b, 14074000, 8);
    text(b, "FT8"); text(b, "W1AW"); text(b, "-10"); text(b, "FT8");
    number(b, 0, 3); number(b, 1000, 4); number(b, 1000, 4);
    text(b, "K1ABC"); text(b, "FN42"); text(b, "FN31");
    assert(radio::wsjtx(b.data(), b.size(), spot) && spot.frequency == 14074000);
    for (size_t n = 0; n < b.size(); ++n) assert(!radio::wsjtx(b.data(), n, spot));
    b.push_back(0); // Optional fields must be tolerated.
    assert(radio::wsjtx(b.data(), b.size(), spot));
    b[7] = 1;
    assert(!radio::wsjtx(b.data(), b.size(), spot));
    b = header(0); assert(!radio::wsjtx(b.data(), b.size(), spot));
    b = header(2); number(b, 1, 1); number(b, 0, 4); number(b, 0, 4);
    number(b, 0, 8); number(b, 0, 4); number(b, UINT32_MAX - 1, 4);
    assert(!radio::wsjtx(b.data(), b.size(), spot));

    radio::Beacon beacon;
    assert(radio::beacon("DX de KM3T-#:    14025.1  W1AW CW 23 dB 22 WPM CQ 1234Z", beacon));
    assert(beacon.receiver == "KM3T" && beacon.call == "W1AW" && beacon.snr == 23);
    assert(radio::beacon("DX de W1ABC: 7074.0 k1abc FT8 -12 dB CQ 1234Z", beacon));
    assert(beacon.call == "K1ABC");
    assert(!radio::beacon("login:", beacon));
    assert(!radio::beacon("DX de KM3T-#: nan W1AW CW 23 dB", beacon));
    assert(!radio::beacon("DX de KM3T-#: 14025 W1AW CW 999 dB", beacon));
    assert(!radio::beacon("DX de KM3T-#: 14025 W1AW CW 23", beacon));
    assert(!radio::beacon("DX de KM3T-#: 14025 W1AW CW 999999999999999999999 dB", beacon));
    assert(!radio::beacon("DX de KM3T-#: 14025x W1AW CW 23 dB", beacon));
    assert(!radio::beacon(std::string(256, 'x'), beacon));

    std::string xml = "<solar><updated>09 Nov 2026 1000 GMT</updated>"
        "<solarflux>150</solarflux><aindex>5</aindex><kindex>2</kindex>";
    for (const auto *name : {"80m-40m", "30m-20m", "17m-15m", "12m-10m"})
        for (const auto *time : {"day", "night"})
            xml += std::string("<band time=\"") + time + "\" name=\"" + name + "\">Good</band>";
    xml += "</solar>";
    radio::Solar solar;
    assert(radio::solar(xml, solar) && solar.flux == "150" && solar.bands[3][1] == "Good");
    assert(!radio::solar("<solar></solar>", solar));
    assert(!radio::solar(std::string(16385, 'x'), solar));
    size_t pos = xml.find(">Good<");
    xml.replace(pos, 6, ">Broken<");
    assert(!radio::solar(xml, solar));
    assert(solar.bands[0][0] == "Good"); // Failed input must not replace cached data.
    std::cout << "Radio data tests passed\n";
}
