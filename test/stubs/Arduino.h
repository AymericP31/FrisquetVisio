#pragma once
#include <cstdint>
#define IRAM_ATTR
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
