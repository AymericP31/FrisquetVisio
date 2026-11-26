// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct FrisquetVisioTraceTime {
    uint32_t previousAt = 0;
    bool havePrevious = false;

    uint32_t elapsedMs(uint32_t now) const { return havePrevious ? now - previousAt : 0; }

    void record(uint32_t now) {
        previousAt = now;
        havePrevious = true;
    }

    static int format(char* out, size_t size, uint32_t elapsed) {
        if (elapsed > 2000) {
            const uint32_t tenths = elapsed / 100 + (elapsed % 100 >= 50);
            return std::snprintf(out, size, "%lu.%lus",
                                 static_cast<unsigned long>(tenths / 10),
                                 static_cast<unsigned long>(tenths % 10));
        }
        return std::snprintf(out, size, "%lums", static_cast<unsigned long>(elapsed));
    }
};
