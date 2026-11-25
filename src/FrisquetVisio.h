// SPDX-License-Identifier: MIT
//
// FrisquetVisio : partie RADIO seule (aucune dépendance Wi-Fi, MQTT ou Home Assistant).
//
// Rôle :
//  - écouter le réseau radio Frisquet Visio (FSK 868,96 MHz) avec un SX1262 ;
//  - décoder les trames thermostats (satellites), chaudière et modules hydrauliques ;
//  - piloter une zone en renvoyant la trame du thermostat, modifiée (consigne + mode),
//    juste après la réponse de la chaudière au vrai thermostat.
//
// Adresses radio (octets 0 = destinataire, 1 = émetteur) :
//   0x80 chaudière, 0x08/0x09/0x0A thermostats zones 1/2/3,
//   0x25/0x26 modules hydrauliques 1/2 (0x26 déduit, non observé).
//
// Utilisation minimale :
//   visio.begin(config);           // une fois
//   for (;;) visio.loop();         // le plus souvent possible, toujours depuis la même tâche
//
// Toutes les méthodes publiques doivent être appelées depuis UNE seule tâche
// (ou sous un mutex commun). Seule l'interruption radio interne tourne ailleurs.
#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <cmath>
#include <cstddef>
#include <cstdint>

class FrisquetVisio {
public:
    static constexpr uint8_t MaxZones = 3;             // thermostats 0x08..0x0A
    static constexpr uint8_t MaxHydraulicModules = 2;  // modules 0x25..0x26
    static constexpr uint16_t MaxTxDelayMs = 1000;

    // Codes d'erreur propres à la bibliothèque (les autres codes viennent de RadioLib).
    static constexpr int16_t InvalidConfig = -32000;
    static constexpr int16_t NotReady = -32001;
    static constexpr int16_t RadioInUse = -32002;

    // Mode demandé pour une zone :
    //  Auto : aucune émission, le thermostat physique décide ;
    //  Heat : la carte impose la consigne heatSetpoint ;
    //  Off  : la carte impose le hors-gel frostSetpoint.
    enum class Mode : uint8_t { Auto, Heat, Off };

    // Événements transmis au callback onEvent(), toujours dans la tâche qui appelle loop().
    enum class EventType : uint8_t {
        Receive,             // trame reçue (status = code RadioLib, CRC compris)
        Transmit,            // trame simulée émise (status = résultat de l'émission)
        TransactionExpired,  // thermostat reçu mais pas de réponse chaudière en 10 s
        ResponseTimeout,     // trame simulée émise mais pas de réponse chaudière en 2 s
        TxConfirmed          // réponse chaudière à la trame simulée (data = réponse,
                             // notifié juste AVANT l'événement Receive de la même trame)
    };

    struct Config {
        uint8_t networkId[4] = {};       // Network ID de l'installation (mot de synchro radio)
        uint8_t zoneCount = MaxZones;    // nombre de thermostats gérés (1 à 3)
        float frequencyMHz = 868.96f;
        float rxBandwidthKHz = 187.2f;
        int8_t outputPowerDbm = 10;
        bool boostedGain = true;         // gain RX renforcé du SX1262
        // Délai minimal (0..1000 ms) entre la trame du thermostat et l'émission simulée.
        // L'émission attend en plus, toujours, la réponse de la chaudière au thermostat.
        uint16_t txDelayMs[MaxZones] = {150, 150, 150};
        // Trames reçues avec erreur CRC :
        //  true  (défaut) : traitées normalement si les octets exploités sont dans une
        //                   plage cohérente (températures, modes), sinon ignorées ;
        //  false : toujours ignorées (seulement signalées par onEvent / le code retour de loop()).
        // L'en-tête (adresses, type) est de toute façon contrôlé au décodage.
        bool acceptCrcMismatch = true;
    };

    // État d'une zone (thermostat).
    struct Zone {
        bool satelliteValid = false;      // au moins une trame thermostat reçue
        bool boilerValid = false;         // au moins une trame chaudière reçue
        // Deux températures par source :
        //  trame thermostat -> chaudière (octets 15-16 / 17-18) ;
        //  réponse chaudière -> thermostat (octets 19-20 / 21-22 pour Z1, +10 par zone).
        float ambientTemperature = NAN;        // ambiante envoyée par le thermostat
        float satelliteSetpoint = NAN;         // consigne envoyée par le thermostat
        float boilerAmbientTemperature = NAN;  // ambiante renvoyée par la chaudière
        float boilerSetpoint = NAN;            // consigne retenue par la chaudière
        uint8_t satelliteRadioMode = 0;   // octet 20 de la trame thermostat (voir doc)
        uint8_t boilerRadioMode = 0;      // mode zone vu par la chaudière
        Mode mode = Mode::Auto;           // mode demandé à la bibliothèque
        float heatSetpoint = 20.0f;       // consigne envoyée en Heat
        float frostSetpoint = 5.0f;       // consigne envoyée en Off (hors-gel)
        uint32_t txCount = 0;             // trames simulées émises avec succès
        bool lastTxConfirmed = false;     // la chaudière a répondu à la dernière émission
        // Message « CHANGER PILES » affiché par le thermostat (trame 0x43 non vide),
        // effacé par une trame 0x43 vide (piles changées). false au démarrage.
        bool batteryLow = false;
    };

    // État de la chaudière (unique).
    struct Boiler {
        bool dataValid = false;           // trame chaudière -> thermostat reçue
        bool modeValid = false;
        float outdoorTemperature = NAN;   // NAN si pas de sonde (valeur radio 129,0 °C)
        // Octet brut, champ de bits : 0x08 chauffage, 0x04 arrêt chauffage (contact sec),
        // 0x10 / 0x20 Frisquet Connect (forum, non confirmé), 9 = anomalie (firmware d'origine).
        uint8_t mode = 0;
    };

    // État d'un module hydraulique (0x25 = index 0, 0x26 = index 1).
    struct Hydraulic {
        bool setpointValid = false;       // trame chaudière -> module reçue
        bool temperatureValid = false;    // trame module -> chaudière reçue
        float waterSetpoint = NAN;        // consigne de départ d'eau demandée par la chaudière
        float waterTemperature = NAN;     // départ d'eau mesuré par le module
        uint8_t mode = 0;                 // 0 arrêt, 1 baisse, 2 stable, 3 hausse, 42 sécurité
    };

    struct Event {
        EventType type;
        const uint8_t* data;   // octets RX/TX, valides uniquement pendant le callback
        size_t length;
        int16_t status;        // code RadioLib
        float rssi;            // NAN pour un TX
        int8_t zone;           // index de zone, -1 si non concerné
    };
    using EventCallback = void (*)(const Event&, void*);
    // Appelé DEPUIS L'INTERRUPTION radio : code IRAM_ATTR très court uniquement.
    using InterruptCallback = void (*)(void*);

    explicit FrisquetVisio(SX1262& radio);
    ~FrisquetVisio();
    FrisquetVisio(const FrisquetVisio&) = delete;
    FrisquetVisio& operator=(const FrisquetVisio&) = delete;

    // Configure le SX1262 et lance l'écoute. SPI doit déjà être démarré.
    int16_t begin(const Config& config);
    // Lit la trame reçue, décode, gère les délais et émet si nécessaire.
    // Renvoie RADIOLIB_ERR_NONE, ou le code de la dernière erreur radio (CRC compris).
    int16_t loop();
    bool isReady() const;
    void onEvent(EventCallback callback, void* context = nullptr);
    // Hook facultatif appelé par l'interruption radio, par exemple pour réveiller
    // une tâche FreeRTOS. À enregistrer AVANT begin().
    bool onRadioInterrupt(InterruptCallback callback, void* context = nullptr);

    bool setMode(uint8_t zone, Mode mode);
    // Consigne de 5 à 24 °C, arrondie à 0,5 °C. mode = Heat ou Off.
    bool setTargetTemperature(uint8_t zone, Mode mode, float temperature);
    bool setTxDelay(uint8_t zone, uint16_t delayMs);
    const Zone* zone(uint8_t index) const;                  // nullptr si index invalide
    const Boiler& boiler() const;
    const Hydraulic* hydraulic(uint8_t index) const;        // nullptr si index invalide
    const Config& config() const;

private:
    // Échange en cours pour une zone : trame thermostat mémorisée en attendant
    // la réponse chaudière, puis le délai, avant d'émettre la trame simulée.
    struct Transaction {
        bool pending = false;            // une émission est à faire
        bool responseReceived = false;   // la chaudière a répondu au thermostat
        bool satelliteCrcMismatch = false;
        uint8_t frame[23] = {};          // dernière trame thermostat, modèle de l'émission
        uint32_t receivedAt = 0;
        // Anti-doublon : les répétitions (+1/+2/+3) d'un échange déjà injecté sont ignorées.
        bool injected = false;
        uint8_t lastId = 0;
        uint8_t lastBase = 0;
        uint32_t lastSentAt = 0;
    };

    static void IRAM_ATTR onRadioPacket();
    static FrisquetVisio* active_;  // instance appelée par l'interruption
    static float temperature(const uint8_t* bytes);
    bool plausible(const uint8_t* frame, size_t length) const;
    int8_t frameZone(const uint8_t* frame, size_t length) const;
    void decode(const uint8_t* frame, size_t length, uint32_t now, bool crcMismatch, float rssi);
    void notify(EventType type, const uint8_t* data, size_t length,
                int16_t status, float rssi, int8_t zone);
    int16_t startReceive();
    int16_t transmit(uint8_t zone);

    SX1262& radio_;
    Config config_;
    Zone zones_[MaxZones];
    Boiler boiler_;
    Hydraulic hydraulics_[MaxHydraulicModules];
    Transaction transactions_[MaxZones];
    EventCallback callback_ = nullptr;
    void* callbackContext_ = nullptr;
    InterruptCallback interruptCallback_ = nullptr;
    void* interruptContext_ = nullptr;
    volatile bool packetReceived_ = false;  // posé par l'interruption, lu par loop()
    bool ready_ = false;
    // Dernière émission simulée, en attente de la réponse chaudière.
    bool waitingForResponse_ = false;
    uint8_t sentZone_ = 0;
    uint8_t sentId_ = 0;
    uint8_t sentNumber_ = 0;
    uint32_t sentAt_ = 0;
    uint8_t rxBuffer_[RADIOLIB_SX126X_MAX_PACKET_LENGTH] = {};
};
