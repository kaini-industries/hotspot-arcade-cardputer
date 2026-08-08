#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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
    HaWsDirtyTracker dirty = {true, true};
    assert(haWsChooseDirtyRetry(dirty, 3) == HaWsDirtyLobby);
    dirty = HaWsDirtyTracker{false, true};
    assert(haWsChooseDirtyRetry(dirty, 3) == HaWsDirtyState);
    dirty = HaWsDirtyTracker{true, false};
    assert(haWsChooseDirtyRetry(dirty, 4) == HaWsDirtyNone);
    dirty = HaWsDirtyTracker{false, true};
    assert(haWsChooseDirtyRetry(dirty, 4) == HaWsDirtyNone);
    dirty = HaWsDirtyTracker{};
    assert(haWsChooseDirtyRetry(dirty, 0) == HaWsDirtyNone);

    // Once the lobby retry is accepted, the state can follow only while the
    // actual queue remains below the coalescing threshold.
    dirty = HaWsDirtyTracker{false, true};
    assert(haWsChooseDirtyRetry(dirty, 3) == HaWsDirtyState);
    assert(haWsChooseDirtyRetry(dirty, 4) == HaWsDirtyNone);
}

struct DirtyLifecycleModel {
    HaWsDirtyTracker dirty = {};
    std::string lobby;
    std::string state;
    std::vector<std::string> delivered;

    void cache(HaWsOutputClass outputClass, const std::string& payload) {
        HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
        if(choice == HaWsDirtyLobby) lobby = payload;
        else if(choice == HaWsDirtyState) state = payload;
        else assert(false);
        haWsDirtyMark(dirty, outputClass);
    }

    void retire(HaWsOutputClass outputClass) {
        HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
        haWsDirtyRetireSuperseded(dirty, outputClass);
        if(choice == HaWsDirtyLobby) lobby.clear();
        else if(choice == HaWsDirtyState) state.clear();
    }

    void directSendSucceeded(
        HaWsOutputClass outputClass,
        const std::string& payload) {
        delivered.push_back(payload);
        retire(outputClass);
    }

    bool retry(bool enqueueSucceeds, size_t queueDepth) {
        HaWsDirtyChoice choice = haWsChooseDirtyRetry(dirty, queueDepth);
        if(choice == HaWsDirtyNone) return false;
        HaWsOutputClass outputClass = haWsDirtyOutputClass(choice);
        if(!enqueueSucceeds) {
            // Matches firmware behavior: replaceable retry failures retain the
            // newest cached payload for another tick.
            assert(haWsChooseSendFailureAction(outputClass) ==
                   HaWsFailureCacheSnapshot);
            return false;
        }
        delivered.push_back(choice == HaWsDirtyLobby ? lobby : state);
        retire(outputClass);
        return true;
    }
};

static void testDirtyStateLifecycleNeverRegresses() {
    DirtyLifecycleModel model;

    // S1 is cached under pressure. Once newer S2 is directly enqueued, S1 must
    // be retired so a later flush cannot send it after S2.
    model.cache(HaWsOutputGameState, "S1");
    assert(model.dirty.state && model.state == "S1");
    model.directSendSucceeded(HaWsOutputGameState, "S2");
    assert(!model.dirty.state && model.state.empty());
    assert(!model.retry(true, 0));
    assert((model.delivered == std::vector<std::string>{"S2"}));

    // Repeated coalescing keeps only the newest state, and a failed retry does
    // not discard it. A later successful retry delivers S4 exactly once.
    model.cache(HaWsOutputGameState, "S3");
    model.cache(HaWsOutputGameStateStream, "S4");
    assert(model.state == "S4");
    assert(!model.retry(false, 0));
    assert(model.dirty.state && model.state == "S4");
    assert(model.retry(true, 0));
    assert(!model.dirty.state && model.state.empty());
    assert((model.delivered == std::vector<std::string>{"S2", "S4"}));

    // Lobby and game state are independent slots: direct lobby delivery cannot
    // erase a newer pending game snapshot.
    model.cache(HaWsOutputLobbyState, "L1");
    model.cache(HaWsOutputGameState, "S5");
    model.directSendSucceeded(HaWsOutputLobbyState, "L2");
    assert(!model.dirty.lobby && model.lobby.empty());
    assert(model.dirty.state && model.state == "S5");
    assert(model.retry(true, 0));
    assert((model.delivered ==
            std::vector<std::string>{"S2", "S4", "L2", "S5"}));
}

int main() {
    testClassification();
    testThresholdBoundaries();
    testSendFailureActions();
    testQueueTracking();
    testControlCountDrivesClose();
    testDirtyRetryDecisions();
    testDirtyStateLifecycleNeverRegresses();
    std::cout << "native outbound-flow-policy tests passed\n";
}
