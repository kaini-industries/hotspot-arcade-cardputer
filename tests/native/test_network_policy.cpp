#include <cassert>
#include <cstdint>
#include <iostream>

#include "ha_network_policy.h"

static void exhaust(HaInboundLimiter& limiter, HaInboundClass inputClass, uint8_t burst) {
    for(uint8_t i = 0; i < burst; i++) assert(haInboundAllow(limiter, inputClass, 100));
    assert(!haInboundAllow(limiter, inputClass, 100));
}

static void testRatePoliciesAndRollover() {
    HaInboundLimiter limiter = {};
    haInboundLimiterReset(limiter, 100);
    exhaust(limiter, HaInboundGeneral, 24);
    for(int i = 0; i < 12; i++) assert(haInboundAllow(limiter, HaInboundGeneral, 1100));
    assert(!haInboundAllow(limiter, HaInboundGeneral, 1100));

    haInboundLimiterReset(limiter, 100);
    exhaust(limiter, HaInboundDraw, 10);
    assert(!haInboundAllow(limiter, HaInboundDraw, 128));
    assert(haInboundAllow(limiter, HaInboundDraw, 129));
    for(int i = 0; i < 10; i++) assert(haInboundAllow(limiter, HaInboundDraw, 1129));
    assert(!haInboundAllow(limiter, HaInboundDraw, 1129));

    haInboundLimiterReset(limiter, 100);
    exhaust(limiter, HaInboundChat, 3);
    assert(!haInboundAllow(limiter, HaInboundChat, 849));
    assert(haInboundAllow(limiter, HaInboundChat, 850));

    haInboundLimiterReset(limiter, 100);
    exhaust(limiter, HaInboundEmoji, 4);
    assert(!haInboundAllow(limiter, HaInboundEmoji, 399));
    assert(haInboundAllow(limiter, HaInboundEmoji, 400));

    haInboundLimiterReset(limiter, UINT32_MAX - 50);
    exhaust(limiter, HaInboundGeneral, 24);
    assert(haInboundAllow(limiter, HaInboundGeneral, 34)); // 85 ms across rollover
}

static void testClassification() {
    assert(haInboundClass("stroke") == HaInboundDraw);
    assert(haInboundClass("say") == HaInboundChat);
    assert(haInboundClass("react") == HaInboundEmoji);
    assert(haInboundClass("hello") == HaInboundGeneral);
    assert(haInboundClass(nullptr) == HaInboundGeneral);
}

static void testSocketAdmissionAndDeadline() {
    HaSocketTable table = {};
    assert(haSocketConnect(table, 1, 100) == HaSocketAccepted);
    assert(haSocketConnect(table, 1, 100) == HaSocketDuplicate);
    assert(haSocketConnect(table, 2, 100) == HaSocketAccepted);
    assert(haSocketConnect(table, 3, 100) == HaSocketPendingFull);
    assert(!haSocketHelloExpired(*haSocketFind(table, 1), 5099));
    assert(haSocketHelloExpired(*haSocketFind(table, 1), 5100));
    assert(haSocketAuthenticate(table, 1));
    assert(!haSocketHelloExpired(*haSocketFind(table, 1), UINT32_MAX));
    assert(haSocketConnect(table, 3, 200) == HaSocketAccepted);
    assert(haSocketAuthenticate(table, 2));
    assert(haSocketAuthenticate(table, 3));

    for(uint32_t wsId = 4; wsId <= HA_WS_OBJECT_LIMIT; wsId++) {
        assert(haSocketConnect(table, wsId, 300) == HaSocketAccepted);
        assert(haSocketAuthenticate(table, wsId));
    }
    assert(haSocketCount(table, true) == HA_WS_OBJECT_LIMIT);
    assert(haSocketConnect(table, 99, 300) == HaSocketObjectsFull);
    haSocketDisconnect(table, 7);
    assert(haSocketConnect(table, 99, 400) == HaSocketAccepted);
    haSocketReset(table);
    assert(haSocketCount(table, true) == 0);
    assert(haSocketCount(table, false) == 0);
    assert(haSocketFind(table, 99) == nullptr);
    assert(haSocketConnect(table, 100, 500) == HaSocketAccepted);

    HaSocketTable wrap = {};
    assert(haSocketConnect(wrap, 5, UINT32_MAX - 1000) == HaSocketAccepted);
    assert(!haSocketHelloExpired(*haSocketFind(wrap, 5), 3998));
    assert(haSocketHelloExpired(*haSocketFind(wrap, 5), 3999));
}

int main() {
    testRatePoliciesAndRollover();
    testClassification();
    testSocketAdmissionAndDeadline();
    std::cout << "native socket/rate-policy tests passed\n";
}
