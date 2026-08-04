// Typed protocol-v18 host-event formatting, kept independent of Arduino so every
// event kind and truncation boundary is covered by the native sanitizer suite.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ha_proto.h"

enum HaHostEventDisposition : uint8_t {
    HaHostEventIgnored = 0,
    HaHostEventLog = 1,
    HaHostEventStatus = 2,
};

static inline HaHostEventDisposition haFormatHostEvent(
    uint8_t kind,
    const char* gameName,
    const char* actorName,
    const char* targetName,
    int16_t value,
    const char* text,
    char* output,
    size_t capacity) {
    if(!output || !capacity) return HaHostEventIgnored;
    output[0] = '\0';
    const char* game = gameName ? gameName : "Arcade";
    const char* actor = actorName ? actorName : "?";
    const char* target = targetName ? targetName : "?";
    const char* detail = text ? text : "";
    HaHostEventDisposition disposition = HaHostEventStatus;
    switch(kind) {
    case HA_HOST_EVT_MATCH_STARTED:
        snprintf(output, capacity, "%s: %s vs %s", game, actor, target);
        break;
    case HA_HOST_EVT_CHAT:
        snprintf(output, capacity, "%s: %s", actor, detail);
        disposition = HaHostEventLog;
        break;
    case HA_HOST_EVT_ROLE:
        snprintf(output, capacity, "%s: %s %s", game, actor, detail);
        break;
    case HA_HOST_EVT_ROUND_WIN:
        if(detail[0])
            snprintf(output, capacity, "%s: %s beat %s (%s)", game, actor, target, detail);
        else
            snprintf(output, capacity, "%s: %s beat %s", game, actor, target);
        break;
    case HA_HOST_EVT_ROUND_DRAW:
        if(detail[0])
            snprintf(output, capacity, "%s: %s / %s draw (%s)", game, actor, target, detail);
        else
            snprintf(output, capacity, "%s: %s / %s draw", game, actor, target);
        break;
    case HA_HOST_EVT_ROUND_COMPLETE:
        snprintf(output, capacity, "%s: round %d complete", game, (int)value);
        break;
    case HA_HOST_EVT_GAME_FINAL:
        snprintf(output, capacity, "%s: game complete", game);
        break;
    default:
        return HaHostEventIgnored;
    }
    output[capacity - 1] = '\0';
    return disposition;
}

