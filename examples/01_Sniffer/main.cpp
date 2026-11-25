#include <Arduino.h>
#include <FrisquetVisio.h>
#include <SPI.h>
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

SX1262 radio = new Module(LoraNss, LoraDio1, LoraRst, LoraBusy);
FrisquetVisio visio(radio);

void onRadioEvent(const FrisquetVisio::Event& event, void*) {
    if (event.type != FrisquetVisio::EventType::Receive) return;
    Serial.printf("RX status=%d RSSI=%.1f len=%u :", event.status, event.rssi,
                  static_cast<unsigned>(event.length));
    for (size_t i = 0; i < event.length; ++i) Serial.printf(" %02X", event.data[i]);
    Serial.println();

    if (NETWORK_ID_RECOVERY && event.status == RADIOLIB_ERR_NONE && event.length >= 11
        && (event.data[1] == 0x80 || event.data[1] == 0x84)
        && (event.data[4] & 0x80) == 0 && event.data[5] == 0x41 && event.data[6] == 4
        && !(event.data[7] == 0xFF && event.data[8] == 0xFF
             && event.data[9] == 0xFF && event.data[10] == 0xFF)) {
        Serial.printf("Network ID : %02X %02X %02X %02X\n",
                      event.data[7], event.data[8], event.data[9], event.data[10]);
    }
}
}

void setup() {
    Serial.begin(115200);
    if (!NETWORK_ID_CONFIGURED && !NETWORK_ID_RECOVERY) {
        Serial.println("ERREUR: configurer le Network ID ou activer NETWORK_ID_RECOVERY.");
        return;
    }

    SPI.begin(LoraSck, LoraMiso, LoraMosi, LoraNss);
    FrisquetVisio::Config config;
    if (NETWORK_ID_RECOVERY) {
        std::memset(config.networkId, 0xFF, sizeof(config.networkId));
        Serial.println("Ecoute association uniquement : aucune trame n'est emise.");
    } else {
        std::memcpy(config.networkId, NETWORK_ID, sizeof(config.networkId));
    }
    // Trames en erreur CRC : toutes sont affichées ci-dessous (status=-7).
    // true = traitées normalement (défaut, aucun problème constaté), false = ignorées.
    config.acceptCrcMismatch = true;
    visio.onEvent(onRadioEvent);
    const int16_t status = visio.begin(config);
    if (status != RADIOLIB_ERR_NONE) Serial.printf("ERREUR initialisation radio : %d\n", status);
}

void loop() {
    if (!visio.isReady()) {
        delay(1000);
        return;
    }
    const int16_t status = visio.loop();
    if (status != RADIOLIB_ERR_NONE && status != RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.printf("ERREUR radio : %d\n", status);
    }
    delay(1);
}
