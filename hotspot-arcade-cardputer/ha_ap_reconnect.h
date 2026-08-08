// Pure planned-AP-restart policy. The reconnect barrier is an exact set of
// durable identity digests captured when transport pauses; a head count is not
// sufficient because a different known participant must not stand in for a
// player whose game state is suspended.
#pragma once

#include <stdint.h>
#include <string.h>
#include "ha_host.h"

struct HaApRequiredRoster {
    char identity[HA_MAX_PLAYERS][HA_CLIENT_ID_LEN];
    uint8_t count;
    uint8_t unidentified;
};

enum HaApReconnectDecision : uint8_t {
    HaApReconnectDecisionWait = 0,
    HaApReconnectDecisionAllRequiredOnline = 1,
    HaApReconnectDecisionWindowExpired = 2,
};

static inline void haApRequiredClear(HaApRequiredRoster& required) {
    required = HaApRequiredRoster{};
}

static inline bool haApRequiredContains(
    const HaApRequiredRoster& required,
    const char* identity) {
    if(!haHostClientIdStable(identity)) return false;
    for(uint8_t i = 0; i < required.count; i++)
        if(strcmp(required.identity[i], identity) == 0) return true;
    return false;
}

// Capture only currently connected engine participants. Ledger-only identities
// that were already offline when the host paused are not required to resume the
// suspended round. An unidentifiable legacy participant prevents automatic
// readiness; the host can still resume early or let the window expire.
static inline void haApRequiredCapture(
    HaApRequiredRoster& required,
    const HaHost& host) {
    haApRequiredClear(required);
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
        const HaHostPlayer& live = host.p[pid];
        if(!live.used || live.sessionIndex >= HA_SESSION_MAX_PLAYERS) {
            if(live.used && required.unidentified != UINT8_MAX)
                required.unidentified++;
            continue;
        }
        const HaHostSessionPlayer& session = host.session[live.sessionIndex];
        // An engine seat may already be inside ordinary disconnect grace when
        // planned downtime begins. It was not online at the checkpoint and is
        // therefore not part of this restart's required reconnect barrier.
        if(session.used && !session.connected) continue;
        if(!session.used || session.pid != pid ||
           !haHostClientIdStable(session.clientId)) {
            if(required.unidentified != UINT8_MAX) required.unidentified++;
            continue;
        }
        if(haApRequiredContains(required, session.clientId)) continue;
        if(required.count >= HA_MAX_PLAYERS) {
            if(required.unidentified != UINT8_MAX) required.unidentified++;
            continue;
        }
        strlcpy(
            required.identity[required.count++],
            session.clientId,
            HA_CLIENT_ID_LEN);
    }
}

static inline uint8_t haApRequiredMissing(
    const HaApRequiredRoster& required,
    const HaHost& host) {
    uint16_t missing = required.unidentified;
    for(uint8_t i = 0; i < required.count; i++) {
        bool online = false;
        for(uint8_t session = 0; session < HA_SESSION_MAX_PLAYERS; session++) {
            if(host.session[session].used &&
               strcmp(host.session[session].clientId, required.identity[i]) == 0) {
                if(host.session[session].connected) online = true;
                break; // ledger validation forbids duplicate durable identities
            }
        }
        if(!online) missing++;
    }
    return missing > UINT8_MAX ? UINT8_MAX : (uint8_t)missing;
}

static inline HaApReconnectDecision haApReconnectEvaluate(
    const HaApRequiredRoster& required,
    const HaHost& host,
    uint32_t now,
    uint32_t startedAt,
    uint32_t windowMs) {
    if(haApRequiredMissing(required, host) == 0)
        return HaApReconnectDecisionAllRequiredOnline;
    if((uint32_t)(now - startedAt) >= windowMs)
        return HaApReconnectDecisionWindowExpired;
    return HaApReconnectDecisionWait;
}
