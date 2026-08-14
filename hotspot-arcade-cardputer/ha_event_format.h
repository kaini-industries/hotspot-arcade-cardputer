// Typed protocol-v18 host-event formatting, kept independent of Arduino so every
// event kind and truncation boundary is covered by the native sanitizer suite.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ha_proto.h"
#include "ha_ui_text.h"

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
    size_t capacity,
    HaUiLocale locale = HaUiEnglish) {
    if(!output || !capacity) return HaHostEventIgnored;
    output[0] = '\0';
    const char* game = gameName ? gameName : "Arcade";
    const char* actor = actorName ? actorName : "?";
    const char* target = targetName ? targetName : "?";
    const char* detail = text ? text : "";
    HaHostEventDisposition disposition = HaHostEventStatus;
    switch(kind) {
    case HA_HOST_EVT_MATCH_STARTED:
        snprintf(
            output,
            capacity,
            haUiTextForLocale(HaUiTextMatchStartedFormat, locale),
            game,
            actor,
            target);
        break;
    case HA_HOST_EVT_CHAT:
        snprintf(output, capacity, "%s: %s", actor, detail);
        disposition = HaHostEventLog;
        break;
    case HA_HOST_EVT_ROLE:
        snprintf(
            output,
            capacity,
            haUiTextForLocale(HaUiTextRoleFormat, locale),
            game,
            actor,
            detail);
        break;
    case HA_HOST_EVT_ROUND_WIN:
        if(detail[0])
            snprintf(
                output,
                capacity,
                haUiTextForLocale(HaUiTextRoundWinDetailFormat, locale),
                game,
                actor,
                target,
                detail);
        else
            snprintf(
                output,
                capacity,
                haUiTextForLocale(HaUiTextRoundWinFormat, locale),
                game,
                actor,
                target);
        break;
    case HA_HOST_EVT_ROUND_DRAW:
        if(detail[0])
            snprintf(
                output,
                capacity,
                haUiTextForLocale(HaUiTextRoundDrawDetailFormat, locale),
                game,
                actor,
                target,
                detail);
        else
            snprintf(
                output,
                capacity,
                haUiTextForLocale(HaUiTextRoundDrawFormat, locale),
                game,
                actor,
                target);
        break;
    case HA_HOST_EVT_ROUND_COMPLETE:
        snprintf(
            output,
            capacity,
            haUiTextForLocale(HaUiTextRoundCompleteFormat, locale),
            game,
            (int)value);
        break;
    case HA_HOST_EVT_GAME_FINAL:
        snprintf(
            output,
            capacity,
            haUiTextForLocale(HaUiTextGameCompleteFormat, locale),
            game);
        break;
    default:
        return HaHostEventIgnored;
    }
    output[capacity - 1] = '\0';
    return disposition;
}
