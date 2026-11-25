// SPDX-License-Identifier: MIT
#pragma once

#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"
#define MQTT_HOST "CHANGE_ME"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define NETWORK_ID_CONFIGURED false
static const uint8_t NETWORK_ID[4] = {0x00, 0x00, 0x00, 0x00};

// Nombre de thermostats (1 à 3) et de modules hydrauliques (0 à 2) déclarés dans HA.
#define ZONE_COUNT 2
#define HYDRAULIC_COUNT 1
// Délai entre la trame du thermostat et la trame simulée, par zone (0 à 1000 ms).
static const uint16_t TX_DELAY_MS[3] = {150, 150, 150};
// Trames en erreur CRC (le SX1262 fournit les octets reçus même si le CRC est faux) :
//   true  = traitées normalement (défaut de la bibliothèque, aucun problème constaté) ;
//   false = ignorées, seulement affichées dans TramType « (CRC non valide) ».
#define ACCEPT_CRC_MISMATCH true
// TramType : true = ajoute les octets de la trame en hexadécimal (debug, ex-ConfTram).
#define TRAM_BYTES false
