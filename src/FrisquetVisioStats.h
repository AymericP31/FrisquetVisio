// SPDX-License-Identifier: MIT
//
// FrisquetVisioStats : compteurs radio et textes de diagnostic (Telnet, OLED), facultatif.
//
// Utilisation : appeler record() depuis le callback onEvent() de FrisquetVisio, donc dans
// la tâche qui appelle visio.loop(). L'objet est un simple bloc de données : une autre
// tâche peut le copier sous le même mutex que visio, puis formater la copie hors mutex.
#pragma once

#include <Arduino.h>
#include "FrisquetVisio.h"

class FrisquetVisioStats {
public:
    struct Rx {
        uint32_t count;      // trames reçues sans erreur
        uint32_t errors;     // trames reçues avec erreur (CRC compris)
        int16_t lastStatus;  // code RadioLib
        float lastRssi;
        uint32_t lastAt;     // millis(), 0 = jamais
    };
    // Émissions simulées d'une zone (Heltec -> chaudière).
    struct Tx {
        uint32_t count;           // émissions réussies
        uint32_t errors;          // émissions en erreur
        uint32_t confirmed;       // réponses chaudière à la trame simulée
        uint32_t timeouts;        // pas de réponse chaudière en 2 s
        uint32_t expired;         // thermostat reçu, pas de réponse chaudière en 10 s : pas de TX
        int16_t lastStatus;
        uint32_t lastAt;
        uint32_t lastDelayMs;     // trame thermostat -> émission
        bool lastConfirmed;
        float lastConfirmRssi;
        uint32_t lastConfirmDelayMs;  // émission -> réponse chaudière
        uint32_t lastConfirmAt;
    };

    Rx satellite[FrisquetVisio::MaxZones] = {};  // vrais thermostats
    Rx boiler = {};                              // chaudière (vers thermostats et modules)
    Rx hydraulic = {};                           // modules hydrauliques
    Rx other = {};                               // trames non attribuées
    Tx tx[FrisquetVisio::MaxZones] = {};
    uint32_t crcErrors = 0;                      // RADIOLIB_ERR_CRC_MISMATCH, toutes trames

    void record(const FrisquetVisio::Event& event, uint32_t now);
    void reset() { *this = FrisquetVisioStats(); }
    // Âge de la dernière trame chaudière ; depuis le démarrage si aucune trame reçue.
    uint32_t boilerRxAgeMs(uint32_t now) const { return now - boiler.lastAt; }
    // Une ligne de trace pour un événement (octets en hexadécimal). Renvoie la longueur écrite.
    size_t formatTrace(const FrisquetVisio::Event& event, uint32_t now, char* out, size_t size) const;
    // Rapport Telnet. device : all, zone1..zone3 (z1..z3), chaudiere, hydrau, heltec, init, help.
    String report(const String& device, const FrisquetVisio::Config& config, bool radioOk,
                  uint32_t now) const;
    // Six lignes courtes pour un écran 128x64.
    String oled(bool radioOk, float rxBandwidthKHz, uint32_t now) const;
};
