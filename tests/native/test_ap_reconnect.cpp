#include <cassert>
#include <cstring>
#include <iostream>

#include "ha_ap_reconnect.h"

static constexpr const char* ID_A = "0000000000000000000000000000000a";
static constexpr const char* ID_B = "0000000000000000000000000000000b";
static constexpr const char* ID_C = "0000000000000000000000000000000c";

static HaHost hostWith(const char* first, const char* second) {
    HaHost host = {};
    const char* identities[] = {first, second};
    for(uint8_t i = 0; i < 2; i++) {
        host.p[i + 1].used = true;
        host.p[i + 1].sessionIndex = i;
        host.session[i].used = true;
        host.session[i].connected = true;
        host.session[i].pid = i + 1;
        std::strcpy(host.session[i].clientId, identities[i]);
    }
    host.sessionCount = 2;
    return host;
}

static void testExactIdentityBarrier() {
    HaHost paused = hostWith(ID_A, ID_B);
    HaApRequiredRoster required = {};
    haApRequiredCapture(required, paused);
    assert(required.count == 2);
    assert(required.unidentified == 0);

    HaHost reconnected = paused;
    reconnected.session[0].connected = false;
    reconnected.session[0].pid = 0;
    reconnected.session[1].connected = false;
    reconnected.session[1].pid = 0;
    // A different known identity cannot satisfy A's required seat merely by
    // restoring the same online-player count.
    std::strcpy(reconnected.session[0].clientId, ID_C);
    reconnected.session[0].connected = true;
    reconnected.session[0].pid = 3;
    assert(haApRequiredMissing(required, reconnected) == 2);
    assert(haApReconnectEvaluate(required, reconnected, 599999, 0, 600000) ==
           HaApReconnectDecisionWait);

    std::strcpy(reconnected.session[0].clientId, ID_A);
    assert(haApRequiredMissing(required, reconnected) == 1);
    reconnected.session[1].connected = true;
    reconnected.session[1].pid = 2;
    assert(haApReconnectEvaluate(required, reconnected, 500, 0, 600000) ==
           HaApReconnectDecisionAllRequiredOnline);
}

static void testExactRolloverSafeWindow() {
    HaHost paused = hostWith(ID_A, ID_B);
    HaApRequiredRoster required = {};
    haApRequiredCapture(required, paused);
    paused.session[1].connected = false;
    const uint32_t start = 0xfffffff0UL;
    assert(haApReconnectEvaluate(
               required, paused, (uint32_t)(start + 599999UL), start, 600000UL) ==
           HaApReconnectDecisionWait);
    assert(haApReconnectEvaluate(
               required, paused, (uint32_t)(start + 600000UL), start, 600000UL) ==
           HaApReconnectDecisionWindowExpired);

    // Readiness cannot win after the boundary. This models a hello accepted by
    // AsyncTCP just before the loop task gets its next chance to evaluate state.
    paused.session[1].connected = true;
    assert(haApReconnectEvaluate(
               required, paused, (uint32_t)(start + 600000UL), start, 600000UL) ==
           HaApReconnectDecisionWindowExpired);
    assert(haApReconnectEvaluate(
               required, paused, (uint32_t)(start + 700000UL), start, 600000UL) ==
           HaApReconnectDecisionWindowExpired);
}

static void testDeadlineIsAppliedBeforeLateInput() {
    const uint32_t start = 0xfffffff0UL;
    assert(!haApReconnectExpiresBeforeInput(
        HaApReconnectWait,
        (uint32_t)(start + 599999UL),
        start,
        600000UL));
    assert(haApReconnectExpiresBeforeInput(
        HaApReconnectWait,
        (uint32_t)(start + 600000UL),
        start,
        600000UL));
    assert(!haApReconnectExpiresBeforeInput(
        HaApRunning,
        (uint32_t)(start + 600000UL),
        start,
        600000UL));
}

static void testCrossFeatureApStatePolicy() {
    assert(haApSsidRenameAllowed(HaApRunning));
    assert(haApSsidRenameAllowed(HaApManualOff));
    assert(!haApSsidRenameAllowed(HaApReconnectWait));

    assert(haApStateAfterHistoryRestore(true) == HaApRunning);
    assert(haApStateAfterHistoryRestore(false) == HaApManualOff);
}

static void testLegacyParticipantNeverAutoResumes() {
    HaHost paused = hostWith(ID_A, ID_B);
    paused.session[1].clientId[0] = '\0';
    HaApRequiredRoster required = {};
    haApRequiredCapture(required, paused);
    assert(required.count == 1);
    assert(required.unidentified == 1);
    assert(haApRequiredMissing(required, paused) == 1);
}

static void testAlreadyOfflineSeatIsNotRequired() {
    HaHost paused = hostWith(ID_A, ID_B);
    paused.session[1].connected = false;
    paused.session[1].pid = 0;
    HaApRequiredRoster required = {};
    haApRequiredCapture(required, paused);
    assert(required.count == 1);
    assert(required.unidentified == 0);
    assert(haApRequiredContains(required, ID_A));
    assert(!haApRequiredContains(required, ID_B));
}

int main() {
    testExactIdentityBarrier();
    testExactRolloverSafeWindow();
    testDeadlineIsAppliedBeforeLateInput();
    testCrossFeatureApStatePolicy();
    testLegacyParticipantNeverAutoResumes();
    testAlreadyOfflineSeatIsNotRequired();
    std::cout << "native AP reconnect tests passed\n";
}
