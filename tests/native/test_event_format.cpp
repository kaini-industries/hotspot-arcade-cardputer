#include <cassert>
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
        sizeof(output));
    assert(disposition == expectedDisposition);
    assert(std::strcmp(output, expected) == 0);
}

static void expectGerman(uint8_t kind, const char* expected, int16_t value = 0) {
    char output[96];
    HaHostEventDisposition disposition = haFormatHostEvent(
        kind,
        "Schach",
        "NOVA",
        "ORBIT",
        value,
        "",
        output,
        sizeof(output),
        HaUiGerman);
    assert(disposition == HaHostEventStatus);
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

    expectGerman(HA_HOST_EVT_MATCH_STARTED, "Schach: NOVA gegen ORBIT");
    expectGerman(HA_HOST_EVT_ROUND_WIN, "Schach: NOVA besiegt ORBIT");
    expectGerman(HA_HOST_EVT_ROUND_DRAW, "Schach: NOVA / ORBIT unentschieden");
    expectGerman(HA_HOST_EVT_ROUND_COMPLETE, "Schach: Runde 7 beendet", 7);
    expectGerman(HA_HOST_EVT_GAME_FINAL, "Schach: Spiel beendet");

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
