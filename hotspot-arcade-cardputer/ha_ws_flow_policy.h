// Allocation-free outbound WebSocket flow-control policy.
//
// AsyncWebSocket exposes only the total text-message queue depth.  We keep a
// parallel fixed-size FIFO so the overload threshold can count application
// control messages without confusing them with replaceable state or streams.
// The sketch owns String storage and actual send/close operations.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HA_WS_COALESCE_DEPTH 4U
#define HA_WS_STREAM_DROP_DEPTH 8U
#define HA_WS_CONTROL_CLOSE_DEPTH 16U
#define HA_WS_TRACKED_QUEUE_CAPACITY 32U

enum HaWsOutputClass : uint8_t {
    HaWsOutputControl = 0,
    HaWsOutputLobbyState = 1,
    HaWsOutputGameState = 2,
    HaWsOutputStream = 3,
    HaWsOutputGameStateStream = 4,
};

enum HaWsOutputAction : uint8_t {
    HaWsOutputSend = 0,
    HaWsOutputCoalesce = 1,
    HaWsOutputDropStream = 2,
    HaWsOutputCloseControl = 3,
};

enum HaWsSendFailureAction : uint8_t {
    HaWsFailureCloseClient = 0,
    HaWsFailureCacheSnapshot = 1,
    HaWsFailureDropMessage = 2,
};

enum HaWsDirtyChoice : uint8_t {
    HaWsDirtyNone = 0,
    HaWsDirtyLobby = 1,
    HaWsDirtyState = 2,
};

struct HaWsDirtyTracker {
    bool lobby;
    bool state;
};

static inline bool haWsOutputIsControl(HaWsOutputClass outputClass) {
    return outputClass == HaWsOutputControl;
}

static inline bool haWsOutputIsReplaceable(HaWsOutputClass outputClass) {
    return outputClass == HaWsOutputLobbyState ||
           outputClass == HaWsOutputGameState ||
           outputClass == HaWsOutputGameStateStream;
}

static inline bool haWsOutputIsStream(HaWsOutputClass outputClass) {
    return outputClass == HaWsOutputStream ||
           outputClass == HaWsOutputGameStateStream;
}

// The engine uses {"t":"pong"} for its heartbeat response and includes a
// "phase" field in authoritative Pong game snapshots.  Keeping that signal
// explicit ensures a heartbeat can never replace the cached game snapshot.
static inline HaWsOutputClass haWsClassifyOutput(
    const char* type,
    bool pongHasPhase) {
    if(!type) return HaWsOutputControl;
    if(strcmp(type, "lobby") == 0) return HaWsOutputLobbyState;
    if(strcmp(type, "ink") == 0) return HaWsOutputStream;
    if(strcmp(type, "pong") == 0)
        return pongHasPhase ? HaWsOutputGameStateStream : HaWsOutputControl;

    static const char* const gameStates[] = {
        "trivia", "duel", "draw", "wyr", "scramble", "react", "gc",
        "bs", "spectrum", "kmk", "chess"
    };
    for(const char* candidate : gameStates)
        if(strcmp(type, candidate) == 0) return HaWsOutputGameState;
    return HaWsOutputControl;
}

static inline HaWsOutputAction haWsChooseOutputAction(
    HaWsOutputClass outputClass,
    size_t totalQueueDepth,
    uint8_t queuedControls) {
    if(haWsOutputIsStream(outputClass) &&
       totalQueueDepth >= HA_WS_STREAM_DROP_DEPTH)
        return HaWsOutputDropStream;
    if(haWsOutputIsReplaceable(outputClass) &&
       totalQueueDepth >= HA_WS_COALESCE_DEPTH)
        return HaWsOutputCoalesce;
    if(haWsOutputIsControl(outputClass) &&
       queuedControls >= HA_WS_CONTROL_CLOSE_DEPTH)
        return HaWsOutputCloseControl;
    return HaWsOutputSend;
}

static inline HaWsSendFailureAction haWsChooseSendFailureAction(
    HaWsOutputClass outputClass) {
    if(haWsOutputIsControl(outputClass)) return HaWsFailureCloseClient;
    if(haWsOutputIsReplaceable(outputClass)) return HaWsFailureCacheSnapshot;
    return HaWsFailureDropMessage;
}

static inline HaWsDirtyChoice haWsDirtyChoiceForOutput(
    HaWsOutputClass outputClass) {
    if(outputClass == HaWsOutputLobbyState) return HaWsDirtyLobby;
    if(outputClass == HaWsOutputGameState ||
       outputClass == HaWsOutputGameStateStream)
        return HaWsDirtyState;
    return HaWsDirtyNone;
}

static inline bool haWsDirtyAny(const HaWsDirtyTracker& dirty) {
    return dirty.lobby || dirty.state;
}

static inline bool haWsDirtyHas(
    const HaWsDirtyTracker& dirty,
    HaWsDirtyChoice choice) {
    if(choice == HaWsDirtyLobby) return dirty.lobby;
    if(choice == HaWsDirtyState) return dirty.state;
    return false;
}

static inline void haWsDirtyMark(
    HaWsDirtyTracker& dirty,
    HaWsOutputClass outputClass) {
    HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
    if(choice == HaWsDirtyLobby) dirty.lobby = true;
    else if(choice == HaWsDirtyState) dirty.state = true;
}

// Any directly enqueued replaceable snapshot is newer than a snapshot cached
// earlier in the serialized engine call stream. Retiring that same-class cache
// prevents a later dirty flush from regressing the client to stale state.
static inline void haWsDirtyRetireSuperseded(
    HaWsDirtyTracker& dirty,
    HaWsOutputClass outputClass) {
    HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
    if(choice == HaWsDirtyLobby) dirty.lobby = false;
    else if(choice == HaWsDirtyState) dirty.state = false;
}

static inline HaWsDirtyChoice haWsChooseDirtyRetry(
    const HaWsDirtyTracker& dirty,
    size_t totalQueueDepth) {
    if(totalQueueDepth >= HA_WS_COALESCE_DEPTH) return HaWsDirtyNone;
    if(dirty.lobby) return HaWsDirtyLobby;
    if(dirty.state) return HaWsDirtyState;
    return HaWsDirtyNone;
}

static inline HaWsOutputClass haWsDirtyOutputClass(HaWsDirtyChoice choice) {
    return choice == HaWsDirtyLobby ? HaWsOutputLobbyState : HaWsOutputGameState;
}

enum HaWsTrackedKind : uint8_t {
    HaWsTrackedOther = 0,
    HaWsTrackedControl = 1,
};

struct HaWsQueueTracker {
    uint8_t entries[HA_WS_TRACKED_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t count;
    uint8_t controlCount;
};

static inline void haWsQueuePopFront(HaWsQueueTracker& tracker) {
    if(!tracker.count) return;
    if(tracker.entries[tracker.head] == HaWsTrackedControl && tracker.controlCount)
        tracker.controlCount--;
    tracker.head = (uint8_t)((tracker.head + 1U) % HA_WS_TRACKED_QUEUE_CAPACITY);
    tracker.count--;
}

static inline bool haWsQueuePush(
    HaWsQueueTracker& tracker,
    HaWsTrackedKind kind) {
    if(tracker.count >= HA_WS_TRACKED_QUEUE_CAPACITY) return false;
    uint8_t tail = (uint8_t)((tracker.head + tracker.count) %
                             HA_WS_TRACKED_QUEUE_CAPACITY);
    tracker.entries[tail] = (uint8_t)kind;
    tracker.count++;
    if(kind == HaWsTrackedControl) tracker.controlCount++;
    return true;
}

// Reconcile the FIFO with AsyncWebSocket's current data-message queue.  All
// firmware text sends pass through this policy.  Unknown growth is nevertheless
// represented as non-control entries so an external/library enqueue cannot
// manufacture a control-overload close.
static inline void haWsQueueObserve(
    HaWsQueueTracker& tracker,
    size_t totalQueueDepth) {
    if(totalQueueDepth > HA_WS_TRACKED_QUEUE_CAPACITY)
        totalQueueDepth = HA_WS_TRACKED_QUEUE_CAPACITY;
    while(tracker.count > totalQueueDepth) haWsQueuePopFront(tracker);
    while(tracker.count < totalQueueDepth)
        if(!haWsQueuePush(tracker, HaWsTrackedOther)) break;
}

static inline bool haWsQueueRecord(
    HaWsQueueTracker& tracker,
    HaWsOutputClass outputClass) {
    return haWsQueuePush(
        tracker,
        haWsOutputIsControl(outputClass) ? HaWsTrackedControl : HaWsTrackedOther);
}
