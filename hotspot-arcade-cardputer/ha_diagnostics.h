#pragma once

#include <stdint.h>
#include "ha_device.h"

struct HaDiagnostics {
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint32_t largestFreeBlock;
    uint32_t maxLoopGapMs;
    uint32_t maxEngineLockUs;
    uint32_t rateRejected[4];
    uint32_t outputCoalesced;
    uint32_t streamDropped;
    uint32_t overloadCloses;
    uint32_t soundDropped;
    uint32_t sdFailures;
    uint32_t checkpointGeneration;
    uint16_t maxSocketQueue;
    uint8_t wsObjects;
    uint8_t wsAuthenticated;
    uint8_t wsPending;
    uint16_t boardId;
    HaDeviceKind deviceKind;
};
