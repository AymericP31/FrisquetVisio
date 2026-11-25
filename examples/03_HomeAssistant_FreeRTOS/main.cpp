// SPDX-License-Identifier: MIT
//
// Exemple complet : Home Assistant + FreeRTOS, organisé comme le firmware d'origine.
// Tout le travail tourne dans des tâches FreeRTOS (la boucle Arduino est supprimée) :
//
//   Tâche        Prio  Rôle
//   -----------  ----  -----------------------------------------------------------------
//   visio_radio   4    réveillée par l'interruption radio : RX, décodage, émission TX
//   visio_ha      3    toutes les 10 ms : Wi-Fi/MQTT (mqtt.loop) puis visioHA.loop()
//
// La radio a la priorité la plus haute : une reconnexion Wi-Fi/MQTT (qui peut bloquer
// plusieurs secondes dans mqtt.loop) ne retarde jamais une réception ni une émission.
// Le mutex visioMutex protège l'objet visio, utilisé par les deux tâches.
//
// ---------------------------------------------------------------------------------------
// Comment part l'émission radio (TX) ?
//
// L'interruption ne fait QUE réveiller la tâche radio. C'est visio.loop(), dans la tâche
// visio_radio, qui lit la trame, décode et émet. Exemple zone 1 en mode Heat :
//
//   t=0      thermostat -> chaudière   ISR -> réveil -> loop() mémorise la trame (TrNum B8)
//   t≈50 ms  chaudière  -> thermostat  ISR -> réveil -> loop() note « réponse reçue »
//   t=150 ms (TX_DELAY_MS écoulé)      AUCUNE interruption : la tâche se réveille seule
//                                      au bout d'1 tick (timeout de ulTaskNotifyTake),
//                                      loop() voit le délai passé et émet TrNum BC +
//                                      consigne HA. Pendant l'émission (~10 ms)
//                                      l'interruption RX est détachée, puis réarmée.
//   t≈200 ms chaudière  -> thermostat  ISR -> réveil -> loop() : réponse à notre trame,
//                                      TramType = "81: Réponse Chaudière Reçue Zone 1".
//
// D'où la règle : ne jamais attendre avec portMAX_DELAY dans la tâche radio, sinon
// l'émission différée ne partirait qu'à la prochaine trame reçue.
// ---------------------------------------------------------------------------------------
#include <Arduino.h>
#include <ArduinoHA.h>
#include <FrisquetVisio.h>
#include <FrisquetVisioHA.h>
#include <SPI.h>
#include <WiFi.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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
HAMqtt mqtt(wifiClient, device, FrisquetVisioHA::entityCount(ZONE_COUNT, HYDRAULIC_COUNT));
FrisquetVisioHA visioHA(visio);
SemaphoreHandle_t visioMutex = nullptr;  // protège visio entre les deux tâches

// Appelée DANS l'interruption radio : uniquement réveiller la tâche radio.
// Interdit ici : Serial, SPI, MQTT, mutex, allocation mémoire.
void IRAM_ATTR wakeRadioTask(void* context) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(context), &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

// Appelée par visio.loop(), donc dans la tâche radio : trace série puis TramType.
void onRadioEvent(const FrisquetVisio::Event& event, void*) {
    if (event.type == FrisquetVisio::EventType::Receive) {
        Serial.printf("RX [%2u] status=%d RSSI=%.1f :", static_cast<unsigned>(event.length),
                      event.status, event.rssi);
        for (size_t i = 0; i < event.length; ++i) Serial.printf(" %02X", event.data[i]);
        Serial.println();
    } else if (event.type == FrisquetVisio::EventType::Transmit) {
        Serial.printf("TX zone %d status=%d TrNum=%02X\n", event.zone + 1, event.status,
                      event.data[3]);
    }
    FrisquetVisioHA::onRadioEvent(event, &visioHA);  // copie non bloquante pour la tâche HA
}

void radioTask(void*) {
    xSemaphoreTake(visioMutex, portMAX_DELAY);
    SPI.begin(9, 11, 10, 8);  // SCK, MISO, MOSI, NSS
    FrisquetVisio::Config config;
    std::memcpy(config.networkId, NETWORK_ID, sizeof(config.networkId));
    config.zoneCount = ZONE_COUNT;
    for (uint8_t zone = 0; zone < FrisquetVisio::MaxZones; ++zone) {
        config.txDelayMs[zone] = TX_DELAY_MS[zone];
    }
    // Trames CRC : true = traitées normalement, false = ignorées (voir config.example.h).
    config.acceptCrcMismatch = ACCEPT_CRC_MISMATCH;
    visio.onEvent(onRadioEvent);
    // Le hook doit être enregistré AVANT begin(). Contexte = handle de cette tâche.
    const bool hooked = visio.onRadioInterrupt(wakeRadioTask, xTaskGetCurrentTaskHandle());
    const int16_t status = hooked ? visio.begin(config) : RADIOLIB_ERR_UNKNOWN;
    xSemaphoreGive(visioMutex);
    if (status != RADIOLIB_ERR_NONE) {
        Serial.printf("ERREUR initialisation radio : %d\n", status);
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        // Réception, délais et émission : tout se passe dans visio.loop().
        xSemaphoreTake(visioMutex, portMAX_DELAY);
        const int16_t loopStatus = visio.loop();
        const bool ready = visio.isReady();
        xSemaphoreGive(visioMutex);
        if (loopStatus != RADIOLIB_ERR_NONE && loopStatus != RADIOLIB_ERR_CRC_MISMATCH) {
            Serial.printf("ERREUR radio : %d\n", loopStatus);
        }
        if (!ready) {
            Serial.println("Radio arretee ; redemarrer apres diagnostic.");
            vTaskSuspend(nullptr);
        }
        // Dort jusqu'à l'interruption radio OU 1 tick : ce timeout permet l'émission
        // différée même quand aucune trame n'arrive.
        ulTaskNotifyTake(pdTRUE, 1);
    }
}

void homeAssistantTask(void*) {
    bool lastReady = false;
    for (;;) {
        // mqtt.loop() gère la (re)connexion au broker et appelle les callbacks HA
        // (commandes climate), qui ne touchent jamais la radio directement.
        if (WiFi.status() == WL_CONNECTED) mqtt.loop();
        // Applique les commandes à visio (sous mutex, quelques µs) puis publie.
        visioHA.loop();

        // Disponibilité HA = radio prête. ArduinoHA la republie à chaque reconnexion.
        if (xSemaphoreTake(visioMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            const bool ready = visio.isReady();
            xSemaphoreGive(visioMutex);
            if (ready != lastReady) {
                lastReady = ready;
                device.setAvailability(ready);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}

void setup() {
    Serial.begin(115200);
    if (!NETWORK_ID_CONFIGURED || std::strcmp(WIFI_SSID, "CHANGE_ME") == 0
        || std::strcmp(MQTT_HOST, "CHANGE_ME") == 0) {
        Serial.println("ERREUR : renseigner config_local.h (Wi-Fi, MQTT, Network ID).");
        return;
    }
    visioMutex = xSemaphoreCreateMutex();

    WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    device.setUniqueId(mac, sizeof(mac));
    device.setName("Frisquet Visio");
    device.setSoftwareVersion("1.0.0");
    device.enableSharedAvailability();
    device.enableLastWill();  // "offline" si la carte disparaît du réseau
    device.setAvailability(false);
    // Entités HA créées avant mqtt.begin() ; TRAM_BYTES = octets dans TramType.
    visioHA.begin(ZONE_COUNT, HYDRAULIC_COUNT, visioMutex, TRAM_BYTES);
    mqtt.setBufferSize(1024);
    mqtt.begin(MQTT_HOST, MQTT_PORT,
               MQTT_USERNAME[0] ? MQTT_USERNAME : nullptr,
               MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Cœur 1, comme le firmware d'origine (le Wi-Fi d'ESP-IDF tourne sur le cœur 0).
    if (xTaskCreatePinnedToCore(radioTask, "visio_radio", 4096, nullptr, 4, nullptr, 1) != pdPASS
        || xTaskCreatePinnedToCore(homeAssistantTask, "visio_ha", 8192, nullptr, 3, nullptr, 1)
               != pdPASS) {
        Serial.println("ERREUR : creation des taches impossible.");
    }
}

void loop() {
    vTaskDelete(nullptr);  // boucle Arduino inutile : tout tourne dans les tâches ci-dessus
}
