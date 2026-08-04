#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "ha_active_nvs.h"

struct FakeSessionPlayer {
    bool used;
    char clientId[HA_CLIENT_ID_LEN];
    char avatar[HA_AVATAR_LEN];
    char nick[HA_NICK_LEN];
    int32_t score;
    char rawResumeToken[40]; // capture must never persist this field
};

struct FakeGameCount {
    uint8_t game;
    uint16_t count;
};

struct FakeHost {
    FakeSessionPlayer session[HA_ACTIVE_NVS_MAX_PLAYERS];
    FakeGameCount games[HA_ACTIVE_NVS_MAX_GAME_COUNTS];
    uint8_t gameCount;
    uint8_t activeGame;
    char lastEvent[48]; // capture must never persist this field
};

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

static FakeHost fullHost() {
    FakeHost host = {};
    host.activeGame = 15;
    host.gameCount = 3;
    host.games[0] = FakeGameCount{1, 5};
    host.games[1] = FakeGameCount{6, 2};
    host.games[2] = FakeGameCount{15, 1};
    std::strcpy(host.lastEvent, "event text that is not session state");
    for(uint8_t i = 0; i < HA_ACTIVE_NVS_MAX_PLAYERS; i++) {
        FakeSessionPlayer& player = host.session[i];
        player.used = true;
        identityFor((uint32_t)i + 1, player.clientId);
        std::snprintf(player.nick, sizeof(player.nick), "PLAYER-%u", (unsigned)i);
        std::strcpy(player.avatar, i & 1 ? "A" : "B");
        player.score = (int32_t)i * 125 - 500;
        std::strcpy(player.rawResumeToken, "raw-resume-secret-must-not-persist");
    }
    return host;
}

static HaActiveNvsRecord capture(const FakeHost& host) {
    HaActiveNvsRecord record = {};
    assert(haActiveNvsCaptureHost(host, 42, 7, record));
    return record;
}

static void resetAll() {
    Preferences::reset();
    haActiveNvsResetCheckpointRateLimit();
}

static bool nvsContains(const std::string& needle) {
    const auto namespaceIt = Preferences::storage.find(HA_ACTIVE_NVS_NAMESPACE);
    if(namespaceIt == Preferences::storage.end()) return false;
    for(const auto& item : namespaceIt->second) {
        std::string bytes((const char*)item.second.data(), item.second.size());
        if(bytes.find(needle) != std::string::npos) return true;
    }
    return false;
}

static void testCaptureAndBoundedRoundTrip() {
    resetAll();
    FakeHost host = fullHost();
    HaActiveNvsRecord record = capture(host);
    assert(record.generation == 0);
    assert(record.sessionNumber == 42);
    assert(record.restoredFrom == 7);
    assert(record.activeGame == 15);
    assert(record.participantCount == 32);
    assert(record.gameCount == 3);
    assert(HA_ACTIVE_NVS_CHUNK_COUNT > 1);
    assert(sizeof(HaActiveNvsEnvelope) <= 4096);

    assert(haActiveNvsCheckpointNoSd(record, 100) == HaActiveNvsCheckpointWritten);
    HaActiveNvsRecord loaded = {};
    assert(haActiveNvsRead(loaded));
    assert(loaded.generation == 1);
    assert(loaded.participantCount == 32);
    assert(std::strcmp(loaded.participants[31].name, "PLAYER-31") == 0);
    assert(loaded.participants[31].cumulativeScore == 3375);
    assert(loaded.games[1].game == 6 && loaded.games[1].count == 2);

    assert(!nvsContains("raw-resume-secret-must-not-persist"));
    assert(!nvsContains("event text that is not session state"));
}

static void testPayloadValidationAndCanonicalization() {
    resetAll();
    FakeHost host = fullHost();
    HaActiveNvsRecord valid = capture(host);

    HaActiveNvsRecord bad = valid;
    bad.sessionNumber = 0;
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.participants[0].identity[0] = 'A';
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    std::strcpy(bad.participants[1].identity, bad.participants[0].identity);
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.participants[0].name[0] = (char)0xC0;
    bad.participants[0].name[1] = (char)0x80;
    bad.participants[0].name[2] = '\0';
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.games[1].game = bad.games[0].game;
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.games[0].count = 0;
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.activeGame = 250;
    assert(!haActiveNvsPayloadValid(bad));
    bad = valid;
    bad.games[0].game = 250;
    assert(!haActiveNvsPayloadValid(bad));

    // Unused caller bytes are not serialized: write canonicalization zeros them.
    valid.participantCount = 1;
    valid.gameCount = 1;
    std::strcpy(valid.participants[1].name, "HIDDEN-UNUSED-DATA");
    valid.generation = 1;
    assert(haActiveNvsWrite(valid));
    Preferences preferences;
    assert(preferences.begin(HA_ACTIVE_NVS_NAMESPACE, true));
    HaActiveNvsEnvelope envelope = {};
    assert(haActiveNvsReadSlot(preferences, 0, envelope));
    preferences.end();
    assert(envelope.record.participants[1].name[0] == '\0');
    assert(!nvsContains("HIDDEN-UNUSED-DATA"));
}

static void testEnvelopeVersionCrcAndGenerationValidation() {
    HaActiveNvsRecord record = capture(fullHost());
    record.generation = 9;
    HaActiveNvsEnvelope envelope = {};
    envelope.magic = HA_ACTIVE_NVS_MAGIC;
    envelope.schema = HA_ACTIVE_NVS_SCHEMA;
    envelope.bytes = sizeof(envelope);
    assert(haActiveNvsCanonicalize(record, envelope.record));
    envelope.crc = haActiveNvsEnvelopeCrc(envelope);
    assert(haActiveNvsEnvelopeValid(envelope));

    HaActiveNvsEnvelope bad = envelope;
    bad.schema++;
    bad.crc = haActiveNvsEnvelopeCrc(bad);
    assert(!haActiveNvsEnvelopeValid(bad));
    bad = envelope;
    bad.bytes--;
    bad.crc = haActiveNvsEnvelopeCrc(bad);
    assert(!haActiveNvsEnvelopeValid(bad));
    bad = envelope;
    bad.record.generation = 0;
    bad.crc = haActiveNvsEnvelopeCrc(bad);
    assert(!haActiveNvsEnvelopeValid(bad));
    bad = envelope;
    bad.record.participants[0].name[0] ^= 1;
    assert(!haActiveNvsEnvelopeValid(bad)); // stale CRC
}

static void testAlternatingGenerationAndCorruptionRecovery() {
    resetAll();
    HaActiveNvsRecord first = capture(fullHost());
    first.generation = 1;
    assert(haActiveNvsWrite(first));

    HaActiveNvsRecord second = first;
    second.generation = 2;
    second.participants[0].cumulativeScore = 9999;
    assert(haActiveNvsWrite(second));
    HaActiveNvsRecord loaded = {};
    int8_t slot = -1;
    assert(haActiveNvsRead(loaded, &slot));
    assert(slot == 1 && loaded.generation == 2);
    assert(loaded.participants[0].cumulativeScore == 9999);
    assert(!haActiveNvsWrite(first)); // stale/equal generations cannot overwrite newest

    auto& newestTail = Preferences::storage[HA_ACTIVE_NVS_NAMESPACE]["b1"];
    assert(!newestTail.empty());
    newestTail[0] ^= 0x80;
    assert(haActiveNvsRead(loaded, &slot));
    assert(slot == 0 && loaded.generation == 1);
    assert(loaded.participants[0].cumulativeScore != 9999);
}

static void testEveryInterruptedChunkWriteKeepsOldGeneration() {
    resetAll();
    HaActiveNvsRecord first = capture(fullHost());
    first.generation = 1;
    assert(haActiveNvsWrite(first));
    const auto baseline = Preferences::storage;

    HaActiveNvsRecord second = first;
    second.generation = 2;
    second.participants[0].cumulativeScore = 123456;
    for(size_t completed = 0; completed < HA_ACTIVE_NVS_CHUNK_COUNT; completed++) {
        Preferences::storage = baseline;
        Preferences::writesRemaining = (int)completed;
        assert(!haActiveNvsWrite(second));
        Preferences::writesRemaining = -1;
        HaActiveNvsRecord loaded = {};
        assert(haActiveNvsRead(loaded));
        assert(loaded.generation == 1);
    }
    Preferences::storage = baseline;
    Preferences::writesRemaining = (int)HA_ACTIVE_NVS_CHUNK_COUNT;
    assert(haActiveNvsWrite(second));
    Preferences::writesRemaining = -1;
    HaActiveNvsRecord loaded = {};
    assert(haActiveNvsRead(loaded));
    assert(loaded.generation == 2);
}

static void testSdNvsArbitration() {
    assert(haActiveNvsChooseSource(false, 0, false, 0) == HaActiveNvsSourceNone);
    assert(haActiveNvsChooseSource(true, 7, false, 0) == HaActiveNvsSourceSd);
    assert(haActiveNvsChooseSource(false, 0, true, 8) == HaActiveNvsSourceNvs);
    assert(haActiveNvsChooseSource(true, 9, true, 8) == HaActiveNvsSourceSd);
    assert(haActiveNvsChooseSource(true, 7, true, 8) == HaActiveNvsSourceNvs);
    assert(haActiveNvsChooseSource(true, 8, true, 8) == HaActiveNvsSourceSd);
    assert(haActiveNvsChooseSource(true, 0, true, 8) == HaActiveNvsSourceNvs);
}

static void testCheckpointRateLimitForceFailureAndRollover() {
    resetAll();
    HaActiveNvsRecord snapshot = capture(fullHost());
    assert(haActiveNvsCheckpointNoSd(snapshot, 100) == HaActiveNvsCheckpointWritten);
    snapshot.participants[0].cumulativeScore++;
    assert(haActiveNvsCheckpointNoSd(snapshot, 30099) == HaActiveNvsCheckpointDeferred);
    HaActiveNvsRecord loaded = {};
    assert(haActiveNvsRead(loaded) && loaded.generation == 1);
    assert(haActiveNvsCheckpointNoSd(snapshot, 30100) == HaActiveNvsCheckpointWritten);
    assert(haActiveNvsRead(loaded) && loaded.generation == 2);

    snapshot.participants[0].cumulativeScore++;
    assert(haActiveNvsCheckpointNoSd(snapshot, 30100, true) == HaActiveNvsCheckpointWritten);
    assert(haActiveNvsRead(loaded) && loaded.generation == 3);

    // A failed forced write does not consume the rate window.
    haActiveNvsResetCheckpointRateLimit();
    Preferences::failWrites = true;
    assert(haActiveNvsCheckpointNoSd(snapshot, 50000, true) == HaActiveNvsCheckpointFailed);
    Preferences::failWrites = false;
    assert(haActiveNvsCheckpointNoSd(snapshot, 50000) == HaActiveNvsCheckpointWritten);
    assert(haActiveNvsRead(loaded) && loaded.generation == 4);

    // Unsigned elapsed time keeps the 30-second gate correct across millis rollover.
    haActiveNvsResetCheckpointRateLimit();
    const uint32_t beforeWrap = UINT32_MAX - 10000;
    assert(haActiveNvsCheckpointNoSd(snapshot, beforeWrap, true) == HaActiveNvsCheckpointWritten);
    assert(haActiveNvsCheckpointNoSd(snapshot, 19998) == HaActiveNvsCheckpointDeferred);
    assert(haActiveNvsCheckpointNoSd(snapshot, 19999) == HaActiveNvsCheckpointWritten);

    assert(haActiveNvsErase());
    assert(!haActiveNvsRead(loaded));
    assert(haActiveNvsCheckpointDue(0));
}

int main() {
    testCaptureAndBoundedRoundTrip();
    testPayloadValidationAndCanonicalization();
    testEnvelopeVersionCrcAndGenerationValidation();
    testAlternatingGenerationAndCorruptionRecovery();
    testEveryInterruptedChunkWriteKeepsOldGeneration();
    testSdNvsArbitration();
    testCheckpointRateLimitForceFailureAndRollover();
    std::cout << "native active NVS tests passed (" << sizeof(HaActiveNvsEnvelope)
              << " bytes, " << HA_ACTIVE_NVS_CHUNK_COUNT << " chunks/slot)\n";
}
