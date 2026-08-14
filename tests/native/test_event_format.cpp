#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "ha_event_format.h"

static void expect(
    uint8_t kind,
    const char* expected,
    HaHostEventDisposition expectedDisposition,
    int16_t value = 0,
    const char* detail = "") {
    char output[96];
    HaHostEventDisposition disposition = haFormatHostEvent(
        kind,
        "Chess",
        "NOVA",
        "ORBIT",
        value,
        detail,
        output,
        sizeof(output),
        HaUiEnglish,
        HA_GAME_CHESS);
    assert(disposition == expectedDisposition);
    assert(std::strcmp(output, expected) == 0);
}

static void expectGerman(
    uint8_t kind,
    uint8_t game,
    const char* gameName,
    const char* detail,
    const char* expected,
    HaHostEventDisposition expectedDisposition = HaHostEventStatus,
    int16_t value = 0) {
    char output[96];
    HaHostEventDisposition disposition = haFormatHostEvent(
        kind,
        gameName,
        "NOVA",
        "ORBIT",
        value,
        detail,
        output,
        sizeof(output),
        HaUiGerman,
        game);
    assert(disposition == expectedDisposition);
    assert(std::strcmp(output, expected) == 0);
}

int main() {
    expect(HA_HOST_EVT_MATCH_STARTED, "Chess: NOVA vs ORBIT", HaHostEventStatus);
    expect(HA_HOST_EVT_CHAT, "NOVA: hello", HaHostEventLog, 0, "hello");
    expect(HA_HOST_EVT_ROLE, "Chess: NOVA drawer", HaHostEventStatus, 0, "drawer");
    expect(HA_HOST_EVT_ROUND_WIN, "Chess: NOVA beat ORBIT", HaHostEventStatus);
    expect(HA_HOST_EVT_ROUND_WIN, "Chess: NOVA beat ORBIT (mate)", HaHostEventStatus, 0, "mate");
    expect(HA_HOST_EVT_ROUND_DRAW, "Chess: NOVA / ORBIT draw", HaHostEventStatus);
    expect(HA_HOST_EVT_ROUND_DRAW, "Chess: NOVA / ORBIT draw (stalemate)", HaHostEventStatus, 0, "stalemate");
    expect(HA_HOST_EVT_ROUND_COMPLETE, "Chess: round 7 complete", HaHostEventStatus, 7);
    expect(HA_HOST_EVT_GAME_FINAL, "Chess: game complete", HaHostEventStatus);

    expectGerman(
        HA_HOST_EVT_MATCH_STARTED,
        HA_GAME_CHESS,
        "Schach",
        "",
        "Schach: NOVA gegen ORBIT");
    expectGerman(
        HA_HOST_EVT_ROUND_WIN,
        HA_GAME_CHESS,
        "Schach",
        "",
        "Schach: NOVA besiegt ORBIT");
    expectGerman(
        HA_HOST_EVT_ROUND_DRAW,
        HA_GAME_CHESS,
        "Schach",
        "",
        "Schach: NOVA / ORBIT unentschieden");
    expectGerman(
        HA_HOST_EVT_ROUND_COMPLETE,
        HA_GAME_CHESS,
        "Schach",
        "",
        "Schach: Runde 7 beendet",
        HaHostEventStatus,
        7);
    expectGerman(
        HA_HOST_EVT_GAME_FINAL,
        HA_GAME_CHESS,
        "Schach",
        "",
        "Schach: Spiel beendet");

    expectGerman(
        HA_HOST_EVT_ROLE,
        HA_GAME_DRAW,
        "Malen",
        "drawer",
        "Malen: NOVA zeichnet");
    expectGerman(
        HA_HOST_EVT_ROLE,
        HA_GAME_KMK,
        "Kiss Marry Kill",
        "chooser",
        "Kiss Marry Kill: NOVA waehlt");
    expectGerman(
        HA_HOST_EVT_ROUND_WIN,
        HA_GAME_DRAW,
        "Malen",
        "guessed",
        "Malen: NOVA besiegt ORBIT (erraten)");

    struct ChessReasonCase {
        uint8_t kind;
        const char* token;
        const char* de;
    };
    static constexpr ChessReasonCase CHESS_REASONS[] = {
        {HA_HOST_EVT_ROUND_WIN, "mate", "Matt"},
        {HA_HOST_EVT_ROUND_DRAW, "stalemate", "Patt"},
        {HA_HOST_EVT_ROUND_WIN, "resign", "Aufgabe"},
        {HA_HOST_EVT_ROUND_WIN, "flag", "Zeit"},
        {HA_HOST_EVT_ROUND_DRAW, "flagdraw", "Zeitremis"},
        {HA_HOST_EVT_ROUND_DRAW, "material", "Material"},
        {HA_HOST_EVT_ROUND_DRAW, "rep3", "3x Wiederholung"},
        {HA_HOST_EVT_ROUND_DRAW, "rep5", "5x Wiederholung"},
        {HA_HOST_EVT_ROUND_DRAW, "move50", "50-Zuege-Regel"},
        {HA_HOST_EVT_ROUND_DRAW, "move75", "75-Zuege-Regel"},
        {HA_HOST_EVT_ROUND_DRAW, "agree", "Einigung"},
        {HA_HOST_EVT_ROUND_WIN, "left", "verlassen"},
    };
    for(const ChessReasonCase& reason : CHESS_REASONS) {
        char expected[96];
        if(reason.kind == HA_HOST_EVT_ROUND_WIN)
            std::snprintf(
                expected,
                sizeof(expected),
                "Schach: NOVA besiegt ORBIT (%s)",
                reason.de);
        else
            std::snprintf(
                expected,
                sizeof(expected),
                "Schach: NOVA / ORBIT unentschieden (%s)",
                reason.de);
        expectGerman(
            reason.kind,
            HA_GAME_CHESS,
            "Schach",
            reason.token,
            expected);
    }

    // User/content text is never translated based on spelling alone.
    expectGerman(
        HA_HOST_EVT_ROLE,
        HA_GAME_SPECTRUM,
        "Spektrum",
        "drawer",
        "Spektrum: NOVA drawer");
    expectGerman(
        HA_HOST_EVT_CHAT,
        HA_GAME_DRAW,
        "Malen",
        "mate",
        "NOVA: mate",
        HaHostEventLog);
    expectGerman(
        HA_HOST_EVT_ROUND_WIN,
        HA_GAME_CHESS,
        "Schach",
        "custom result",
        "Schach: NOVA besiegt ORBIT (custom result)");

    char tiny[8];
    assert(haFormatHostEvent(
               HA_HOST_EVT_CHAT,
               "Game",
               "PLAYER",
               "?",
               0,
               "a deliberately long message",
               tiny,
               sizeof(tiny)) == HaHostEventLog);
    assert(tiny[sizeof(tiny) - 1] == '\0');
    tiny[0] = 'x';
    assert(haFormatHostEvent(255, "G", "A", "B", 0, "", tiny, sizeof(tiny)) ==
           HaHostEventIgnored);
    assert(tiny[0] == '\0');
    std::cout << "native typed-event formatter tests passed\n";
}
