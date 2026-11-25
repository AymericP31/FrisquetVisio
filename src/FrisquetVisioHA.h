// SPDX-License-Identifier: MIT
//
// FrisquetVisioHA : passerelle Home Assistant (MQTT Discovery via ArduinoHA) pour FrisquetVisio.
// Partie optionnelle : compilée seulement si la bibliothèque ArduinoHA est installée
// (dawidchyrzynski/home-assistant-integration). Le Wi-Fi et le broker restent dans le sketch.
//
// Entités créées (mêmes identifiants que le firmware d'origine HAFrisquetVisio) :
//   Zone_N        climate : Auto = thermostat physique, Heat = consigne imposée, Off = hors-gel
//                 température affichée = ambiante renvoyée par la chaudière (comme l'original)
//   ModeZoneN     texte   : mode radio du thermostat (Heure pleine, Hors gel...)
//   Text          nombre  : température extérieure
//   ModeChaudiere texte   : mode chaudière
//   TspEauN       nombre  : consigne d'eau du module hydraulique N
//   TmesEauN      nombre  : départ d'eau mesuré par le module hydraulique N
//   ModeHydN      texte   : mode du module hydraulique N
//   TramType      texte   : dernière trame reçue/émise ("8: Zone 1", "81: Réponse Chaudière",
//                           "Envoie consigne Zone 1"...) pour vérifier le fonctionnement radio
// Les consignes Heat et Off sont sauvegardées en NVS (Preferences "frisquet" / "modesp").
//
// Règles d'appel :
//   - créer HAMqtt AVANT d'appeler begin(), et appeler begin() AVANT mqtt.begin() ;
//   - appeler mqtt.loop() puis visioHA.loop() dans la MÊME tâche ;
//   - brancher TramType : visio.onEvent(FrisquetVisioHA::onRadioEvent, &visioHA)
//     (ou appeler FrisquetVisioHA::onRadioEvent depuis votre propre callback) ;
//   - si visio.loop() tourne dans une autre tâche (FreeRTOS), passer à begin() le
//     mutex qui protège visio. visioHA ne touche visio que sous ce mutex, et jamais
//     pendant un appel MQTT : une reconnexion réseau ne bloque donc pas la radio.
#pragma once

#include <ArduinoHA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "FrisquetVisio.h"

class FrisquetVisioHA {
public:
    // Nombre d'entités créées : à prévoir dans HAMqtt(client, device, N).
    static constexpr uint8_t entityCount(uint8_t zones, uint8_t hydraulicModules) {
        return 3 + zones * 2 + hydraulicModules * 3;
    }

    explicit FrisquetVisioHA(FrisquetVisio& visio);
    FrisquetVisioHA(const FrisquetVisioHA&) = delete;
    FrisquetVisioHA& operator=(const FrisquetVisioHA&) = delete;

    // Crée les entités et relit les consignes NVS. Une seule instance possible.
    // visioMutex : mutex partagé avec la tâche qui appelle visio.loop(), ou nullptr
    // si visio.loop() et visioHA.loop() tournent dans la même tâche.
    // tramBytes : true = TramType contient aussi les octets en hexadécimal (debug,
    // équivalent de ConfTram dans le firmware d'origine).
    bool begin(uint8_t zoneCount = 1, uint8_t hydraulicModuleCount = 1,
               SemaphoreHandle_t visioMutex = nullptr, bool tramBytes = false);
    // Transmet les commandes HA à la radio puis publie les états modifiés.
    void loop();
    // Callback pour visio.onEvent(..., &visioHA). Appelé dans la tâche radio : copie
    // seulement la trame dans une file (non bloquant), publiée ensuite par loop().
    static void onRadioEvent(const FrisquetVisio::Event& event, void* self);

private:
    // Commande HA en attente d'être appliquée à la radio (consignes dans setpoints_).
    struct Request {
        bool dirty = true;
        FrisquetVisio::Mode mode = FrisquetVisio::Mode::Auto;
    };

    // Copie d'un événement radio pour TramType (49 octets = plus longue trame Visio).
    struct RadioRecord {
        uint32_t at;
        int16_t status;
        FrisquetVisio::EventType type;
        int8_t zone;
        uint8_t length;
        float rssi;
        uint8_t data[49];
    };

    static void onModeCommand(HAHVAC::Mode mode, HAHVAC* sender);
    static void onTargetTemperatureCommand(HANumeric temperature, HAHVAC* sender);
    static FrisquetVisioHA* instance_;  // les callbacks ArduinoHA sont statiques
    int8_t zoneIndex(const HAHVAC* sender) const;
    void syncWithRadio();
    void publish();
    void publishTramType();
    void saveSetpoints();

    FrisquetVisio& visio_;
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t radioRecords_ = nullptr;
    bool tramBytes_ = false;
    uint8_t zoneCount_ = 0;
    uint8_t hydraulicCount_ = 0;

    HAHVAC* climates_[FrisquetVisio::MaxZones] = {};
    HASensor* zoneModes_[FrisquetVisio::MaxZones] = {};
    HASensorNumber* outdoor_ = nullptr;
    HASensor* boilerMode_ = nullptr;
    HASensor* tramType_ = nullptr;
    HASensorNumber* waterSetpoints_[FrisquetVisio::MaxHydraulicModules] = {};
    HASensorNumber* waterTemperatures_[FrisquetVisio::MaxHydraulicModules] = {};
    HASensor* hydraulicModes_[FrisquetVisio::MaxHydraulicModules] = {};

    Request requests_[FrisquetVisio::MaxZones];
    // Copie de l'état radio, prise sous mutex puis publiée sans mutex.
    FrisquetVisio::Zone zones_[FrisquetVisio::MaxZones];
    FrisquetVisio::Boiler boiler_;
    FrisquetVisio::Hydraulic hydraulics_[FrisquetVisio::MaxHydraulicModules];
    bool haveSnapshot_ = false;
    // Dernier code publié pour chaque capteur texte (0xFFFF = rien publié).
    uint16_t lastZoneMode_[FrisquetVisio::MaxZones];
    uint16_t lastBoilerMode_ = 0xFFFF;
    uint16_t lastHydraulicMode_[FrisquetVisio::MaxHydraulicModules];
    bool wasConnected_ = false;
    // Consignes par zone et par mode, même format que le firmware d'origine :
    // [zone][0] Auto (non utilisé), [1] Heat, [2] Off.
    float setpoints_[FrisquetVisio::MaxZones][3];
};
