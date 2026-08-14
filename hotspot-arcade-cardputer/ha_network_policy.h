// Bounded, allocation-free WebSocket admission and inbound rate policy.
//
// This header is deliberately independent of Wi-Fi and AsyncWebServer so the
// timing/capacity rules can be exercised by the native sanitizer suite. The .ino
// owns socket close/send mechanics and calls these helpers from its callbacks.
#pragma once

#include <stdint.h>
#include <string.h>

#define HA_WS_AUTH_LIMIT 10
#define HA_WS_PENDING_LIMIT 2
#define HA_WS_OBJECT_LIMIT 12
#define HA_WS_HELLO_DEADLINE_MS 5000UL

enum HaInboundClass : uint8_t {
    HaInboundGeneral = 0,
    HaInboundDraw = 1,
    HaInboundChat = 2,
    HaInboundEmoji = 3,
    HaInboundClassCount = 4,
};

struct HaRatePolicy {
    uint16_t rateNumerator;
    uint16_t rateDenominator;
    uint8_t burst;
};

static const HaRatePolicy HA_RATE_POLICIES[HaInboundClassCount] = {
    {12, 1, 24}, // general control: 12/s, burst 24
    {35, 1, 10}, // Draw strokes: 35/s, burst 10
    {4, 3, 3},   // chat: 1/750 ms, burst 3
    {10, 3, 4},  // emoji: 1/300 ms, burst 4
};

struct HaTokenBucket {
    uint32_t tokensQ16;
    uint32_t refillRemainder;
    uint32_t lastMs;
};

struct HaInboundLimiter {
    bool initialized;
    HaTokenBucket buckets[HaInboundClassCount];
};

static inline HaInboundClass haInboundClass(const char* type) {
    if(type && strcmp(type, "stroke") == 0) return HaInboundDraw;
    if(type && strcmp(type, "say") == 0) return HaInboundChat;
    if(type && strcmp(type, "react") == 0) return HaInboundEmoji;
    return HaInboundGeneral;
}

static inline void haInboundLimiterReset(HaInboundLimiter& limiter, uint32_t now) {
    limiter = HaInboundLimiter{};
    limiter.initialized = true;
    for(uint8_t i = 0; i < HaInboundClassCount; i++) {
        limiter.buckets[i].tokensQ16 =
            (uint32_t)HA_RATE_POLICIES[i].burst << 16;
        limiter.buckets[i].lastMs = now;
    }
}

static inline bool haInboundAllow(
    HaInboundLimiter& limiter,
    HaInboundClass inputClass,
    uint32_t now) {
    if(inputClass >= HaInboundClassCount) return false;
    if(!limiter.initialized) haInboundLimiterReset(limiter, now);
    const HaRatePolicy& policy = HA_RATE_POLICIES[inputClass];
    HaTokenBucket& bucket = limiter.buckets[inputClass];
    uint32_t elapsed = now - bucket.lastMs; // rollover-safe unsigned elapsed time
    bucket.lastMs = now;
    if(elapsed) {
        const uint32_t denominator = 1000UL * policy.rateDenominator;
        uint64_t refill = (uint64_t)elapsed * 65536ULL * policy.rateNumerator +
                          bucket.refillRemainder;
        uint64_t added = refill / denominator;
        bucket.refillRemainder = (uint32_t)(refill % denominator);
        uint32_t capacity = (uint32_t)policy.burst << 16;
        uint64_t replenished = (uint64_t)bucket.tokensQ16 + added;
        bucket.tokensQ16 = replenished > capacity ? capacity : (uint32_t)replenished;
    }
    if(bucket.tokensQ16 < 65536UL) return false;
    bucket.tokensQ16 -= 65536UL;
    return true;
}

struct HaSocketSlot {
    bool used;
    bool authenticated;
    uint32_t wsId;
    uint32_t connectedAtMs;
    HaInboundLimiter limiter;
};

struct HaSocketTable {
    HaSocketSlot slots[HA_WS_OBJECT_LIMIT];
};

static inline void haSocketReset(HaSocketTable& table) {
    table = HaSocketTable{};
}

enum HaSocketAdmission : uint8_t {
    HaSocketAccepted = 0,
    HaSocketDuplicate = 1,
    HaSocketPendingFull = 2,
    HaSocketObjectsFull = 3,
};

static inline HaSocketSlot* haSocketFind(HaSocketTable& table, uint32_t wsId) {
    if(!wsId) return nullptr;
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++)
        if(table.slots[i].used && table.slots[i].wsId == wsId) return &table.slots[i];
    return nullptr;
}

static inline uint8_t haSocketCount(const HaSocketTable& table, bool authenticated) {
    uint8_t count = 0;
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++)
        if(table.slots[i].used && table.slots[i].authenticated == authenticated) count++;
    return count;
}

static inline HaSocketAdmission haSocketConnect(
    HaSocketTable& table,
    uint32_t wsId,
    uint32_t now) {
    if(!wsId || haSocketFind(table, wsId)) return HaSocketDuplicate;
    if(haSocketCount(table, false) >= HA_WS_PENDING_LIMIT) return HaSocketPendingFull;
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++) {
        HaSocketSlot& slot = table.slots[i];
        if(slot.used) continue;
        slot = HaSocketSlot{};
        slot.used = true;
        slot.wsId = wsId;
        slot.connectedAtMs = now;
        haInboundLimiterReset(slot.limiter, now);
        return HaSocketAccepted;
    }
    return HaSocketObjectsFull;
}

static inline bool haSocketAuthenticate(HaSocketTable& table, uint32_t wsId) {
    HaSocketSlot* slot = haSocketFind(table, wsId);
    if(!slot) return false;
    slot->authenticated = true;
    return true;
}

static inline void haSocketDisconnect(HaSocketTable& table, uint32_t wsId) {
    HaSocketSlot* slot = haSocketFind(table, wsId);
    if(slot) *slot = HaSocketSlot{};
}

static inline bool haSocketHelloExpired(const HaSocketSlot& slot, uint32_t now) {
    return slot.used && !slot.authenticated &&
           (uint32_t)(now - slot.connectedAtMs) >= HA_WS_HELLO_DEADLINE_MS;
}
