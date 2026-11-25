#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#define RADIOLIB_SX126X_MAX_PACKET_LENGTH 255
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_UNKNOWN -1
#define RADIOLIB_ERR_PACKET_TOO_LONG -4
#define RADIOLIB_ERR_CRC_MISMATCH -7

class SX1262 {
public:
    int16_t beginStatus = 0;
    int16_t receiveStatus = 0;
    int16_t receiveStatusAfterTransmit = 0;
    int16_t readStatus = 0;
    int16_t transmitStatus = 0;
    unsigned readCount = 0;
    bool interruptAttachedDuringTransmit = false;
    std::vector<uint8_t> incoming;
    std::vector<std::vector<uint8_t>> sent;
    void (*callback)() = nullptr;

    int16_t beginFSK(float, float, float, float, int8_t, uint16_t) { return beginStatus; }
    int16_t setPreambleLength(size_t) { return 0; }
    int16_t setSyncWord(uint8_t*, size_t) { return 0; }
    int16_t setRxBoostedGainMode(bool, bool) { return 0; }
    void setPacketReceivedAction(void (*action)()) { callback = action; }
    void clearPacketReceivedAction() { callback = nullptr; }
    int16_t startReceive() { return receiveStatus; }
    size_t getPacketLength() { return incoming.size(); }
    int16_t readData(uint8_t* output, size_t length) {
        ++readCount;
        std::copy(incoming.begin(), incoming.begin() + length, output);
        return readStatus;
    }
    float getRSSI() { return -50.0f; }
    int16_t transmit(const uint8_t* data, size_t length) {
        interruptAttachedDuringTransmit = callback != nullptr;
        sent.emplace_back(data, data + length);
        if (receiveStatusAfterTransmit != 0) receiveStatus = receiveStatusAfterTransmit;
        return transmitStatus;
    }
    void deliver(const std::vector<uint8_t>& frame, int16_t status = 0) {
        incoming = frame;
        readStatus = status;
        if (callback) callback();
    }
};
