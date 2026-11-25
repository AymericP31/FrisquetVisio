// SPDX-License-Identifier: MIT
#include "FrisquetVisio.h"

#include <cstring>

namespace {
// Une trame thermostat attend la réponse chaudière au plus 10 s, sinon elle est abandonnée.
constexpr uint32_t TransactionTimeoutMs = 10000;
// Fenêtre pendant laquelle une répétition du thermostat n'est pas réinjectée.
constexpr uint32_t DuplicateWindowMs = 60000;
// Après une émission simulée, la réponse chaudière est attendue au plus 2 s.
constexpr uint32_t SimulatedResponseTimeoutMs = 2000;
constexpr float MinSetpoint = 5.0f;
constexpr float MaxSetpoint = 24.0f;
constexpr uint8_t BoilerAddress = 0x80;
constexpr uint8_t FirstSatelliteAddress = 0x08;
constexpr uint8_t FirstHydraulicAddress = 0x25;
// T° extérieure 129,0 °C (0x050A) : la chaudière n'a pas de sonde extérieure.
constexpr uint16_t NoOutdoorSensor = 0x050A;

// Le Network ID FF FF FF FF sert uniquement à écouter l'association : émission interdite.
bool isBroadcast(const uint8_t* id) {
    return id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF && id[3] == 0xFF;
}

// Index de module hydraulique (0 ou 1) pour une adresse radio, -1 sinon.
int8_t hydraulicIndex(uint8_t address) {
    const uint8_t index = static_cast<uint8_t>(address - FirstHydraulicAddress);
    return index < FrisquetVisio::MaxHydraulicModules ? static_cast<int8_t>(index) : -1;
}
}

FrisquetVisio* FrisquetVisio::active_ = nullptr;

FrisquetVisio::FrisquetVisio(SX1262& radio) : radio_(radio) {}

FrisquetVisio::~FrisquetVisio() {
    if (active_ == this) {
        radio_.clearPacketReceivedAction();
        active_ = nullptr;
    }
}

int16_t FrisquetVisio::begin(const Config& config) {
    // Une seule instance peut posséder l'interruption radio.
    if (active_ != nullptr && active_ != this) return RadioInUse;
    if (ready_) return RadioInUse;
    if (config.zoneCount == 0 || config.zoneCount > MaxZones
        || !std::isfinite(config.frequencyMHz) || !std::isfinite(config.rxBandwidthKHz)) {
        return InvalidConfig;
    }
    const bool broadcast = isBroadcast(config.networkId);
    for (uint8_t i = 0; i < MaxZones; ++i) {
        if (config.txDelayMs[i] > MaxTxDelayMs) return InvalidConfig;
        if (broadcast && zones_[i].mode != Mode::Auto) return InvalidConfig;
    }

    config_ = config;
    waitingForResponse_ = false;
    packetReceived_ = false;
    for (uint8_t i = 0; i < MaxZones; ++i) transactions_[i] = Transaction{};

    // Paramètres radio Visio : FSK 25 kbit/s, déviation 50 kHz, préambule court,
    // Network ID utilisé comme mot de synchronisation.
    int16_t status = radio_.beginFSK(config_.frequencyMHz, 25.0f, 50.0f,
                                    config_.rxBandwidthKHz, config_.outputPowerDbm, 4);
    if (status != RADIOLIB_ERR_NONE) return status;
    status = radio_.setPreambleLength(4);
    if (status != RADIOLIB_ERR_NONE) return status;
    status = radio_.setSyncWord(config_.networkId, sizeof(config_.networkId));
    if (status != RADIOLIB_ERR_NONE) return status;
    status = radio_.setRxBoostedGainMode(config_.boostedGain, true);
    if (status != RADIOLIB_ERR_NONE) return status;

    active_ = this;
    status = startReceive();
    if (status != RADIOLIB_ERR_NONE) {
        radio_.clearPacketReceivedAction();
        active_ = nullptr;
    }
    return status;
}

bool FrisquetVisio::isReady() const { return ready_; }

void FrisquetVisio::onEvent(EventCallback callback, void* context) {
    callback_ = callback;
    callbackContext_ = context;
}

bool FrisquetVisio::onRadioInterrupt(InterruptCallback callback, void* context) {
    // Refusé une fois la radio active : l'interruption pourrait lire un hook à moitié écrit.
    if (active_ == this) return false;
    interruptCallback_ = callback;
    interruptContext_ = context;
    return true;
}

bool FrisquetVisio::setMode(uint8_t index, Mode mode) {
    if (index >= config_.zoneCount) return false;
    if (mode != Mode::Auto && mode != Mode::Heat && mode != Mode::Off) return false;
    if (isBroadcast(config_.networkId) && mode != Mode::Auto) return false;
    zones_[index].mode = mode;
    // Retour en Auto : on abandonne une éventuelle émission en attente.
    if (mode == Mode::Auto) transactions_[index].pending = false;
    return true;
}

bool FrisquetVisio::setTargetTemperature(uint8_t index, Mode mode, float value) {
    if (index >= config_.zoneCount || (mode != Mode::Heat && mode != Mode::Off)
        || !std::isfinite(value) || value < MinSetpoint || value > MaxSetpoint) return false;
    // Le protocole transporte des dixièmes de degré ; le thermostat utilise des pas de 0,5 °C.
    const float rounded = std::round(value * 2.0f) / 2.0f;
    if (mode == Mode::Heat) zones_[index].heatSetpoint = rounded;
    else zones_[index].frostSetpoint = rounded;
    return true;
}

bool FrisquetVisio::setTxDelay(uint8_t index, uint16_t delayMs) {
    if (index >= config_.zoneCount || delayMs > MaxTxDelayMs) return false;
    config_.txDelayMs[index] = delayMs;
    return true;
}

const FrisquetVisio::Zone* FrisquetVisio::zone(uint8_t index) const {
    return index < config_.zoneCount ? &zones_[index] : nullptr;
}

const FrisquetVisio::Boiler& FrisquetVisio::boiler() const { return boiler_; }

const FrisquetVisio::Hydraulic* FrisquetVisio::hydraulic(uint8_t index) const {
    return index < MaxHydraulicModules ? &hydraulics_[index] : nullptr;
}

const FrisquetVisio::Config& FrisquetVisio::config() const { return config_; }

// Interruption DIO1 du SX1262 : on ne fait que lever un drapeau (et le hook éventuel).
// Lecture SPI, décodage et émission sont faits plus tard par loop(), hors interruption.
void IRAM_ATTR FrisquetVisio::onRadioPacket() {
    FrisquetVisio* const instance = active_;
    if (instance != nullptr) {
        instance->packetReceived_ = true;
        if (instance->interruptCallback_ != nullptr) {
            instance->interruptCallback_(instance->interruptContext_);
        }
    }
}

// Température codée sur 2 octets, big-endian signé, en dixièmes de degré.
float FrisquetVisio::temperature(const uint8_t* bytes) {
    const uint16_t value = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    return static_cast<int16_t>(value) / 10.0f;
}

// Trame CRC invalide : octets exploités dans une plage cohérente ?
bool FrisquetVisio::plausible(const uint8_t* frame, size_t length) const {
    auto in = [frame](size_t i, float lo, float hi) {
        const float t = temperature(frame + i);
        return t >= lo && t <= hi;
    };
    switch (length) {
    case 23:  // Thermostat : ambiance, consigne, mode
        return in(15, 0, 40) && in(17, 5, 30)
            && (frame[20] <= 0x05 || frame[20] == 0x10 || frame[20] == 0x14);
    case 49:  // Chaudière : T° ext (saturée à 35 °C ou 129 °C = sans sonde), mode, zones gérées
        for (size_t z = 0; z < config_.zoneCount; ++z) {
            if (!in(19 + z * 10, 0, 40) || !in(21 + z * 10, 5, 30)) return false;
        }
        return (((frame[7] << 8) | frame[8]) == NoOutdoorSensor || in(7, -20, 35))
            && frame[17] < 0x40;
    case 19:  // Chaudière -> module : T° eau consigne, mode (9 = chaudière arrêtée)
        return in(15, 0, 70) && (frame[18] <= 3 || frame[18] == 9 || frame[18] == 42);
    case 9:   // Module -> chaudière : T° eau départ
        return in(7, 0, 70);
    case 15:  // Chaudière -> module : mode chaudière (champ de bits)
        return frame[13] < 0x40;
    default:  // Aucun octet exploité
        return true;
    }
}

// Zone concernée par une trame thermostat (23 o) ou par la réponse chaudière (49 o), sinon -1.
int8_t FrisquetVisio::frameZone(const uint8_t* frame, size_t length) const {
    if (length < 6) return -1;
    uint8_t address = 0;
    if (length == 23 && frame[0] == BoilerAddress && frame[4] == 0x01) address = frame[1];
    else if (length == 49 && frame[1] == BoilerAddress && frame[4] == 0x81) address = frame[0];
    if (address < FirstSatelliteAddress || address >= FirstSatelliteAddress + config_.zoneCount) {
        return -1;
    }
    return static_cast<int8_t>(address - FirstSatelliteAddress);
}

void FrisquetVisio::decode(const uint8_t* frame, size_t length, uint32_t now, bool crcMismatch,
                           float rssi) {
    // Thermostat -> chaudière, octet 5 = 0x43 : message affiché, octets 6-7 = longueur du
    // texte qui suit. Seul texte observé : « SATELLITE Zn » « CHANGER PILES ». Longueur 0 =
    // message effacé (piles changées, redémarrage du thermostat).
    if (length >= 8 && frame[0] == BoilerAddress && frame[4] == 0x01 && frame[5] == 0x43
        && static_cast<size_t>((frame[6] << 8) | frame[7]) == length - 8) {
        const uint8_t zone = static_cast<uint8_t>(frame[1] - FirstSatelliteAddress);
        if (zone < config_.zoneCount) zones_[zone].batteryLow = length > 8;
        return;
    }
    // Octet 5 = 0x17 pour toutes les autres trames Visio exploitées.
    if (length < 6 || frame[5] != 0x17) return;
    const int8_t index = frameZone(frame, length);

    // 1) Thermostat -> chaudière (23 octets).
    if (index >= 0 && length == 23) {
        Zone& zone = zones_[index];
        Transaction& transaction = transactions_[index];
        zone.satelliteValid = true;
        zone.ambientTemperature = temperature(frame + 15);
        zone.satelliteSetpoint = temperature(frame + 17);
        zone.satelliteRadioMode = frame[20];

        // Octet 3 = TrNum : bits 7..2 = numéro d'échange, bits 1..0 = n° de répétition.
        // Une répétition d'un échange déjà injecté ne doit pas être réinjectée.
        const uint8_t base = frame[3] & 0xFC;
        const bool duplicate = transaction.injected && transaction.lastId == frame[2]
            && transaction.lastBase == base && now - transaction.lastSentAt < DuplicateWindowMs;
        // En Auto rien n'est émis ; sinon on mémorise la trame comme modèle d'émission.
        transaction.pending = zone.mode != Mode::Auto && !duplicate;
        transaction.satelliteCrcMismatch = crcMismatch;
        if (transaction.pending) {
            std::memcpy(transaction.frame, frame, sizeof(transaction.frame));
            transaction.receivedAt = now;
            transaction.responseReceived = false;
        }
        return;
    }

    // 2) Chaudière -> thermostat (49 octets) : extérieur, mode, et consignes de toutes les zones.
    if (index >= 0 && length == 49) {
        boiler_.dataValid = true;
        boiler_.modeValid = true;
        boiler_.outdoorTemperature = ((frame[7] << 8) | frame[8]) == NoOutdoorSensor
            ? NAN : temperature(frame + 7);
        boiler_.mode = frame[17];
        for (uint8_t i = 0; i < config_.zoneCount; ++i) {
            zones_[i].boilerValid = true;
            zones_[i].boilerAmbientTemperature = temperature(frame + 19 + i * 10);
            zones_[i].boilerSetpoint = temperature(frame + 21 + i * 10);
            zones_[i].boilerRadioMode = frame[24 + i * 10];
        }

        // Réponse au vrai thermostat : l'émission simulée peut partir après le délai.
        Transaction& transaction = transactions_[index];
        if (transaction.pending && frame[0] == transaction.frame[1]
            && frame[1] == transaction.frame[0] && frame[2] == transaction.frame[2]
            && frame[3] == transaction.frame[3]) {
            if (transaction.satelliteCrcMismatch) {
                // Bloc de la zone dans la reponse chaudiere : 19-26 (Z1), 29-36 (Z2), 39-46 (Z3).
                const uint8_t* TramBoilerZone = frame + 19 + index * 10;
                // Comparer aux octets 15-22 du thermostat ; toute difference annule le TX.
                if (std::memcmp(transaction.frame + 15, TramBoilerZone, 8) != 0) {
                    transaction.pending = false;
                    return;
                }
                std::memcpy(transaction.frame + 15, TramBoilerZone, 8);
            }
            transaction.responseReceived = true;
        }
        // Réponse à notre trame simulée : confirmation.
        if (waitingForResponse_ && index == sentZone_
            && frame[2] == sentId_ && frame[3] == sentNumber_
            && now - sentAt_ < SimulatedResponseTimeoutMs) {
            zones_[index].lastTxConfirmed = true;
            waitingForResponse_ = false;
            notify(EventType::TxConfirmed, frame, length,
                   crcMismatch ? RADIOLIB_ERR_CRC_MISMATCH : RADIOLIB_ERR_NONE, rssi, index);
        }
        return;
    }

    // 3) Modules hydrauliques (adresse 0x25 ou 0x26).
    if (length == 19 && frame[1] == BoilerAddress && frame[4] == 0x03) {
        // Chaudière -> module : consigne d'eau et mode de régulation du module.
        const int8_t module = hydraulicIndex(frame[0]);
        if (module < 0) return;
        hydraulics_[module].setpointValid = true;
        hydraulics_[module].waterSetpoint = temperature(frame + 15);
        hydraulics_[module].mode = frame[18];
    } else if (length == 9 && frame[0] == BoilerAddress && frame[4] == 0x83) {
        // Module -> chaudière : température de départ d'eau mesurée.
        const int8_t module = hydraulicIndex(frame[1]);
        if (module < 0) return;
        hydraulics_[module].temperatureValid = true;
        hydraulics_[module].waterTemperature = temperature(frame + 7);
    } else if (length == 15 && frame[1] == BoilerAddress && frame[4] == 0x81
               && hydraulicIndex(frame[0]) >= 0) {
        // Chaudière -> module : date et mode chaudière.
        boiler_.modeValid = true;
        boiler_.mode = frame[13];
    }
}

void FrisquetVisio::notify(EventType type, const uint8_t* data, size_t length,
                          int16_t status, float rssi, int8_t zoneIndex) {
    if (callback_ != nullptr) {
        const Event event{type, data, length, status, rssi, zoneIndex};
        callback_(event, callbackContext_);
    }
}

// (Ré)arme la réception et l'interruption « paquet reçu ».
int16_t FrisquetVisio::startReceive() {
    radio_.setPacketReceivedAction(onRadioPacket);
    const int16_t status = radio_.startReceive();
    ready_ = status == RADIOLIB_ERR_NONE;
    return status;
}

// Émet la trame simulée pour une zone : copie de la trame thermostat,
// TrNum = échange suivant (+4), consigne et mode remplacés.
int16_t FrisquetVisio::transmit(uint8_t index) {
    Transaction& transaction = transactions_[index];
    const Zone& zone = zones_[index];
    uint8_t frame[sizeof(transaction.frame)];
    std::memcpy(frame, transaction.frame, sizeof(frame));

    // +4 = nouvel échange pour la chaudière. Un +1 serait vu comme une répétition :
    // la chaudière répond mais n'applique pas la consigne (constaté sur la zone 1).
    const uint8_t base = frame[3] & 0xFC;
    frame[3] = static_cast<uint8_t>(base + 4);

    // Octets 17-18 : consigne en dixièmes de degré.
    const float target = zone.mode == Mode::Heat ? zone.heatSetpoint : zone.frostSetpoint;
    const uint16_t encoded = static_cast<uint16_t>(std::round(target * 10.0f));
    frame[17] = static_cast<uint8_t>(encoded >> 8);
    frame[18] = static_cast<uint8_t>(encoded);

    // Octet 20 : mode. On garde la famille du thermostat (permanent 0x00/0x01 ou auto
    // 0x04/0x05, dérogation 0x02/0x03 -> auto) et on force Jour (0x01/0x05) ou Hors gel (0x10/0x14).
    const bool fixedMode = frame[20] == 0x00 || frame[20] == 0x01;
    frame[20] = zone.mode == Mode::Heat ? (fixedMode ? 0x01 : 0x05)
                                        : (fixedMode ? 0x10 : 0x14);

    // L'interruption est détachée pendant l'émission : sinon la fin d'émission (TX_DONE,
    // même broche DIO1) serait prise pour une réception.
    radio_.clearPacketReceivedAction();
    const int16_t status = radio_.transmit(frame, sizeof(frame));  // bloquant (~10 ms)
    const uint32_t now = millis();
    transaction.pending = false;
    zones_[index].lastTxConfirmed = false;
    if (status == RADIOLIB_ERR_NONE) {
        transaction.injected = true;
        transaction.lastId = frame[2];
        transaction.lastBase = base;
        transaction.lastSentAt = now;
        zones_[index].txCount++;
        waitingForResponse_ = true;
        sentZone_ = index;
        sentId_ = frame[2];
        sentNumber_ = frame[3];
        sentAt_ = now;
    }
    // Réarme la réception même si l'émission a échoué (le SX1262 reste en veille sinon).
    const int16_t receiveStatus = startReceive();
    notify(EventType::Transmit, frame, sizeof(frame), status, NAN, index);
    return receiveStatus != RADIOLIB_ERR_NONE ? receiveStatus : status;
}

int16_t FrisquetVisio::loop() {
    if (!ready_) return NotReady;

    // Étape 1 : une trame a été signalée par l'interruption -> lecture SPI.
    if (packetReceived_) {
        packetReceived_ = false;
        size_t length = radio_.getPacketLength();
        int16_t status = RADIOLIB_ERR_NONE;
        if (length == 0 || length > sizeof(rxBuffer_)) {
            status = length == 0 ? RADIOLIB_ERR_UNKNOWN : RADIOLIB_ERR_PACKET_TOO_LONG;
            length = 0;
        } else {
            std::memset(rxBuffer_, 0, length);
            // En cas d'erreur CRC, RadioLib copie quand même les octets puis renvoie l'erreur.
            status = radio_.readData(rxBuffer_, length);
        }
        const float rssi = radio_.getRSSI();
        radio_.clearPacketReceivedAction();
        const int16_t receiveStatus = startReceive();
        const bool usable = status == RADIOLIB_ERR_NONE
            || (status == RADIOLIB_ERR_CRC_MISMATCH && config_.acceptCrcMismatch
                && length > 0 && plausible(rxBuffer_, length));
        if (usable) decode(rxBuffer_, length, millis(), status == RADIOLIB_ERR_CRC_MISMATCH, rssi);
        notify(EventType::Receive, rxBuffer_, length, status, rssi, frameZone(rxBuffer_, length));
        if (receiveStatus != RADIOLIB_ERR_NONE) return receiveStatus;
        if (status != RADIOLIB_ERR_NONE) return status;
    }

    // Étape 2 : délais dépassés.
    const uint32_t now = millis();
    if (waitingForResponse_ && now - sentAt_ >= SimulatedResponseTimeoutMs) {
        waitingForResponse_ = false;
        notify(EventType::ResponseTimeout, nullptr, 0, RADIOLIB_ERR_NONE, NAN, sentZone_);
    }
    for (uint8_t i = 0; i < config_.zoneCount; ++i) {
        Transaction& transaction = transactions_[i];
        if (transaction.pending && now - transaction.receivedAt >= TransactionTimeoutMs) {
            transaction.pending = false;
            notify(EventType::TransactionExpired, nullptr, 0, RADIOLIB_ERR_NONE, NAN, i);
        }
    }

    // Étape 3 : émission. Pas d'émission si on attend la réponse à notre trame précédente,
    // ni si une trame vient d'arriver (elle sera lue au prochain appel).
    if (waitingForResponse_ || packetReceived_) return RADIOLIB_ERR_NONE;
    // Une zone prête au plus par appel : la réception reprend la main au plus vite.
    for (uint8_t i = 0; i < config_.zoneCount; ++i) {
        const Transaction& transaction = transactions_[i];
        if (transaction.pending && transaction.responseReceived
            && now - transaction.receivedAt >= config_.txDelayMs[i]) {
            return transmit(i);
        }
    }
    return RADIOLIB_ERR_NONE;
}
