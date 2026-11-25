// SPDX-License-Identifier: MIT
// Compilé uniquement si ArduinoHA est disponible : la partie radio reste utilisable seule.
#if __has_include(<ArduinoHA.h>)

#include "FrisquetVisioHA.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr float MinSetpoint = 5.0f;
constexpr float MaxSetpoint = 24.0f;
constexpr float DefaultSetpoints[3] = {18.0f, 20.0f, 5.0f};  // Auto, Heat, Off
constexpr uint16_t NothingPublished = 0xFFFF;

// ArduinoHA garde les pointeurs : identifiants et noms doivent être des chaînes constantes.
const char* const ClimateIds[] = {"Zone_1", "Zone_2", "Zone_3"};
const char* const ClimateNames[] = {"Zone 1", "Zone 2", "Zone 3"};
const char* const ZoneModeIds[] = {"ModeZone1", "ModeZone2", "ModeZone3"};
const char* const ZoneModeNames[] = {"Mode zone 1", "Mode zone 2", "Mode zone 3"};
const char* const WaterSetpointIds[] = {"TspEau1", "TspEau2"};
const char* const WaterSetpointNames[] = {"Consigne eau module 1", "Consigne eau module 2"};
const char* const WaterTemperatureIds[] = {"TmesEau1", "TmesEau2"};
const char* const WaterTemperatureNames[] = {"T°eau depart module 1", "T°eau depart module 2"};
const char* const HydraulicModeIds[] = {"ModeHyd1", "ModeHyd2"};
const char* const HydraulicModeNames[] = {"Mode module hydraulique 1", "Mode module hydraulique 2"};

// Conversion des codes radio en texte (mêmes libellés que HAFrisquetVisio).
const char* boilerModeText(uint8_t mode, char* buffer) {
    switch (mode) {
    case 0: return "Heating Off";
    case 8: return "Heating On";
    case 9: return "Anomalie";
    default: std::snprintf(buffer, 24, "%u", mode); return buffer;
    }
}

const char* hydraulicModeText(uint8_t mode, char* buffer) {
    switch (mode) {
    case 0: return "Heating Off";
    case 1: return "Heat down";
    case 2: return "Flow";
    case 3: return "Heat up";
    case 9: return "Boiler off";
    case 42: return "Security";
    default: std::snprintf(buffer, 24, "%u", mode); return buffer;
    }
}

const char* zoneModeText(uint8_t mode, char* buffer) {
    switch (mode) {
    case 0x00: return "Auto Heure creuse";
    case 0x01: return "Auto Heure pleine";
    case 0x02: return "Dérogation nuit";
    case 0x03: return "Dérogation jour";
    case 0x04: return "Heure creuse";
    case 0x05: return "Heure pleine";
    case 0x10: return "Hors gel";
    case 0x14: return "Auto Hors gel";
    default: std::snprintf(buffer, 24, "Inconnu (0x%02X)", mode); return buffer;
    }
}

// Publie un capteur texte seulement si le code a changé (évite d'inonder MQTT).
void publishText(HASensor* sensor, uint16_t& last, uint8_t code,
                 const char* (*toText)(uint8_t, char*)) {
    if (last == code) return;
    char buffer[24];
    if (sensor->setValue(toText(code, buffer))) last = code;
}

HAHVAC::Mode toHomeAssistant(FrisquetVisio::Mode mode) {
    if (mode == FrisquetVisio::Mode::Heat) return HAHVAC::HeatMode;
    if (mode == FrisquetVisio::Mode::Off) return HAHVAC::OffMode;
    return HAHVAC::AutoMode;
}

HASensorNumber* temperatureSensor(const char* id, const char* name, const char* icon) {
    HASensorNumber* sensor = new HASensorNumber(id, HASensorNumber::PrecisionP1);
    sensor->setName(name);
    sensor->setIcon(icon);
    sensor->setUnitOfMeasurement("°C");
    return sensor;
}

HASensor* textSensor(const char* id, const char* name) {
    HASensor* sensor = new HASensor(id);
    sensor->setName(name);
    sensor->setIcon("mdi:state-machine");
    return sensor;
}

// Libellé TramType d'une trame reçue, mêmes codes que le firmware d'origine :
// 8/9/10 thermostat zone 1/2/3, 81 réponse chaudière, 251/253/255/257 module hydraulique.
// Adresses : octet 0 = destinataire, octet 1 = émetteur (0x80 chaudière, 0x25/0x26 modules).
int frameLabel(const uint8_t* f, uint8_t n, char* out, size_t size) {
    const bool fromBoiler = n > 1 && f[1] == 0x80;
    const bool toBoiler = n > 1 && f[0] == 0x80;
    const auto module = [](uint8_t address) { return address == 0x25 || address == 0x26; };
    if (n == 23 && toBoiler && f[1] >= 0x08 && f[1] <= 0x0A) {
        return std::snprintf(out, size, "%u: Zone %u", f[1], f[1] - 0x07);
    }
    if (n == 49 && fromBoiler && f[0] >= 0x08 && f[0] <= 0x0A) {
        return std::snprintf(out, size, "81: Réponse Chaudière Reçue Zone %u", f[0] - 0x07);
    }
    if (n == 19 && fromBoiler && module(f[0])) {
        return std::snprintf(out, size, "251: Consigne Module hydrau %u", f[0] - 0x24);
    }
    if (n == 9 && toBoiler && module(f[1])) {
        return std::snprintf(out, size, "253: Réponse Module hydrau %u", f[1] - 0x24);
    }
    if (n == 17 && toBoiler && module(f[1])) {
        return std::snprintf(out, size, "255: Envoie T°ext Chaudière");
    }
    if (n == 15 && fromBoiler && module(f[0])) {
        return std::snprintf(out, size, "257: Réponse Chaudière date");
    }
    return std::snprintf(out, size, "Tram non valide");
}
}

FrisquetVisioHA* FrisquetVisioHA::instance_ = nullptr;

FrisquetVisioHA::FrisquetVisioHA(FrisquetVisio& visio) : visio_(visio) {
    for (uint8_t i = 0; i < FrisquetVisio::MaxZones; ++i) {
        lastZoneMode_[i] = NothingPublished;
        for (uint8_t m = 0; m < 3; ++m) setpoints_[i][m] = DefaultSetpoints[m];
    }
    for (uint8_t i = 0; i < FrisquetVisio::MaxHydraulicModules; ++i) {
        lastHydraulicMode_[i] = NothingPublished;
    }
}

bool FrisquetVisioHA::begin(uint8_t zoneCount, uint8_t hydraulicModuleCount,
                            SemaphoreHandle_t visioMutex, bool tramBytes) {
    if (instance_ != nullptr || zoneCount == 0 || zoneCount > FrisquetVisio::MaxZones
        || hydraulicModuleCount > FrisquetVisio::MaxHydraulicModules) {
        return false;
    }
    // ponytail: 8 événements en attente max ; au-delà les plus récents sont perdus
    // pour TramType seulement (la radio n'est pas affectée).
    radioRecords_ = xQueueCreate(8, sizeof(RadioRecord));
    if (radioRecords_ == nullptr) return false;
    instance_ = this;
    mutex_ = visioMutex;
    tramBytes_ = tramBytes;
    zoneCount_ = zoneCount;
    hydraulicCount_ = hydraulicModuleCount;

    // Consignes Heat/Off sauvegardées ; valeur hors plage -> valeur par défaut.
    Preferences preferences;
    if (preferences.begin("frisquet", true)) {
        if (preferences.getBytesLength("modesp") == sizeof(setpoints_)) {
            preferences.getBytes("modesp", setpoints_, sizeof(setpoints_));
        }
        preferences.end();
    }
    for (uint8_t i = 0; i < FrisquetVisio::MaxZones; ++i) {
        for (uint8_t m = 0; m < 3; ++m) {
            const float value = setpoints_[i][m];
            if (!std::isfinite(value) || value < MinSetpoint || value > MaxSetpoint) {
                setpoints_[i][m] = DefaultSetpoints[m];
            }
        }
    }

    // Les entités sont créées ici (et jamais détruites) car ArduinoHA exige que
    // HAMqtt existe déjà au moment de leur construction.
    for (uint8_t i = 0; i < zoneCount_; ++i) {
        HAHVAC* climate = new HAHVAC(ClimateIds[i],
                                     HAHVAC::TargetTemperatureFeature | HAHVAC::ModesFeature);
        climate->setName(ClimateNames[i]);
        // Bornes du curseur HA reprises du firmware d'origine (16-24 °C). La validation
        // interne reste 5-24 °C : consigne Off 5 °C restaurée depuis NVS ou MQTT.
        climate->setMinTemp(16);
        climate->setMaxTemp(MaxSetpoint);
        climate->setTempStep(0.5f);        // pas réellement transmis par la radio
        climate->setModes(HAHVAC::OffMode | HAHVAC::HeatMode | HAHVAC::AutoMode);
        climate->setRetain(true);          // le mode choisi est restauré après redémarrage
        climate->onModeCommand(onModeCommand);
        climate->onTargetTemperatureCommand(onTargetTemperatureCommand);
        climates_[i] = climate;
        zoneModes_[i] = textSensor(ZoneModeIds[i], ZoneModeNames[i]);
    }
    outdoor_ = temperatureSensor("Text", "T°Air exterieur", "mdi:home-thermometer-outline");
    boilerMode_ = textSensor("ModeChaudiere", "Mode Chaudiere");
    tramType_ = textSensor("TramType", "visio_Tram reçu");
    for (uint8_t i = 0; i < hydraulicCount_; ++i) {
        waterSetpoints_[i] = temperatureSensor(WaterSetpointIds[i], WaterSetpointNames[i],
                                               "mdi:home-thermometer");
        waterTemperatures_[i] = temperatureSensor(WaterTemperatureIds[i], WaterTemperatureNames[i],
                                                  "mdi:home-thermometer");
        hydraulicModes_[i] = textSensor(HydraulicModeIds[i], HydraulicModeNames[i]);
    }
    return true;
}

void FrisquetVisioHA::loop() {
    syncWithRadio();
    publish();
    publishTramType();
}

void FrisquetVisioHA::onRadioEvent(const FrisquetVisio::Event& event, void* self) {
    FrisquetVisioHA* const ha = static_cast<FrisquetVisioHA*>(self);
    if (ha == nullptr || ha->radioRecords_ == nullptr) return;
    // Confirmation : l'événement Receive qui suit affiche déjà « 81: Réponse Chaudière ».
    if (event.type == FrisquetVisio::EventType::TxConfirmed) return;
    RadioRecord record;
    record.at = millis();
    record.status = event.status;
    record.type = event.type;
    record.zone = event.zone;
    record.rssi = event.rssi;
    record.length = static_cast<uint8_t>(event.length < sizeof(record.data) ? event.length
                                                                             : sizeof(record.data));
    if (event.data != nullptr) std::memcpy(record.data, event.data, record.length);
    xQueueSend(ha->radioRecords_, &record, 0);  // jamais bloquant pour la tâche radio
}

// Publie chaque événement radio en attente dans TramType (vidé même hors connexion).
void FrisquetVisioHA::publishTramType() {
    HAMqtt* const mqtt = HAMqtt::instance();
    const bool connected = mqtt != nullptr && mqtt->isConnected();
    RadioRecord r;
    while (xQueueReceive(radioRecords_, &r, 0) == pdTRUE) {
        if (!connected) continue;
        char text[256];
        int n = 0;
        if (r.type == FrisquetVisio::EventType::Receive) {
            if (r.status == RADIOLIB_ERR_NONE || r.status == RADIOLIB_ERR_CRC_MISMATCH) {
                n = frameLabel(r.data, r.length, text, sizeof(text));
                // CRC faux : trame quand même affichée, pour juger la qualité radio.
                if (r.status == RADIOLIB_ERR_CRC_MISMATCH) {
                    n += std::snprintf(text + n, sizeof(text) - n, " (CRC non valide)");
                }
            } else {
                n = std::snprintf(text, sizeof(text), "No tram received (%d)", r.status);
            }
            n += std::snprintf(text + n, sizeof(text) - n, " - Time: %lus RSSI %.0f",
                               static_cast<unsigned long>(r.at / 1000), r.rssi);
        } else if (r.type == FrisquetVisio::EventType::Transmit) {
            // Octets 17-18 de la trame émise = consigne en dixièmes de degré.
            const float target = ((r.data[17] << 8) | r.data[18]) / 10.0f;
            n = r.status == RADIOLIB_ERR_NONE
                ? std::snprintf(text, sizeof(text), "Envoie consigne Zone %d : %.1f°C TrNum %02X",
                                r.zone + 1, target, r.data[3])
                : std::snprintf(text, sizeof(text), "Erreur Envoie consigne Zone %d (%d)",
                                r.zone + 1, r.status);
        } else if (r.type == FrisquetVisio::EventType::ResponseTimeout) {
            n = std::snprintf(text, sizeof(text), "Zone %d : pas de réponse chaudière à l'envoi",
                              r.zone + 1);
        } else {
            n = std::snprintf(text, sizeof(text), "Zone %d : pas de réponse chaudière au thermostat",
                              r.zone + 1);
        }
        // Option debug : octets en hexadécimal (49 x 3 caractères max, tient dans 256).
        for (uint8_t i = 0; tramBytes_ && i < r.length && n < static_cast<int>(sizeof(text)) - 4; ++i) {
            n += std::snprintf(text + n, sizeof(text) - n, i == 0 ? " : %02X" : " %02X", r.data[i]);
        }
        tramType_->setValue(text);
    }
}

int8_t FrisquetVisioHA::zoneIndex(const HAHVAC* sender) const {
    for (uint8_t i = 0; i < zoneCount_; ++i) {
        if (climates_[i] == sender) return static_cast<int8_t>(i);
    }
    return -1;
}

// Commande de mode depuis HA (appelée pendant mqtt.loop()).
void FrisquetVisioHA::onModeCommand(HAHVAC::Mode mode, HAHVAC* sender) {
    FrisquetVisioHA* const self = instance_;
    const int8_t i = self != nullptr ? self->zoneIndex(sender) : -1;
    if (i < 0) return;
    Request& request = self->requests_[i];
    if (mode == HAHVAC::AutoMode) request.mode = FrisquetVisio::Mode::Auto;
    else if (mode == HAHVAC::HeatMode) request.mode = FrisquetVisio::Mode::Heat;
    else if (mode == HAHVAC::OffMode) request.mode = FrisquetVisio::Mode::Off;
    else {
        sender->setMode(toHomeAssistant(request.mode), true);  // mode non géré : on rétablit
        return;
    }
    request.dirty = true;
    sender->setMode(mode, true);
}

// Commande de consigne depuis HA (appelée pendant mqtt.loop()).
void FrisquetVisioHA::onTargetTemperatureCommand(HANumeric temperature, HAHVAC* sender) {
    FrisquetVisioHA* const self = instance_;
    const int8_t i = self != nullptr ? self->zoneIndex(sender) : -1;
    if (i < 0) return;
    Request& request = self->requests_[i];

    // En Auto la consigne appartient au thermostat physique : on réaffiche la sienne.
    if (request.mode == FrisquetVisio::Mode::Auto) {
        const FrisquetVisio::Zone& zone = self->zones_[i];
        if (self->haveSnapshot_ && zone.satelliteValid) {
            sender->setTargetTemperature(zone.satelliteSetpoint, true);
        }
        return;
    }

    const uint8_t slot = request.mode == FrisquetVisio::Mode::Heat ? 1 : 2;
    const float value = temperature.toFloat();
    if (!std::isfinite(value) || value < MinSetpoint || value > MaxSetpoint) {
        sender->setTargetTemperature(self->setpoints_[i][slot], true);  // refusée
        return;
    }
    const float rounded = std::round(value * 2.0f) / 2.0f;
    if (rounded != self->setpoints_[i][slot]) {
        self->setpoints_[i][slot] = rounded;
        self->saveSetpoints();
    }
    request.dirty = true;
    sender->setTargetTemperature(rounded, true);
}

// Seule fonction qui touche à la radio : courte, sous mutex, sans appel MQTT.
void FrisquetVisioHA::syncWithRadio() {
    // Mutex occupé (émission en cours...) : on réessaie au prochain loop().
    if (mutex_ != nullptr && xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (visio_.isReady()) {
        for (uint8_t i = 0; i < zoneCount_; ++i) {
            Request& request = requests_[i];
            if (request.dirty) {
                visio_.setTargetTemperature(i, FrisquetVisio::Mode::Heat, setpoints_[i][1]);
                visio_.setTargetTemperature(i, FrisquetVisio::Mode::Off, setpoints_[i][2]);
                visio_.setMode(i, request.mode);  // refusé si Network ID FF FF FF FF
                request.dirty = false;
            }
            const FrisquetVisio::Zone* zone = visio_.zone(i);
            if (zone != nullptr) zones_[i] = *zone;
            request.mode = zones_[i].mode;  // l'état réel de la radio fait foi
        }
        boiler_ = visio_.boiler();
        for (uint8_t i = 0; i < hydraulicCount_; ++i) hydraulics_[i] = *visio_.hydraulic(i);
        haveSnapshot_ = true;
    }
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
}

// Publie vers HA. Les setters ArduinoHA ne publient que si la valeur change.
void FrisquetVisioHA::publish() {
    HAMqtt* const mqtt = HAMqtt::instance();
    const bool connected = mqtt != nullptr && mqtt->isConnected();
    if (connected && !wasConnected_) {
        // Reconnexion : ArduinoHA republie les nombres, pas les capteurs texte.
        for (uint16_t& last : lastZoneMode_) last = NothingPublished;
        for (uint16_t& last : lastHydraulicMode_) last = NothingPublished;
        lastBoilerMode_ = NothingPublished;
    }
    wasConnected_ = connected;
    if (!connected || !haveSnapshot_) return;

    for (uint8_t i = 0; i < zoneCount_; ++i) {
        if (requests_[i].dirty) continue;  // commande pas encore appliquée à la radio
        const FrisquetVisio::Zone& zone = zones_[i];
        HAHVAC* const climate = climates_[i];
        climate->setMode(toHomeAssistant(zone.mode));
        // Comme le firmware d'origine : température courante = ambiante renvoyée par la
        // chaudière (réponse 49 octets), disponible pour toutes les zones à chaque réponse.
        if (zone.boilerValid) climate->setCurrentTemperature(zone.boilerAmbientTemperature);
        // Consigne affichée : celle imposée (Heat/Off) ou celle du thermostat (Auto).
        const float target = zone.mode == FrisquetVisio::Mode::Heat ? zone.heatSetpoint
            : zone.mode == FrisquetVisio::Mode::Off ? zone.frostSetpoint
            : zone.satelliteValid ? zone.satelliteSetpoint : NAN;
        if (std::isfinite(target)) climate->setTargetTemperature(target);
        if (zone.satelliteValid) {
            publishText(zoneModes_[i], lastZoneMode_[i], zone.satelliteRadioMode, zoneModeText);
        }
    }

    if (boiler_.dataValid) outdoor_->setValue(boiler_.outdoorTemperature);
    if (boiler_.modeValid) publishText(boilerMode_, lastBoilerMode_, boiler_.mode, boilerModeText);

    for (uint8_t i = 0; i < hydraulicCount_; ++i) {
        const FrisquetVisio::Hydraulic& module = hydraulics_[i];
        if (module.setpointValid) {
            waterSetpoints_[i]->setValue(module.waterSetpoint);
            publishText(hydraulicModes_[i], lastHydraulicMode_[i], module.mode, hydraulicModeText);
        }
        if (module.temperatureValid) waterTemperatures_[i]->setValue(module.waterTemperature);
    }
}

void FrisquetVisioHA::saveSetpoints() {
    Preferences preferences;
    if (!preferences.begin("frisquet", false)) return;
    preferences.putBytes("modesp", setpoints_, sizeof(setpoints_));
    preferences.end();
}

#endif
