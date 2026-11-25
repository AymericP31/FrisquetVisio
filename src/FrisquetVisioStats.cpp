// SPDX-License-Identifier: MIT
#include "FrisquetVisioStats.h"

#include <cstdio>

namespace {

using Type = FrisquetVisio::EventType;

uint32_t age(uint32_t now, uint32_t at) { return at == 0 ? 0 : now - at; }

bool isHydraulicAddress(uint8_t address) { return address == 0x25 || address == 0x26; }

// Chaudière -> thermostat (49 o), chaudière -> module (19 o consigne, 15 o mode).
bool isBoiler(const uint8_t* d, size_t n) {
    return (n == 49 && d[1] == 0x80 && d[4] == 0x81)
        || (n == 19 && isHydraulicAddress(d[0]) && d[1] == 0x80 && d[4] == 0x03)
        || (n == 15 && isHydraulicAddress(d[0]) && d[1] == 0x80 && d[4] == 0x81);
}

// Module -> chaudière (9 o départ d'eau, 17 o question).
bool isHydraulic(const uint8_t* d, size_t n) {
    return ((n == 9 && d[4] == 0x83) || (n == 17 && d[4] == 0x01))
        && d[0] == 0x80 && isHydraulicAddress(d[1]);
}

size_t appendHex(char* out, size_t size, size_t pos, const uint8_t* data, size_t length) {
    for (size_t i = 0; data != nullptr && i < length && pos + 3 < size; ++i) {
        pos += std::snprintf(out + pos, size - pos, "%02x ", data[i]);
    }
    return pos;
}

const char* yesNo(bool value) { return value ? "oui" : "non"; }

}  // namespace

void FrisquetVisioStats::record(const FrisquetVisio::Event& e, uint32_t now) {
    if (e.type == Type::Receive) {
        if (e.status == RADIOLIB_ERR_CRC_MISMATCH) ++crcErrors;
        Rx& rx = (e.length == 23 && e.zone >= 0) ? satellite[e.zone]
               : isBoiler(e.data, e.length)      ? boiler
               : isHydraulic(e.data, e.length)   ? hydraulic
                                                 : other;
        if (e.status == RADIOLIB_ERR_NONE) ++rx.count;
        else ++rx.errors;
        rx.lastStatus = e.status;
        rx.lastRssi = e.rssi;
        rx.lastAt = now;
        return;
    }
    if (e.zone < 0 || e.zone >= FrisquetVisio::MaxZones) return;
    Tx& t = tx[e.zone];
    switch (e.type) {
    case Type::Transmit:
        if (e.status == RADIOLIB_ERR_NONE) ++t.count;
        else ++t.errors;
        t.lastStatus = e.status;
        t.lastAt = now;
        t.lastDelayMs = now - satellite[e.zone].lastAt;
        t.lastConfirmed = false;
        break;
    case Type::TxConfirmed:
        ++t.confirmed;
        t.lastConfirmed = true;
        t.lastConfirmRssi = e.rssi;
        t.lastConfirmDelayMs = now - t.lastAt;
        t.lastConfirmAt = now;
        break;
    case Type::ResponseTimeout: ++t.timeouts; break;
    case Type::TransactionExpired: ++t.expired; break;
    default: break;
    }
}

size_t FrisquetVisioStats::formatTrace(const FrisquetVisio::Event& e, uint32_t now, char* out,
                                       size_t size) const {
    if (out == nullptr || size == 0) return 0;
    if (e.type != Type::Receive && (e.zone < 0 || e.zone >= FrisquetVisio::MaxZones)) return 0;
    const unsigned zone = static_cast<unsigned>(e.zone + 1);
    int n = 0;
    switch (e.type) {
    case Type::Receive:
        n = std::snprintf(out, size, "RX st=%d len=%u rssi=%.1f Time: %lums: ", e.status,
                          static_cast<unsigned>(e.length), e.rssi, static_cast<unsigned long>(now));
        break;
    case Type::Transmit:
        n = std::snprintf(out, size, "%sDelay %lums: \nSent Zone %u: ",
                          e.status == RADIOLIB_ERR_NONE ? "" : "TX ERREUR ",
                          static_cast<unsigned long>(now - satellite[e.zone].lastAt), zone);
        break;
    case Type::TxConfirmed:
        n = std::snprintf(out, size, "Zone %u: confirmation chaudiere %lu ms, RSSI %.1f dBm",
                          zone, static_cast<unsigned long>(now - tx[e.zone].lastAt), e.rssi);
        break;
    case Type::ResponseTimeout:
        n = std::snprintf(out, size, "Zone %u: confirmation chaudiere expiree", zone);
        break;
    case Type::TransactionExpired:
        n = std::snprintf(out, size, "Zone %u: pas de reponse chaudiere au thermostat, pas de TX",
                          zone);
        break;
    }
    if (n < 0) return 0;
    const size_t pos = static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
    if (e.type != Type::Receive && e.type != Type::Transmit) return pos;
    return appendHex(out, size, pos, e.data, e.length);
}

String FrisquetVisioStats::report(const String& requested, const FrisquetVisio::Config& config,
                                  bool radioOk, uint32_t now) const {
    String device = requested;
    device.trim();
    device.toLowerCase();
    if (device.length() == 0) device = "all";
    int zone = -1;  // zone1, z1, zone 1...
    if (device.startsWith("z")) {
        const char c = device[device.length() - 1];
        if (c >= '1' && c <= '3') zone = c - '1';
    }

    String r = "\nRadio metrics";
    if (device != "all") r += " [" + device + "]";
    r += ":\n";

    auto radio = [&]() {
        r += "  Radio " + String(radioOk ? "OK" : "KO") + ": freq=" + String(config.frequencyMHz, 3);
        r += " MHz, RxBw=" + String(config.rxBandwidthKHz, 1) + " kHz, puissance=";
        r += String(config.outputPowerDbm) + " dBm, gain boost=" + yesNo(config.boostedGain);
        r += ", zones=" + String(config.zoneCount) + "\n";
    };
    auto rx = [&](const String& name, const Rx& m) {
        r += "  " + name + ": RX=" + String(m.count);
        r += ", RX errors=" + String(m.errors);
        r += ", status=" + String(m.lastStatus);
        r += ", RSSI=" + String(m.lastRssi, 1) + " dBm";
        r += ", age=" + String(age(now, m.lastAt)) + " ms\n";
    };
    auto sim = [&](uint8_t z) {
        const Tx& t = tx[z];
        r += "  Satellite simule Zone " + String(z + 1) + " (Heltec -> chaudiere):\n";
        r += "    Emission: TX=" + String(t.count) + ", TX errors=" + String(t.errors);
        r += ", status=" + String(t.lastStatus) + ", age=" + String(age(now, t.lastAt)) + " ms";
        r += ", delai satellite -> TX=" + String(t.lastDelayMs) + " ms";
        r += ", delai configure=" + String(config.txDelayMs[z]) + " ms\n";
        r += "    Confirmation derniere=" + String(yesNo(t.lastConfirmed));
        r += ", reponses valides=" + String(t.confirmed);
        r += ", sans reponse=" + String(t.timeouts);
        r += ", RSSI=" + String(t.lastConfirmRssi, 1) + " dBm";
        r += ", delai=" + String(t.lastConfirmDelayMs) + " ms";
        r += ", age=" + String(age(now, t.lastConfirmAt)) + " ms";
        r += ", thermostat sans reponse chaudiere=" + String(t.expired) + "\n";
    };

    if (device == "init") {
        radio();
    } else if (device == "all") {
        radio();
        r += "\n  Satellites reels (RX Heltec):\n";
        for (uint8_t z = 0; z < config.zoneCount; ++z) rx("Zone " + String(z + 1), satellite[z]);
        r += "\n  Appareils radio reels (RX Heltec):\n";
        rx("Chaudiere", boiler);
        rx("Module hydraulique", hydraulic);
        r += "\n  Satellite simule / emissions Heltec:\n";
        for (uint8_t z = 0; z < config.zoneCount; ++z) sim(z);
        r += "\n  CRC (toutes trames)=" + String(crcErrors) + "\n";
        rx("Trames non attribuees", other);
    } else if (zone >= 0) {
        if (zone >= config.zoneCount) {
            r += "  Zone " + String(zone + 1) + " inactive (zoneCount=" + String(config.zoneCount) + ")\n";
        } else {
            rx("Satellite reel Zone " + String(zone + 1) + " (RX Heltec)", satellite[zone]);
            sim(static_cast<uint8_t>(zone));
        }
    } else if (device == "chaudiere" || device == "boiler") {
        rx("Chaudiere (RX Heltec)", boiler);
        r += "  Reponses aux satellites simules:\n";
        for (uint8_t z = 0; z < config.zoneCount; ++z) {
            const Tx& t = tx[z];
            r += "    Zone " + String(z + 1) + ": confirme=" + yesNo(t.lastConfirmed);
            r += ", valides=" + String(t.confirmed) + ", sans reponse=" + String(t.timeouts);
            r += ", RSSI=" + String(t.lastConfirmRssi, 1) + " dBm";
            r += ", delai=" + String(t.lastConfirmDelayMs) + " ms\n";
        }
    } else if (device == "hydrau" || device == "hydraulique" || device == "module") {
        rx("Module hydraulique (RX Heltec)", hydraulic);
    } else if (device == "heltec" || device == "tx") {
        r += "  Emissions par satellite simule:\n";
        for (uint8_t z = 0; z < config.zoneCount; ++z) sim(z);
    } else if (device == "help") {
        r += "  Appareils: zone1, zone2, zone3, chaudiere, hydrau, heltec, init\n";
    } else {
        r += "  Appareil inconnu. Utiliser: zone1, zone2, zone3, chaudiere, hydrau, heltec, init\n";
    }
    r += "  Status 0 = RADIOLIB_ERR_NONE, -7 = CRC\n";
    return r;
}

String FrisquetVisioStats::oled(bool radioOk, float rxBandwidthKHz, uint32_t now) const {
    auto line = [&](const char* name, const Rx& m) {
        const uint32_t total = m.count + m.errors;
        const float errPct = total ? 100.0f * m.errors / total : 0.0f;
        return String(name) + " " + String(m.count) + " " + String(errPct, 1) + "% "
            + String(static_cast<int>(m.lastRssi)) + " " + String(age(now, m.lastAt) / 1000) + "s\n";
    };
    uint32_t txErrors = 0;
    for (const Tx& t : tx) txErrors += t.errors + t.timeouts;
    return String("Radio ") + (radioOk ? "OK" : "KO") + "  RxBw " + String(rxBandwidthKHz, 1) + "\n"
        + line("Ch", boiler) + line("Hy", hydraulic) + line("Z1", satellite[0]) + line("Z2", satellite[1])
        + "CRC " + String(crcErrors) + "  TX err " + String(txErrors);
}
