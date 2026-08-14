// Pure allocation thresholds for active-game content.
//
// Kept independent of ESP heap APIs so exact boundary/overflow behavior is part
// of the native sanitizer suite as well as the board integration.
#pragma once

#include <stddef.h>
#include <stdint.h>

static constexpr size_t HA_CONTENT_INTERNAL_RESERVE_BYTES = 64U * 1024U;
static constexpr size_t HA_CONTENT_FRANKENDRAW_FALLBACK_BUDGET_BYTES = 32U * 1024U;
static constexpr size_t HA_CONTENT_MUTATION_GUARD_BYTES =
    HA_CONTENT_INTERNAL_RESERVE_BYTES +
    HA_CONTENT_FRANKENDRAW_FALLBACK_BUDGET_BYTES;

static inline bool haContentInternalReserveHeld(size_t freeBytes) {
    return freeBytes >= HA_CONTENT_INTERNAL_RESERVE_BYTES;
}

static inline bool haContentInternalFallbackFits(
    size_t freeBytes,
    size_t requestedBytes) {
    if(!requestedBytes ||
       requestedBytes > SIZE_MAX - HA_CONTENT_INTERNAL_RESERVE_BYTES)
        return false;
    return freeBytes >= requestedBytes + HA_CONTENT_INTERNAL_RESERVE_BYTES;
}

static inline bool haContentMutationGuardHeld(size_t freeBytes) {
    return freeBytes >= HA_CONTENT_MUTATION_GUARD_BYTES;
}
