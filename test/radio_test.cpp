#include "FrisquetVisio.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

uint32_t fakeMillis = 0;

using Mode = FrisquetVisio::Mode;
using Type = FrisquetVisio::EventType;

struct Events {
    int received = 0;
    int sent = 0;
    int expired = 0;
    int timeouts = 0;
    int confirmed = 0;
    int16_t lastTxStatus = 0;
};

static void record(const FrisquetVisio::Event& event, void* context) {
    Events& events = *static_cast<Events*>(context);
    if (event.type == Type::Receive) ++events.received;
    if (event.type == Type::Transmit) {
        ++events.sent;
        events.lastTxStatus = event.status;
    }
    if (event.type == Type::TransactionExpired) ++events.expired;
    if (event.type == Type::ResponseTimeout) ++events.timeouts;
    if (event.type == Type::TxConfirmed) ++events.confirmed;
}

static std::vector<uint8_t> satellite(uint8_t zone, uint8_t id, uint8_t number, uint8_t mode = 5) {
    std::vector<uint8_t> frame(23);
    frame[0] = 0x80;
    frame[1] = 0x08 + zone;
    frame[2] = id;
    frame[3] = number;
    frame[4] = 0x01;
    frame[5] = 0x17;
    frame[15] = 0;
    frame[16] = 225;
    frame[17] = 0;
    frame[18] = 205;
    frame[20] = mode;
    return frame;
}

static std::vector<uint8_t> response(uint8_t zone, uint8_t id, uint8_t number) {
    std::vector<uint8_t> frame(49);
    frame[0] = 0x08 + zone;
    frame[1] = 0x80;
    frame[2] = id;
    frame[3] = number;
    frame[4] = 0x81;
    frame[5] = 0x17;
    frame[7] = 0xFF;
    frame[8] = 0xCE;
    frame[17] = 6;
    frame[22] = 220;
    frame[24] = 5;
    frame[32] = 195;
    frame[34] = 5;
    frame[42] = 100;
    return frame;
}

static std::vector<uint8_t> responseEcho(uint8_t zone, const std::vector<uint8_t>& sat) {
    std::vector<uint8_t> frame = response(zone, sat[2], sat[3]);
    std::copy(sat.begin() + 15, sat.begin() + 23, frame.begin() + 19 + zone * 10);
    return frame;
}

static void receive(SX1262& radio, FrisquetVisio& visio, const std::vector<uint8_t>& frame,
                    int16_t status = RADIOLIB_ERR_NONE) {
    radio.deliver(frame, status);
    assert(visio.loop() == status);
}

static void testTransactions() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    Events events;
    FrisquetVisio::Config config;
    config.zoneCount = 2;
    config.networkId[0] = 0x12;
    config.txDelayMs[0] = 100;
    config.txDelayMs[1] = 100;
    config.acceptCrcMismatch = false;  // ce test vérifie le rejet des trames CRC
    visio.onEvent(record, &events);
    assert(visio.begin(config) == 0);
    assert(visio.setTargetTemperature(0, Mode::Heat, 22.25f));
    assert(visio.zone(0)->heatSetpoint == 22.5f);
    assert(!visio.setTargetTemperature(0, Mode::Auto, 20.0f));
    assert(!visio.setTargetTemperature(0, Mode::Heat, NAN));
    assert(!visio.setTargetTemperature(0, Mode::Heat, 24.5f));
    assert(!visio.setMode(2, Mode::Heat));
    assert(!visio.setTxDelay(0, 1001));
    assert(visio.setMode(0, Mode::Heat));

    fakeMillis = 1000;
    receive(radio, visio, satellite(0, 0x11, 0xB8));
    assert(visio.zone(0)->ambientTemperature == 22.5f);
    assert(visio.zone(0)->satelliteSetpoint == 20.5f);
    assert(std::isnan(visio.zone(0)->boilerAmbientTemperature));
    fakeMillis = 1050;
    receive(radio, visio, response(1, 0x11, 0xB8));
    receive(radio, visio, response(0, 0x12, 0xB8));
    receive(radio, visio, response(0, 0x11, 0xB8), RADIOLIB_ERR_CRC_MISMATCH);
    assert(radio.sent.empty());
    fakeMillis = 1100;
    receive(radio, visio, response(0, 0x11, 0xB8));
    assert(radio.sent.size() == 1);
    assert(radio.sent[0][3] == 0xBC);
    assert(radio.sent[0][17] == 0 && radio.sent[0][18] == 225);
    assert(radio.sent[0][20] == 0x05);
    assert(visio.boiler().outdoorTemperature == -5.0f);
    assert(visio.zone(1)->boilerSetpoint == 19.5f);
    assert(visio.zone(0)->boilerAmbientTemperature == 0.0f);
    assert(!visio.zone(0)->lastTxConfirmed && events.confirmed == 0);
    fakeMillis = 1150;
    receive(radio, visio, response(0, 0x11, 0xBC));
    assert(visio.zone(0)->lastTxConfirmed && events.confirmed == 1);

    fakeMillis = 12800;
    receive(radio, visio, satellite(0, 0x11, 0xBB));
    fakeMillis = 12900;
    receive(radio, visio, response(0, 0x11, 0xBB));
    assert(radio.sent.size() == 1);

    fakeMillis = 600000;
    receive(radio, visio, satellite(0, 0x11, 0xBC));
    fakeMillis = 600100;
    receive(radio, visio, response(0, 0x11, 0xBC));
    assert(radio.sent.size() == 2 && radio.sent.back()[3] == 0xC0);
    assert(events.sent == 2);

    fakeMillis = 602099;
    assert(visio.loop() == 0 && events.timeouts == 0);
    fakeMillis = 602100;
    assert(visio.loop() == 0 && events.timeouts == 1);
    receive(radio, visio, response(0, 0x11, 0xC0));
    assert(!visio.zone(0)->lastTxConfirmed && events.confirmed == 1);

    fakeMillis = 700000;
    receive(radio, visio, satellite(0, 0x11, 0xFC));
    fakeMillis = 700100;
    receive(radio, visio, response(0, 0x11, 0xFC));
    assert(radio.sent.back()[3] == 0x00);
    receive(radio, visio, response(0, 0x11, 0x00));

    assert(visio.setMode(0, Mode::Auto));
    fakeMillis = 800000;
    receive(radio, visio, satellite(0, 0x11, 0x00));
    fakeMillis = 800100;
    receive(radio, visio, response(0, 0x11, 0x00));
    assert(radio.sent.size() == 3);
}

static void testZonesAndTimeouts() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    Events events;
    FrisquetVisio::Config config;
    config.txDelayMs[0] = config.txDelayMs[1] = 100;
    visio.onEvent(record, &events);
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));
    assert(visio.setMode(1, Mode::Off));

    fakeMillis = 100;
    receive(radio, visio, satellite(0, 1, 0x20));
    fakeMillis = 200;
    receive(radio, visio, response(0, 1, 0x20));
    assert(radio.sent.size() == 1);
    fakeMillis = 300;
    receive(radio, visio, satellite(1, 2, 0x40, 0));
    fakeMillis = 400;
    receive(radio, visio, response(1, 2, 0x40));
    assert(radio.sent.size() == 1);
    fakeMillis = 500;
    receive(radio, visio, response(0, 1, 0x24));
    assert(radio.sent.size() == 2);
    assert(radio.sent.back()[1] == 0x09 && radio.sent.back()[3] == 0x44);
    assert(radio.sent.back()[18] == 50 && radio.sent.back()[20] == 0x10);
    receive(radio, visio, response(1, 2, 0x44));

    fakeMillis = 1000;
    receive(radio, visio, satellite(0, 1, 0x28));
    fakeMillis = 11000;
    assert(visio.loop() == 0 && events.expired == 1);
    receive(radio, visio, response(0, 1, 0x28));
    assert(radio.sent.size() == 2);

    fakeMillis = UINT32_MAX - 50;
    receive(radio, visio, satellite(0, 1, 0x2C));
    fakeMillis = 20;
    receive(radio, visio, response(0, 1, 0x2C));
    assert(radio.sent.size() == 2);
    fakeMillis = 49;
    assert(visio.loop() == 0 && radio.sent.size() == 3);
    assert(radio.sent.back()[3] == 0x30);
}

static void testConfigurationAndErrors() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.zoneCount = 0;
    assert(visio.begin(config) == FrisquetVisio::InvalidConfig);
    config.zoneCount = 3;
    config.txDelayMs[2] = 1001;
    assert(visio.begin(config) == FrisquetVisio::InvalidConfig);
    config.txDelayMs[2] = 1000;
    radio.beginStatus = -8;
    assert(visio.begin(config) == -8);
    radio.beginStatus = 0;
    assert(visio.begin(config) == 0);
    SX1262 otherRadio;
    FrisquetVisio other(otherRadio);
    assert(other.begin(config) == FrisquetVisio::RadioInUse);
    assert(visio.zone(3) == nullptr);
    assert(!visio.setMode(0, static_cast<Mode>(99)));
    assert(visio.setTxDelay(2, 1000));

    radio.receiveStatus = -42;
    radio.deliver(satellite(0, 1, 0));
    assert(visio.loop() == -42);
    assert(!visio.isReady() && visio.loop() == FrisquetVisio::NotReady);
}

static void testBroadcastSafety() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    for (uint8_t& byte : config.networkId) byte = 0xFF;
    assert(visio.setMode(0, Mode::Heat));
    assert(visio.begin(config) == FrisquetVisio::InvalidConfig);
    assert(visio.setMode(0, Mode::Auto));
    assert(visio.begin(config) == 0);
    assert(!visio.setMode(0, Mode::Heat));
    fakeMillis = 100;
    receive(radio, visio, satellite(0, 1, 0));
    fakeMillis = 250;
    receive(radio, visio, response(0, 1, 0));
    assert(radio.sent.empty());
}

static void testDelayModeAndRetries() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.txDelayMs[0] = 1000;
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));
    assert(visio.setTargetTemperature(0, Mode::Off, 6.25f));

    fakeMillis = 10000;
    receive(radio, visio, satellite(0, 0x44, 0xB9));
    fakeMillis = 10070;
    receive(radio, visio, response(0, 0x44, 0xB8));
    assert(radio.sent.empty());
    fakeMillis = 10100;
    receive(radio, visio, response(0, 0x44, 0xB9));
    assert(radio.sent.empty());
    fakeMillis = 10999;
    assert(visio.loop() == 0 && radio.sent.empty());
    assert(visio.setMode(0, Mode::Off));
    fakeMillis = 11000;
    assert(visio.loop() == 0 && radio.sent.size() == 1);
    assert(radio.sent.back()[3] == 0xBC);
    assert(radio.sent.back()[17] == 0 && radio.sent.back()[18] == 65);
    assert(radio.sent.back()[20] == 0x14);
    receive(radio, visio, response(0, 0x44, 0xBC));
    assert(visio.zone(0)->lastTxConfirmed);

    assert(visio.setTxDelay(0, 0));
    assert(visio.setMode(0, Mode::Heat));
    fakeMillis = 13000;
    receive(radio, visio, satellite(0, 0x44, 0xC0, 0));
    assert(radio.sent.size() == 1);
    receive(radio, visio, response(0, 0x44, 0xC0));
    assert(radio.sent.size() == 2);
    assert(radio.sent.back()[3] == 0xC4 && radio.sent.back()[20] == 0x01);
    receive(radio, visio, response(0, 0x44, 0xC4));

    fakeMillis = 14000;
    receive(radio, visio, satellite(0, 0x44, 0xC4));
    assert(visio.setMode(0, Mode::Auto));
    receive(radio, visio, response(0, 0x44, 0xC4));
    assert(radio.sent.size() == 2);
}

static void testHydraulicFrames() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.acceptCrcMismatch = false;  // la trame CRC ci-dessous doit être ignorée
    assert(visio.begin(config) == 0);

    fakeMillis = 0;
    std::vector<uint8_t> boilerFrame = response(0, 1, 0);
    boilerFrame[42] = 215;
    boilerFrame[44] = 5;
    receive(radio, visio, boilerFrame);
    assert(visio.boiler().dataValid && visio.boiler().mode == 6);
    assert(visio.zone(2)->boilerValid && visio.zone(2)->boilerSetpoint == 21.5f);
    assert(visio.zone(2)->boilerRadioMode == 5);

    std::vector<uint8_t> command(19);
    command[0] = 0x25;
    command[1] = 0x80;
    command[4] = 0x03;
    command[5] = 0x17;
    command[16] = 205;
    command[18] = 3;
    receive(radio, visio, command);
    assert(visio.hydraulic(0)->setpointValid && !visio.hydraulic(1)->setpointValid);
    assert(visio.hydraulic(0)->waterSetpoint == 20.5f && visio.hydraulic(0)->mode == 3);
    assert(visio.hydraulic(2) == nullptr);

    command[16] = 100;
    receive(radio, visio, command, RADIOLIB_ERR_CRC_MISMATCH);
    assert(visio.hydraulic(0)->waterSetpoint == 20.5f);

    command[0] = 0x26;
    command[18] = 2;
    receive(radio, visio, command);
    assert(visio.hydraulic(1)->setpointValid && visio.hydraulic(1)->mode == 2);
    assert(visio.hydraulic(1)->waterSetpoint == 10.0f && visio.hydraulic(0)->mode == 3);
    command[0] = 0x27;
    command[18] = 1;
    receive(radio, visio, command);
    assert(visio.hydraulic(1)->mode == 2);

    std::vector<uint8_t> answer(9);
    answer[0] = 0x80;
    answer[1] = 0x25;
    answer[4] = 0x83;
    answer[5] = 0x17;
    answer[7] = 0xFF;
    answer[8] = 0x9C;
    receive(radio, visio, answer);
    assert(visio.hydraulic(0)->temperatureValid);
    assert(visio.hydraulic(0)->waterTemperature == -10.0f);
    assert(!visio.hydraulic(1)->temperatureValid);

    std::vector<uint8_t> mode(15);
    mode[0] = 0x25;
    mode[1] = 0x80;
    mode[4] = 0x81;
    mode[5] = 0x17;
    mode[13] = 4;
    receive(radio, visio, mode);
    assert(visio.boiler().modeValid && visio.boiler().mode == 4);
    mode[5] = 0;
    mode[13] = 9;
    receive(radio, visio, mode);
    assert(visio.boiler().mode == 4);
}

static void testTransmitFailure() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    Events events;
    FrisquetVisio::Config config;
    config.txDelayMs[0] = 0;
    visio.onEvent(record, &events);
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));

    fakeMillis = 100;
    receive(radio, visio, satellite(0, 1, 0));
    receive(radio, visio, response(0, 1, 0));
    receive(radio, visio, response(0, 1, 4));
    assert(visio.zone(0)->lastTxConfirmed && visio.zone(0)->txCount == 1);

    fakeMillis = 200;
    radio.transmitStatus = -91;
    receive(radio, visio, satellite(0, 1, 4));
    radio.deliver(response(0, 1, 4));
    assert(visio.loop() == -91);
    assert(events.lastTxStatus == -91 && visio.isReady());
    assert(!visio.zone(0)->lastTxConfirmed && visio.zone(0)->txCount == 1);

    radio.transmitStatus = 0;
    receive(radio, visio, satellite(0, 1, 5));
    receive(radio, visio, response(0, 1, 5));
    assert(radio.sent.size() == 3 && radio.sent.back()[3] == 8);
    assert(visio.zone(0)->txCount == 2 && !visio.zone(0)->lastTxConfirmed);
}

static void testDuplicateWindow() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.txDelayMs[0] = 0;
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));

    fakeMillis = 1000;
    receive(radio, visio, satellite(0, 1, 0x40));
    receive(radio, visio, response(0, 1, 0x40));
    receive(radio, visio, response(0, 1, 0x44));
    assert(radio.sent.size() == 1);

    fakeMillis = 60999;
    receive(radio, visio, satellite(0, 1, 0x41));
    receive(radio, visio, response(0, 1, 0x41));
    assert(radio.sent.size() == 1);

    fakeMillis = 61000;
    receive(radio, visio, satellite(0, 1, 0x42));
    receive(radio, visio, response(0, 1, 0x42));
    assert(radio.sent.size() == 2 && radio.sent.back()[3] == 0x44);
}

static void testReceiveFailureAfterTransmit() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    Events events;
    FrisquetVisio::Config config;
    config.txDelayMs[0] = 0;
    visio.onEvent(record, &events);
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));
    radio.receiveStatusAfterTransmit = -42;

    fakeMillis = 100;
    receive(radio, visio, satellite(0, 1, 0x10));
    radio.deliver(response(0, 1, 0x10));
    assert(visio.loop() == -42);
    assert(events.sent == 1 && events.lastTxStatus == 0);
    assert(visio.zone(0)->txCount == 1 && !visio.zone(0)->lastTxConfirmed);
    assert(!visio.isReady() && visio.loop() == FrisquetVisio::NotReady);
}

static void countInterrupt(void* context) {
    ++*static_cast<unsigned*>(context);
}

static void testInterruptHook() {
    SX1262 radio;
    unsigned wakeups = 0;
    unsigned rejectedContext = 0;
    {
        FrisquetVisio visio(radio);
        Events events;
        FrisquetVisio::Config config;
        assert(config.zoneCount == 3 && config.txDelayMs[0] == 150);
        assert(visio.zone(0)->mode == Mode::Auto);
        assert(visio.zone(0)->heatSetpoint == 20.0f && visio.zone(0)->frostSetpoint == 5.0f);
        visio.onEvent(record, &events);
        assert(visio.onRadioInterrupt(countInterrupt, &wakeups));
        assert(visio.begin(config) == 0);
        assert(!visio.onRadioInterrupt(countInterrupt, &rejectedContext));
        assert(!visio.onRadioInterrupt(nullptr));
        assert(visio.setMode(0, Mode::Heat));

        fakeMillis = 100;
        radio.deliver(satellite(0, 1, 0x40));
        assert(wakeups == 1 && rejectedContext == 0);
        assert(radio.readCount == 0 && events.received == 0);
        assert(!visio.zone(0)->satelliteValid);
        assert(visio.loop() == 0);
        assert(radio.readCount == 1 && events.received == 1);
        assert(visio.zone(0)->satelliteValid && radio.sent.empty());

        fakeMillis = 150;
        receive(radio, visio, response(0, 1, 0x40));
        assert(wakeups == 2 && radio.sent.empty());
        fakeMillis = 249;
        assert(visio.loop() == 0 && radio.sent.empty());
        fakeMillis = 250;
        assert(visio.loop() == 0);
        assert(wakeups == 2 && radio.sent.size() == 1);
        assert(!radio.interruptAttachedDuringTransmit);
        assert(radio.sent.back()[18] == 200);
        receive(radio, visio, response(0, 1, 0x44));
        assert(wakeups == 3 && visio.zone(0)->lastTxConfirmed);
    }
    radio.deliver(satellite(0, 1, 0x44));
    assert(wakeups == 3 && radio.callback == nullptr);
}

static void testInterruptHookDisabled() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    unsigned wakeups = 0;
    assert(visio.onRadioInterrupt(countInterrupt, &wakeups));
    assert(visio.onRadioInterrupt(nullptr));
    assert(visio.begin(FrisquetVisio::Config{}) == 0);
    fakeMillis = 100;
    receive(radio, visio, satellite(0, 1, 0));
    assert(wakeups == 0 && visio.zone(0)->satelliteValid);
}

static void testAcceptCrcMismatch() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.networkId[0] = 0x12;
    config.txDelayMs[0] = 0;
    assert(config.acceptCrcMismatch);  // défaut : trames CRC traitées normalement
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));

    fakeMillis = 900000;
    receive(radio, visio, satellite(0, 0x21, 0x40), RADIOLIB_ERR_CRC_MISMATCH);
    assert(visio.zone(0)->satelliteValid);
    const std::vector<uint8_t> sat = satellite(0, 0x21, 0x40);
    receive(radio, visio, responseEcho(0, sat), RADIOLIB_ERR_CRC_MISMATCH);
    assert(radio.sent.empty() && visio.loop() == 0);
    assert(radio.sent.size() == 1 && radio.sent[0][3] == 0x44);
    receive(radio, visio, response(0, 0x21, 0x44), RADIOLIB_ERR_CRC_MISMATCH);
    assert(visio.zone(0)->lastTxConfirmed);

    // CRC + consigne hors plage (4.0 °C) : trame ignorée, aucune transaction.
    fakeMillis += 600000;
    std::vector<uint8_t> bad = satellite(0, 0x21, 0x44);
    bad[18] = 40;
    const float before = visio.zone(0)->satelliteSetpoint;
    receive(radio, visio, bad, RADIOLIB_ERR_CRC_MISMATCH);
    receive(radio, visio, response(0, 0x21, 0x44), RADIOLIB_ERR_NONE);
    assert(visio.zone(0)->satelliteSetpoint == before);
    assert(visio.loop() == 0 && radio.sent.size() == 1);
    // Même trame sans erreur CRC : décodée normalement.
    receive(radio, visio, bad);
    assert(visio.zone(0)->satelliteSetpoint == 4.0f);

}

static void testCrcEchoMismatch() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.networkId[0] = 0x12;
    config.txDelayMs[0] = 0;
    assert(visio.begin(config) == 0);
    assert(visio.setMode(0, Mode::Heat));
    fakeMillis = 1500000;
    const std::vector<uint8_t> sat = satellite(0, 0x22, 0x50);
    receive(radio, visio, sat, RADIOLIB_ERR_CRC_MISMATCH);
    std::vector<uint8_t> mismatchedEcho = responseEcho(0, sat);
    mismatchedEcho[19] ^= 1;
    receive(radio, visio, mismatchedEcho);
    assert(visio.loop() == 0 && radio.sent.empty());
}

static void testForumFrames() {
    SX1262 radio;
    FrisquetVisio visio(radio);
    FrisquetVisio::Config config;
    config.zoneCount = 2;
    assert(visio.begin(config) == 0);

    // 129,0 °C (0x050A) = pas de sonde extérieure ; mode 0x28 accepté même avec CRC faux.
    std::vector<uint8_t> boilerFrame = response(0, 1, 0);
    boilerFrame[7] = 0x05;
    boilerFrame[8] = 0x0A;
    boilerFrame[17] = 0x28;
    receive(radio, visio, boilerFrame, RADIOLIB_ERR_CRC_MISMATCH);
    assert(visio.boiler().dataValid && std::isnan(visio.boiler().outdoorTemperature));
    assert(visio.boiler().mode == 0x28);
    boilerFrame[17] = 0x48;
    receive(radio, visio, boilerFrame, RADIOLIB_ERR_CRC_MISMATCH);
    assert(visio.boiler().mode == 0x28);

    // Trame 0x43 « SATELLITE Z2 / CHANGER PILES » puis message effacé.
    const char text[] = "SATELLITE Z2\0CHANGER PILES \0";
    std::vector<uint8_t> message = {0x80, 0x09, 0x00, 0xB4, 0x01, 0x43, 0x00, 0x1C};
    message.insert(message.end(), text, text + 28);
    receive(radio, visio, message);
    assert(visio.zone(1)->batteryLow && !visio.zone(0)->batteryLow);
    receive(radio, visio, {0x80, 0x09, 0x00, 0x04, 0x01, 0x43, 0x00, 0x01});  // longueur fausse
    assert(visio.zone(1)->batteryLow);
    receive(radio, visio, {0x80, 0x09, 0x00, 0x04, 0x01, 0x43, 0x00, 0x00});
    assert(!visio.zone(1)->batteryLow);
}

int main() {
    testTransactions();
    testForumFrames();
    testAcceptCrcMismatch();
    testCrcEchoMismatch();
    testZonesAndTimeouts();
    testConfigurationAndErrors();
    testBroadcastSafety();
    testDelayModeAndRetries();
    testHydraulicFrames();
    testTransmitFailure();
    testDuplicateWindow();
    testReceiveFailureAfterTransmit();
    testInterruptHook();
    testInterruptHookDisabled();
}
