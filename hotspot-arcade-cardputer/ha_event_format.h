// Typed protocol-v18 host-event formatting, kept independent of Arduino so every
// event kind and truncation boundary is covered by the native sanitizer suite.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ha_proto.h"
#include "ha_ui_text.h"
#include "ha_utf8.h"

enum HaHostEventDisposition : uint8_t {
    HaHostEventIgnored = 0,
    HaHostEventLog = 1,
    HaHostEventStatus = 2,
};

struct HaHostEventDetailTranslation {
    const char* token;
    const char* de;
};

static inline const char* haLocalizeHostEventDetail(
    uint8_t kind,
    uint8_t game,
    const char* detail,
    HaUiLocale locale) {
    const char* safe = detail ? detail : "";
    if(locale != HaUiGerman || !safe[0]) return safe;

    // Only tokens emitted by a known engine/game pair are translated. Spectrum
    // clues, chat, and all other arbitrary user/content text remain byte-for-byte
    // unchanged even if they happen to equal one of these reserved words.
    if(kind == HA_HOST_EVT_ROLE && game == HA_GAME_DRAW && strcmp(safe, "drawer") == 0)
        return "zeichnet";
    if(kind == HA_HOST_EVT_ROLE && game == HA_GAME_KMK && strcmp(safe, "chooser") == 0)
        return "waehlt";
    if(kind == HA_HOST_EVT_ROUND_WIN && game == HA_GAME_DRAW && strcmp(safe, "guessed") == 0)
        return "erraten";
    if(kind == HA_HOST_EVT_ROUND_WIN && game == HA_GAME_FILLBLANK &&
       strcmp(safe, "picked") == 0)
        return "ausgewaehlt";
    if(kind == HA_HOST_EVT_ROLE && game == HA_GAME_SPYFALL &&
       strcmp(safe, "missed accusation") == 0)
        return "falsche Beschuldigung";

    if((kind == HA_HOST_EVT_ROUND_WIN || kind == HA_HOST_EVT_ROUND_DRAW) &&
       game == HA_GAME_CHESS) {
        static constexpr HaHostEventDetailTranslation CHESS_DETAILS[] = {
            {"mate", "Matt"},
            {"stalemate", "Patt"},
            {"resign", "Aufgabe"},
            {"flag", "Zeit"},
            {"flagdraw", "Zeitremis"},
            {"material", "Material"},
            {"rep3", "3x Wiederholung"},
            {"rep5", "5x Wiederholung"},
            {"move50", "50-Zuege-Regel"},
            {"move75", "75-Zuege-Regel"},
            {"agree", "Einigung"},
            {"left", "verlassen"},
        };
        for(size_t i = 0; i < sizeof(CHESS_DETAILS) / sizeof(CHESS_DETAILS[0]); i++)
            if(strcmp(safe, CHESS_DETAILS[i].token) == 0) return CHESS_DETAILS[i].de;
    }
    return safe;
}

static inline HaHostEventDisposition haFormatHostEvent(
    uint8_t kind,
    const char* gameName,
    const char* actorName,
    const char* targetName,
    int16_t value,
    const char* text,
    char* output,
    size_t capacity,
    HaUiLocale locale = HaUiEnglish,
    uint8_t gameId = HA_GAME_NONE) {
    if(!output || !capacity) return HaHostEventIgnored;
    output[0] = '\0';
    const char* game = gameName ? gameName : "Arcade";
    const char* actor = actorName ? actorName : "?";
    const char* target = targetName ? targetName : "?";
    const char* detail = haLocalizeHostEventDetail(kind, gameId, text, locale);
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
    haUtf8SafeTerminate(output, capacity);
    return disposition;
}
