// Allocation-free SSID configuration transaction. Persistence always precedes
// runtime mutation. If the candidate AP cannot start, the old SSID is made
// durable before it is restored in RAM; a failed rollback deliberately leaves
// the last successfully persisted candidate in RAM with transport stopped.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef bool (*HaSsidPersistFn)(const char* ssid);
typedef bool (*HaSsidStartFn)(const char* ssid);

enum HaSsidTransactionResult : uint8_t {
    HaSsidNoChange = 0,
    HaSsidAppliedOffline,
    HaSsidAppliedRunning,
    HaSsidCandidateRejectedPriorRunning,
    HaSsidCandidateRejectedPriorOffline,
    HaSsidFallbackRunning,
    HaSsidFallbackOffline,
    HaSsidRollbackRejectedCandidateOffline,
};

static inline bool haSsidCopy(char* destination, size_t capacity, const char* source) {
    if(!destination || !source || capacity < 2) return false;
    size_t length = strnlen(source, capacity);
    if(!length || length >= capacity) return false;
    memcpy(destination, source, length + 1);
    return true;
}

static inline HaSsidTransactionResult haSsidApplyTransaction(
    char* runtimeSsid,
    size_t capacity,
    const char* candidate,
    bool restartTransport,
    HaSsidPersistFn persist,
    HaSsidStartFn start) {
    if(!runtimeSsid || !candidate || !persist || (restartTransport && !start) ||
       !candidate[0] || strnlen(candidate, capacity) >= capacity)
        return HaSsidNoChange;
    if(strcmp(runtimeSsid, candidate) == 0) return HaSsidNoChange;

    char previous[33];
    if(capacity > sizeof(previous) || !haSsidCopy(previous, capacity, runtimeSsid))
        return HaSsidNoChange;

    if(!persist(candidate)) {
        if(!restartTransport) return HaSsidNoChange;
        return start(previous) ? HaSsidCandidateRejectedPriorRunning :
                                 HaSsidCandidateRejectedPriorOffline;
    }
    (void)haSsidCopy(runtimeSsid, capacity, candidate);
    if(!restartTransport) return HaSsidAppliedOffline;
    if(start(runtimeSsid)) return HaSsidAppliedRunning;

    // Do not expose the fallback in RAM until it is the newest durable value.
    if(!persist(previous)) return HaSsidRollbackRejectedCandidateOffline;
    (void)haSsidCopy(runtimeSsid, capacity, previous);
    return start(runtimeSsid) ? HaSsidFallbackRunning : HaSsidFallbackOffline;
}
