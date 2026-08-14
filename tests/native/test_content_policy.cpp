#include <cassert>
#include <cstdint>
#include <iostream>

#include "ha_content_policy.h"

int main() {
    static_assert(HA_CONTENT_INTERNAL_RESERVE_BYTES == 64U * 1024U);
    static_assert(HA_CONTENT_FRANKENDRAW_FALLBACK_BUDGET_BYTES == 32U * 1024U);
    static_assert(HA_CONTENT_MUTATION_GUARD_BYTES == 96U * 1024U);

    assert(!haContentInternalReserveHeld(HA_CONTENT_INTERNAL_RESERVE_BYTES - 1));
    assert(haContentInternalReserveHeld(HA_CONTENT_INTERNAL_RESERVE_BYTES));

    assert(!haContentInternalFallbackFits(SIZE_MAX, 0));
    assert(!haContentInternalFallbackFits(
        SIZE_MAX,
        SIZE_MAX - HA_CONTENT_INTERNAL_RESERVE_BYTES + 1));
    assert(!haContentInternalFallbackFits(
        HA_CONTENT_INTERNAL_RESERVE_BYTES + 4095,
        4096));
    assert(haContentInternalFallbackFits(
        HA_CONTENT_INTERNAL_RESERVE_BYTES + 4096,
        4096));

    assert(!haContentMutationGuardHeld(HA_CONTENT_MUTATION_GUARD_BYTES - 1));
    assert(haContentMutationGuardHeld(HA_CONTENT_MUTATION_GUARD_BYTES));
    std::cout << "native content-allocation policy tests passed\n";
}
