#include <cassert>
#include <cstdint>
#include <iostream>

#include "ha_ws_flow_policy.h"

static void testClassification() {
    assert(haWsClassifyOutput(nullptr, false) == HaWsOutputControl);
    assert(haWsClassifyOutput("welcome", false) == HaWsOutputControl);
    assert(haWsClassifyOutput("lobby", false) == HaWsOutputLobbyState);
    assert(haWsClassifyOutput("ink", false) == HaWsOutputStream);

    // Heartbeat pong and authoritative Pong state deliberately differ.
    assert(haWsClassifyOutput("pong", false) == HaWsOutputControl);
    assert(haWsClassifyOutput("pong", true) == HaWsOutputGameStateStream);

    static const char* const gameStates[] = {
        "trivia", "duel", "draw", "wyr", "scramble", "react", "gc",
        "bs", "spectrum", "kmk", "chess"
    };
    for(const char* type : gameStates)
        assert(haWsClassifyOutput(type, false) == HaWsOutputGameState);
}

static void testThresholdBoundaries() {
    assert(haWsChooseOutputAction(HaWsOutputGameState, 3, 0) == HaWsOutputSend);
    assert(haWsChooseOutputAction(HaWsOutputGameState, 4, 0) ==
           HaWsOutputCoalesce);
    assert(haWsChooseOutputAction(HaWsOutputGameState, 8, 0) ==
           HaWsOutputCoalesce);

    assert(haWsChooseOutputAction(HaWsOutputStream, 7, 0) == HaWsOutputSend);
    assert(haWsChooseOutputAction(HaWsOutputStream, 8, 0) ==
           HaWsOutputDropStream);
    assert(haWsChooseOutputAction(HaWsOutputGameStateStream, 3, 0) ==
           HaWsOutputSend);
    assert(haWsChooseOutputAction(HaWsOutputGameStateStream, 4, 0) ==
           HaWsOutputCoalesce);
    assert(haWsChooseOutputAction(HaWsOutputGameStateStream, 7, 0) ==
           HaWsOutputCoalesce);
    assert(haWsChooseOutputAction(HaWsOutputGameStateStream, 8, 0) ==
           HaWsOutputDropStream);

    // Total depth alone must never close a control message.
    assert(haWsChooseOutputAction(HaWsOutputControl, 31, 0) == HaWsOutputSend);
    assert(haWsChooseOutputAction(HaWsOutputControl, 31, 15) == HaWsOutputSend);
    assert(haWsChooseOutputAction(HaWsOutputControl, 16, 16) ==
           HaWsOutputCloseControl);
    assert(haWsChooseOutputAction(HaWsOutputControl, 3, 16) ==
           HaWsOutputCloseControl);
}

static void testSendFailureActions() {
    assert(haWsChooseSendFailureAction(HaWsOutputControl) ==
           HaWsFailureCloseClient);
    assert(haWsChooseSendFailureAction(HaWsOutputLobbyState) ==
           HaWsFailureCacheSnapshot);
    assert(haWsChooseSendFailureAction(HaWsOutputGameState) ==
           HaWsFailureCacheSnapshot);
    assert(haWsChooseSendFailureAction(HaWsOutputGameStateStream) ==
           HaWsFailureCacheSnapshot);
    assert(haWsChooseSendFailureAction(HaWsOutputStream) ==
           HaWsFailureDropMessage);

    // A failed heartbeat is control loss and must force resume recovery.
    HaWsOutputClass heartbeat = haWsClassifyOutput("pong", false);
    assert(haWsChooseSendFailureAction(heartbeat) == HaWsFailureCloseClient);
}

static void testQueueTracking() {
    HaWsQueueTracker tracker = {};
    assert(haWsQueueRecord(tracker, HaWsOutputGameState));
    assert(haWsQueueRecord(tracker, HaWsOutputControl));
    assert(haWsQueueRecord(tracker, HaWsOutputControl));
    assert(haWsQueueRecord(tracker, HaWsOutputStream));
    assert(tracker.count == 4);
    assert(tracker.controlCount == 2);

    // FIFO completion removes the state and first control.
    haWsQueueObserve(tracker, 2);
    assert(tracker.count == 2);
    assert(tracker.controlCount == 1);
    haWsQueueObserve(tracker, 0);
    assert(tracker.count == 0);
    assert(tracker.controlCount == 0);

    // Unexpected queue growth is tracked but cannot be misclassified as control.
    haWsQueueObserve(tracker, 9);
    assert(tracker.count == 9);
    assert(tracker.controlCount == 0);
    haWsQueueObserve(tracker, 3);
    assert(tracker.count == 3);
    assert(tracker.controlCount == 0);
    haWsQueueObserve(tracker, 0);

    // Ring wraparound preserves exact control counts.
    for(uint8_t i = 0; i < 20; i++)
        assert(haWsQueueRecord(
            tracker,
            (i & 1U) ? HaWsOutputControl : HaWsOutputGameState));
    assert(tracker.controlCount == 10);
    haWsQueueObserve(tracker, 5);
    assert(tracker.controlCount == 3);
    for(uint8_t i = 0; i < 20; i++)
        assert(haWsQueueRecord(tracker, HaWsOutputControl));
    assert(tracker.count == 25);
    assert(tracker.controlCount == 23);
    haWsQueueObserve(tracker, 0);
    assert(tracker.controlCount == 0);
}

static void testControlCountDrivesClose() {
    HaWsQueueTracker tracker = {};
    for(uint8_t i = 0; i < 12; i++)
        assert(haWsQueueRecord(tracker, HaWsOutputGameState));
    for(uint8_t i = 0; i < 15; i++)
        assert(haWsQueueRecord(tracker, HaWsOutputControl));
    assert(tracker.count == 27);
    assert(tracker.controlCount == 15);
    assert(haWsChooseOutputAction(
               HaWsOutputControl, tracker.count, tracker.controlCount) ==
           HaWsOutputSend);
    assert(haWsQueueRecord(tracker, HaWsOutputControl));
    assert(haWsChooseOutputAction(
               HaWsOutputControl, tracker.count, tracker.controlCount) ==
           HaWsOutputCloseControl);
}

static void testDirtyRetryDecisions() {
    assert(haWsChooseDirtyRetry(true, true, 3) == HaWsDirtyLobby);
    assert(haWsChooseDirtyRetry(false, true, 3) == HaWsDirtyState);
    assert(haWsChooseDirtyRetry(true, false, 4) == HaWsDirtyNone);
    assert(haWsChooseDirtyRetry(false, true, 4) == HaWsDirtyNone);
    assert(haWsChooseDirtyRetry(false, false, 0) == HaWsDirtyNone);

    // Once the lobby retry is accepted, the state can follow only while the
    // actual queue remains below the coalescing threshold.
    assert(haWsChooseDirtyRetry(false, true, 3) == HaWsDirtyState);
    assert(haWsChooseDirtyRetry(false, true, 4) == HaWsDirtyNone);
}

int main() {
    testClassification();
    testThresholdBoundaries();
    testSendFailureActions();
    testQueueTracking();
    testControlCountDrivesClose();
    testDirtyRetryDecisions();
    std::cout << "native outbound-flow-policy tests passed\n";
}

