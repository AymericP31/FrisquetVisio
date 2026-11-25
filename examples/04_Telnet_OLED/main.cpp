// SPDX-License-Identifier: MIT
//
// Exemple 04 : diagnostic radio par Telnet (port 23) et écran OLED (Heltec WiFi LoRa 32 V3).
//  - Telnet : trace de chaque trame reçue/émise + commandes
//      radio, radio-zone1..3, radio-chaudiere, radio-hydrau, radio-heltec, radio-init, radio-reset ;
//  - OLED : compteurs radio rafraîchis chaque seconde, bouton PRG = écran allumé/éteint.
// Toutes les zones restent en Auto : écoute seule, aucune trame n'est émise.
#include <Arduino.h>
#include <ESPTelnet.h>
#include <FrisquetVisio.h>
#include <FrisquetVisioStats.h>
#include <SPI.h>
#include <oled/SSD1306Wire.h>  // bibliothèque Heltec ESP32 Dev-Boards
#include <WiFi.h>
#include <cstring>

#if __has_include("config_local.h")
#include "config_local.h"
#else
#include "config.example.h"
#endif

namespace {
constexpr int LoraNss = 8;
constexpr int LoraSck = 9;
constexpr int LoraMosi = 10;
constexpr int LoraMiso = 11;
constexpr int LoraRst = 12;
constexpr int LoraBusy = 13;
constexpr int LoraDio1 = 14;
constexpr int OledSda = 17;
constexpr int OledScl = 18;
constexpr int OledRst = 21;
constexpr int OledVext = 36;  // alimentation écran, active à LOW
constexpr int PrgButton = 0;

SX1262 radio = new Module(LoraNss, LoraDio1, LoraRst, LoraBusy);
FrisquetVisio visio(radio);
FrisquetVisioStats stats;
ESPTelnet telnet;
SSD1306Wire display(0x3c, OledSda, OledScl, OledRst);
bool telnetStarted = false;
bool oledOn = true;

// Telnet attend des fins de ligne \r\n.
void print(const String& text) {
    Serial.println(text);
    String out = text;
    out.replace("\n", "\r\n");
    telnet.print(out + "\r\n");
}

void onRadioEvent(const FrisquetVisio::Event& event, void*) {
    const uint32_t now = millis();
    char line[224];
    if (stats.formatTrace(event, now, line, sizeof(line)) > 0) print(line);
    stats.record(event, now);
}

void onTelnetInput(String command) {
    command.trim();
    if (command == "radio-reset") {
        stats.reset();
        print("Compteurs radio remis a zero");
    } else if (command == "radio" || command.startsWith("radio-") || command.startsWith("radio ")) {
        const String device = command.length() > 6 ? command.substring(6) : String();
        print(stats.report(device, visio.config(), visio.isReady(), millis()));
    } else {
        print("Commandes : radio, radio-zone1..3, radio-chaudiere, radio-hydrau, radio-heltec, "
              "radio-init, radio-reset");
    }
}

void drawOled(uint32_t now) {
    display.clear();
    display.drawString(0, 0, stats.oled(visio.isReady(), visio.config().rxBandwidthKHz, now));
    display.display();
}
}  // namespace

void setup() {
    Serial.begin(115200);

    pinMode(OledVext, OUTPUT);
    digitalWrite(OledVext, LOW);
    delay(50);
    display.init();
    display.setFont(ArialMT_Plain_10);
    pinMode(PrgButton, INPUT_PULLUP);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    telnet.onInputReceived(onTelnetInput);

    if (!NETWORK_ID_CONFIGURED) {
        Serial.println("ERREUR: configurer le Network ID (voir exemple 01_Sniffer).");
        return;
    }
    SPI.begin(LoraSck, LoraMiso, LoraMosi, LoraNss);
    FrisquetVisio::Config config;
    std::memcpy(config.networkId, NETWORK_ID, sizeof(config.networkId));
    visio.onEvent(onRadioEvent);
    const int16_t status = visio.begin(config);
    if (status != RADIOLIB_ERR_NONE) Serial.printf("ERREUR initialisation radio : %d\n", status);
}

void loop() {
    if (!telnetStarted && WiFi.isConnected()) telnetStarted = telnet.begin();
    telnet.loop();
    if (visio.isReady()) visio.loop();

    static bool buttonWasUp = true;
    const bool buttonUp = digitalRead(PrgButton) == HIGH;
    if (buttonWasUp && !buttonUp) {
        oledOn = !oledOn;
        if (oledOn) display.displayOn();
        else display.displayOff();
    }
    buttonWasUp = buttonUp;

    static uint32_t lastOledAt = 0;
    const uint32_t now = millis();
    if (oledOn && now - lastOledAt >= 1000) {
        lastOledAt = now;
        drawOled(now);
    }
    delay(1);
}
