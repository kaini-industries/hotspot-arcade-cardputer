#pragma once

#include <stdint.h>

// M5Unified/M5GFX stable board identifiers. Keep these numeric values independent
// of the Arduino headers so the classifier can be exercised by the native suite.
static constexpr uint16_t HA_BOARD_ID_CARDPUTER = 14;
static constexpr uint16_t HA_BOARD_ID_CARDPUTER_ADVANCE = 24;

enum HaDeviceKind : uint8_t {
    HaDeviceUnsupported = 0,
    HaDeviceCardputer = 1,
    HaDeviceCardputerAdvance = 2,
};

constexpr HaDeviceKind haDeviceClassify(uint16_t boardId) {
    return boardId == HA_BOARD_ID_CARDPUTER
               ? HaDeviceCardputer
               : boardId == HA_BOARD_ID_CARDPUTER_ADVANCE
                     ? HaDeviceCardputerAdvance
                     : HaDeviceUnsupported;
}

constexpr bool haDeviceSupported(HaDeviceKind kind) {
    return kind == HaDeviceCardputer || kind == HaDeviceCardputerAdvance;
}

constexpr const char* haDeviceName(HaDeviceKind kind) {
    return kind == HaDeviceCardputer
               ? "Cardputer"
               : kind == HaDeviceCardputerAdvance
                     ? "Cardputer Advance"
                     : "Unsupported";
}
