#include "FrisquetVisioTraceTime.h"

#include <cassert>
#include <cstring>

static void check(uint32_t elapsed, const char* expected) {
    char text[20];
    const int size = FrisquetVisioTraceTime::format(text, sizeof(text), elapsed);
    assert(size == static_cast<int>(std::strlen(expected)));
    assert(std::strcmp(text, expected) == 0);
}

int main() {
    FrisquetVisioTraceTime time;
    assert(time.elapsedMs(4775000) == 0);
    time.record(4775000);
    assert(time.elapsedMs(4775012) == 12);
    time.record(4775012);
    assert(time.elapsedMs(4775030) == 18);
    time.record(0);
    assert(time.elapsedMs(10) == 10);
    time.record(UINT32_MAX - 4);
    assert(time.elapsedMs(5) == 10);

    check(0, "0ms");
    check(12, "12ms");
    check(1999, "1999ms");
    check(2000, "2000ms");
    check(2001, "2.0s");
    check(2049, "2.0s");
    check(2050, "2.1s");
    check(2100, "2.1s");
    check(2500, "2.5s");
    check(2950, "3.0s");
    check(4775000, "4775.0s");
    check(UINT32_MAX, "4294967.3s");

    char shortText[3];
    assert(FrisquetVisioTraceTime::format(shortText, sizeof(shortText), 2001) == 4);
    assert(shortText[2] == '\0');
}
