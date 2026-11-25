// SPDX-License-Identifier: MIT
//
// Exemple simple Home Assistant : radio + MQTT dans la boucle Arduino, sans tâche dédiée.
// Crée 1 thermostat (zone 1), les capteurs chaudière / module hydraulique 1 et TramType.
//
// Limite : pendant une reconnexion Wi-Fi ou MQTT, mqtt.loop() peut bloquer quelques
// secondes et des trames radio peuvent être manquées. Pour une installation réelle,
// utiliser 03_HomeAssistant_FreeRTOS (tâches radio et HA séparées).
#include <Arduino.h>
#include <ArduinoHA.h>
#include <FrisquetVisio.h>
#include <FrisquetVisioHA.h>
#include <SPI.h>
#include <WiFi.h>
#include <cstring>

#if __has_include("config_local.h")
#include "config_local.h"
#else
#include "config.example.h"
#endif

namespace {
SX1262 radio = new Module(8, 14, 12, 13);  // NSS, DIO1, RESET, BUSY (Heltec V3/V4)
FrisquetVisio visio(radio);
WiFiClient wifiClient;
HADevice device;
HAMqtt mqtt(wifiClient, device, FrisquetVisioHA::entityCount(1, 1));
FrisquetVisioHA visioHA(visio);
bool started = false;
}

void setup() {
    Serial.begin(115200);
    if (!NETWORK_ID_CONFIGURED || std::strcmp(WIFI_SSID, "CHANGE_ME") == 0
        || std::strcmp(MQTT_HOST, "CHANGE_ME") == 0) {
        Serial.println("ERREUR : renseigner config_local.h (Wi-Fi, MQTT, Network ID).");
        return;
    }

    // Radio : valeurs par défaut de la bibliothèque, seul le Network ID est fourni.
    SPI.begin(9, 11, 10, 8);  // SCK, MISO, MOSI, NSS
    FrisquetVisio::Config config;
    std::memcpy(config.networkId, NETWORK_ID, sizeof(config.networkId));
    // Trames en erreur CRC : true = traitées normalement comme une trame correcte (défaut,
    // aucun problème constaté) ; false = ignorées. Visible dans TramType « (CRC non valide) ».
    config.acceptCrcMismatch = true;
    // TramType : chaque trame reçue/émise est publiée dans HA pour vérifier la radio.
    visio.onEvent(FrisquetVisioHA::onRadioEvent, &visioHA);
    const int16_t status = visio.begin(config);
    if (status != RADIOLIB_ERR_NONE) {
        Serial.printf("ERREUR initialisation radio : %d\n", status);
        return;
    }

    // Home Assistant : appareil identifié par l'adresse MAC, puis entités Frisquet.
    WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("Frisquet Visio");
    visioHA.begin();  // 1 zone, 1 module hydraulique
    mqtt.setBufferSize(1024);  // la découverte du climate dépasse le tampon par défaut
    mqtt.begin(MQTT_HOST, MQTT_PORT,
               MQTT_USERNAME[0] ? MQTT_USERNAME : nullptr,
               MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    started = true;
}

void loop() {
    if (!started) {
        delay(1000);
        return;
    }
    // 1) Radio : réception, décodage et émission éventuelle de la trame simulée.
    const int16_t status = visio.loop();
    if (status != RADIOLIB_ERR_NONE && status != RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.printf("ERREUR radio : %d\n", status);
    }
    // 2) MQTT : reçoit les commandes HA (mode, consigne).
    if (WiFi.status() == WL_CONNECTED) mqtt.loop();
    // 3) Applique les commandes à la radio et publie les états vers HA.
    visioHA.loop();
    delay(1);
}
