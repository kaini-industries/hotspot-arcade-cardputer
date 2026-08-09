#include <cassert>
#include <cstdio>
#include <cstring>

#include "ha_device.h"

static_assert(HA_BOARD_ID_CARDPUTER == 14, "Cardputer board id must remain stable");
static_assert(
    HA_BOARD_ID_CARDPUTER_ADVANCE == 24,
    "Cardputer Advance board id must remain stable");
static_assert(
    haDeviceClassify(HA_BOARD_ID_CARDPUTER) == HaDeviceCardputer,
    "Cardputer classification failed");
static_assert(
    haDeviceClassify(HA_BOARD_ID_CARDPUTER_ADVANCE) == HaDeviceCardputerAdvance,
    "Cardputer Advance classification failed");
static_assert(
    haDeviceClassify(0) == HaDeviceUnsupported,
    "unknown boards must be rejected");
static_assert(haDeviceSupported(HaDeviceCardputer), "Cardputer must be supported");
static_assert(
    haDeviceSupported(HaDeviceCardputerAdvance),
    "Cardputer Advance must be supported");
static_assert(
    !haDeviceSupported(HaDeviceUnsupported),
    "unknown boards must not be supported");

int main() {
    assert(std::strcmp(haDeviceName(HaDeviceCardputer), "Cardputer") == 0);
    assert(
        std::strcmp(
            haDeviceName(HaDeviceCardputerAdvance),
            "Cardputer Advance") == 0);
    assert(std::strcmp(haDeviceName(HaDeviceUnsupported), "Unsupported") == 0);
    assert(haDeviceClassify(UINT16_MAX) == HaDeviceUnsupported);
    std::puts("native device-classifier tests passed");
    return 0;
}
