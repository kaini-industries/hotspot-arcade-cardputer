#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iostream>

#define HA_MAX_PLAYERS 10
#include "ha_host.h"

uint8_t haLang = 0;

static void identityFor(uint32_t value, char out[HA_CLIENT_ID_LEN]) {
    int length = std::snprintf(
        out,
        HA_CLIENT_ID_LEN,
        "%08x%08x%08x%08x",
        value,
        value ^ 0x13579BDFU,
        value ^ 0x2468ACE0U,
        value ^ 0xA5A5A5A5U);
    assert(length == 32);
}

static void testStableReconnectAndSaturatingScores() {
    haHostReset();
    char identity[HA_CLIENT_ID_LEN];
    identityFor(1, identity);
    assert(haHostCanTrackIdentity(identity));
    assert(haHostJoinStable(1, identity, "ALICE", "A"));
    assert(haHostSessionPlayerCount() == 1);
    assert(haHostOnlineSessionPlayerCount() == 1);

    haHostScore(1, INT_MAX);
    haHostScore(1, 500);
    assert(haHost.p[1].score == INT32_MAX);
    haHostLeave(1);
    assert(haHostPlayerCount() == 0);
    assert(haHostOnlineSessionPlayerCount() == 0);

    assert(haHostJoinStable(2, identity, "ALICE 2", "B"));
    assert(haHostSessionPlayerCount() == 1);
    assert(haHost.p[2].score == INT32_MAX);
    assert(std::strcmp(haHost.session[0].nick, "ALICE 2") == 0);
    assert(!haHostJoinStable(2, identity, "ALICE 3", "C"));
    assert(haHostSessionPlayerCount() == 1);

    haHostScore(2, INT_MIN);
    haHostScore(2, INT_MIN);
    assert(haHost.p[2].score == INT32_MIN);
    assert(haHost.session[0].score == INT32_MIN);
}

static void testBoundedLedgerAdmission() {
    haHostReset();
    char identity[HA_CLIENT_ID_LEN];
    for(uint32_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++) {
        identityFor(i + 10, identity);
        assert(haHostCanTrackIdentity(identity));
        assert(haHostJoinStable(1, identity, "PLAYER", "P"));
        haHostLeave(1);
    }
    assert(haHostSessionPlayerCount() == HA_SESSION_MAX_PLAYERS);
    identityFor(10, identity);
    assert(haHostCanTrackIdentity(identity));
    identityFor(999, identity);
    assert(!haHostCanTrackIdentity(identity));
}

static void testImportAndSparsePlayCounts() {
    haHostReset();
    char first[HA_CLIENT_ID_LEN];
    char second[HA_CLIENT_ID_LEN];
    identityFor(41, first);
    identityFor(42, second);
    assert(haHostImportSessionPlayer(first, "NOVA", 123, "N"));
    assert(!haHostImportSessionPlayer(first, "DUPLICATE", 456, "D"));
    assert(!haHostImportSessionPlayer("not-a-digest", "BAD", 0, "B"));
    assert(haHostImportSessionPlayer(second, "ORBIT", -55, "O"));
    assert(haHostImportSessionGamePlay(HA_GAME_DRAW, UINT16_MAX));
    assert(haHostGamePlayed(HA_GAME_DRAW));
    assert(haHostGamePlayCount(haHost, HA_GAME_DRAW) == UINT16_MAX);
    assert(!haHostImportSessionGamePlay(250, 1));
    haHostImportSessionGame(250);
    assert(haHost.activeGame == HA_GAME_NONE);
    haHostImportSessionGame(HA_GAME_CHESS);
    assert(haHost.activeGame == HA_GAME_CHESS);
}

static void testEventRingCapacityAndLocalizedPresenceEvents() {
    static_assert(HA_EV_MAX == 24, "the Cardputer event console must retain 24 entries");
    haHostReset();
    haHost.evTotal = 0;
    for(unsigned i = 0; i < 30; i++) {
        char event[HA_EV_LEN];
        std::snprintf(event, sizeof(event), "EVENT %02u", i);
        haHostLog(event);
    }
    assert(haHost.evTotal == 30);
    for(unsigned i = 6; i < 30; i++) {
        char expected[HA_EV_LEN];
        std::snprintf(expected, sizeof(expected), "EVENT %02u", i);
        assert(std::strcmp(haHost.ev[i % HA_EV_MAX], expected) == 0);
    }

    for(size_t i = 0; i < HA_GENERATED_LANGUAGE_COUNT; i++)
        if(std::strcmp(HA_GENERATED_LANGUAGES[i].code, "de") == 0) haLang = (uint8_t)i;
    char identity[HA_CLIENT_ID_LEN];
    identityFor(77, identity);
    assert(haHostJoinStable(1, identity, "NOVA", "N"));
    assert(std::strcmp(haHost.ev[(haHost.evTotal - 1) % HA_EV_MAX], "DA NOVA") == 0);
    haHostLeave(1);
    assert(std::strcmp(haHost.ev[(haHost.evTotal - 1) % HA_EV_MAX], "WEG NOVA") == 0);
    haLang = 0;
}

int main() {
    testStableReconnectAndSaturatingScores();
    testBoundedLedgerAdmission();
    testImportAndSparsePlayCounts();
    testEventRingCapacityAndLocalizedPresenceEvents();
    std::cout << "native cumulative-ledger tests passed\n";
}
