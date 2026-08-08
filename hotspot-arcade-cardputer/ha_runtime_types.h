#pragma once

#include <Arduino.h>
#include "ha_network_policy.h"
#include "ha_ws_flow_policy.h"

// Declared in a real header so Arduino's generated function prototypes see the
// types before prototypes for sketch-local helpers that use them.
struct HaWsFlowState {
    uint32_t wsId;
    HaWsDirtyTracker dirty;
    String lobby;
    String state;
    HaWsQueueTracker queue;
};

struct HaAuthCounter {
    bool windowStarted;
    uint8_t failures;
    uint32_t windowStartMs;
    uint32_t lockUntilMs;
};

struct HaAuthClientBucket {
    bool used;
    uint32_t clientKey;
    uint32_t lastSeenMs;
    HaAuthCounter counter;
};
