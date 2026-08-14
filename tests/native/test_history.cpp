#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

bool haSdOk = true;
#include "ha_history.h"

static void resetHistoryRuntime(bool resetMedia = true) {
    delete haHistRt.active;
    delete haHistScratch;
    delete haHistVerify;
    delete haHistStorage;
    haHistRt = HaHistRuntime{false, false, -1, nullptr};
    haHistScratch = nullptr;
    haHistVerify = nullptr;
    haHistStorage = nullptr;
    haHistRestoreHandler = nullptr;
    haSdOk = true;
    if(resetMedia) {
        SD.reset();
        Preferences::reset();
    }
}

static void identityFor(uint32_t value, char out[HA_CLIENT_ID_LEN]) {
    int written = std::snprintf(
        out,
        HA_CLIENT_ID_LEN,
        "%08x%08x%08x%08x",
        value,
        value ^ 0x13579BDFU,
        value ^ 0x2468ACE0U,
        value ^ 0xA5A5A5A5U);
    assert(written == 32);
}

static HaHost hostWithPlayers(int32_t firstScore = 10, int32_t secondScore = 5) {
    HaHost host = {};
    for(uint8_t pid = 0; pid <= HA_MAX_PLAYERS; pid++)
        host.p[pid].sessionIndex = HA_SESSION_INDEX_NONE;
    host.activeGame = HA_GAME_TRIVIA;
    host.sessionCount = 2;
    host.session[0].used = true;
    identityFor(1, host.session[0].clientId);
    std::strcpy(host.session[0].nick, "ALICE");
    std::strcpy(host.session[0].avatar, "A");
    host.session[0].score = firstScore;
    host.session[1].used = true;
    identityFor(2, host.session[1].clientId);
    std::strcpy(host.session[1].nick, "BOB");
    std::strcpy(host.session[1].avatar, "B");
    host.session[1].score = secondScore;
    host.gameCount = 1;
    host.games[0] = HaHostGamePlay{HA_GAME_TRIVIA, 1};
    return host;
}

static std::string archivePath(uint32_t number) {
    char path[64];
    haHistArchivePath(number, path, sizeof(path));
    return path;
}

static bool acceptRestore(const HaHistSession& session) {
    return session.archived && session.num != 0;
}

static void testKnownGameValidation() {
    static constexpr uint8_t KNOWN_GAMES[] = {
        HA_GAME_TRIVIA,
        HA_GAME_CONNECT4,
        HA_GAME_TICTACTOE,
        HA_GAME_DOTS,
        HA_GAME_DRAW,
        HA_GAME_PONG,
        HA_GAME_REACT,
        HA_GAME_WYR,
        HA_GAME_SCRAMBLE,
        HA_GAME_REVERSI,
        HA_GAME_GUESSCOLOR,
        HA_GAME_BATTLESHIP,
        HA_GAME_SPECTRUM,
        HA_GAME_KMK,
        HA_GAME_CHESS,
        HA_GAME_SECRETS,
        HA_GAME_FILLBLANK,
        HA_GAME_WEREWOLF,
        HA_GAME_SPYFALL,
        HA_GAME_FRANKENDRAW,
    };
    static_assert(sizeof(KNOWN_GAMES) == 20, "history test covers all v22 games");
    assert(HA_GENERATED_GAME_COUNT == 21); // twenty games plus the lobby sentinel
    assert(haHistKnownGame(HA_GAME_NONE, true));
    assert(!haHistKnownGame(HA_GAME_NONE, false));
    for(uint8_t game : KNOWN_GAMES) {
        assert(haHistKnownGame(game, false));
        HaHost host = hostWithPlayers();
        host.activeGame = game;
        host.games[0].game = game;
        HaHistSession record = {};
        haHistFromHost(host, record);
        record.num = 1;
        record.seq = 1;
        assert(haHistRecordValid(record));
    }
    assert(!haHistKnownGame(250, true));
}

static void testStrictRecordsAndInterruptedActiveWrites() {
    resetHistoryRuntime();
    assert(haHistBegin());
    assert(SD.exists(HA_HIST_INDEX));
    assert(haHistIndexValidate(HA_HIST_INDEX));

    HaHost goodHost = hostWithPlayers();
    assert(haHistCheckpoint(goodHost, true));
    const char* activePath = haHistRt.activeSlot == 0 ? HA_HIST_ACTIVE_A : HA_HIST_ACTIVE_B;
    HaHistSession valid = {};
    assert(haHistReadRecord(activePath, valid, 0));

    HaHistSession invalid = valid;
    invalid.p[0].nick[0] = (char)0xC0;
    invalid.p[0].nick[1] = (char)0x80;
    invalid.p[0].nick[2] = '\0';
    assert(!haHistWriteRecord("/hotspot-arcade/bad-utf8", invalid));
    invalid = valid;
    invalid.p[0].nick[0] = (char)0xC2;
    invalid.p[0].nick[1] = (char)0x80; // Unicode C1 control U+0080
    invalid.p[0].nick[2] = '\0';
    assert(!haHistWriteRecord("/hotspot-arcade/bad-control", invalid));
    invalid = valid;
    std::strcpy(invalid.p[1].clientId, invalid.p[0].clientId);
    assert(!haHistWriteRecord("/hotspot-arcade/bad-duplicate", invalid));
    invalid = valid;
    invalid.p[0].clientId[0] = 'A';
    assert(!haHistWriteRecord("/hotspot-arcade/bad-identity", invalid));
    invalid = valid;
    invalid.game = 250;
    assert(!haHistWriteRecord("/hotspot-arcade/bad-game", invalid));

    auto original = SD.files.at(activePath);
    assert(!original.empty() && original.back() == '\n');
    SD.files[activePath].pop_back();
    assert(!haHistReadRecord(activePath, invalid, 0));
    SD.files[activePath] = original;
    SD.files[activePath][20] ^= 1;
    assert(!haHistReadRecord(activePath, invalid, 0));
    SD.files[activePath] = original;

    const auto baselineFiles = SD.files;
    const auto baselineNvs = Preferences::storage;
    HaHost changedHost = hostWithPlayers(999, -10);
    SD.writeRemaining = 100000;
    assert(haHistCheckpoint(changedHost, true));
    const char* changedPath = haHistRt.activeSlot == 0 ? HA_HIST_ACTIVE_A : HA_HIST_ACTIVE_B;
    const long fullLength = (long)SD.files.at(changedPath).size();
    assert(fullLength > 0);

    for(long cut = 0; cut < fullLength; cut++) {
        SD.files = baselineFiles;
        Preferences::storage = baselineNvs;
        resetHistoryRuntime(false);
        assert(haHistBegin());
        SD.writeRemaining = cut;
        assert(!haHistCheckpoint(changedHost, true));
        SD.writeRemaining = -1;
        resetHistoryRuntime(false);
        assert(haHistBegin());
        assert(haHistActive.count == 2);
        assert(haHistActive.p[0].score == 10);
    }
    assert(haHistCheckpointRestored(goodHost, 77));
    assert(haHistActive.restoredFrom == 77);

    // A restore is a new active session even when the current active session is
    // empty and therefore has nothing to archive first.
    HaHost empty = {};
    assert(haHistCheckpoint(empty, true));
    const uint32_t emptyActiveNumber = haHistActive.num;
    const uint32_t emptyActiveGeneration = haHistActive.seq;
    assert(haHistStartRestoredActive(goodHost, 78));
    assert(haHistActive.num != emptyActiveNumber);
    assert(haHistActive.seq == emptyActiveGeneration + 1);
    assert(haHistActive.restoredFrom == 78);
    assert(haHistActive.count == 2);

    // A pre-protocol host snapshot has no durable identity. It must remain
    // checkpointable without inventing a transient PID-shaped identity.
    HaHost legacyHost = {};
    legacyHost.activeGame = HA_GAME_TRIVIA;
    legacyHost.p[1].used = true;
    std::strcpy(legacyHost.p[1].nick, "LEGACY");
    legacyHost.p[1].score = 7;
    assert(haHistCheckpoint(legacyHost, true));
    assert(haHistActive.count == 1 && !haHistActive.p[0].clientId[0]);
}

static void testArchiveEqualityAndIndexCommitFailure() {
    resetHistoryRuntime();
    assert(haHistBegin());
    HaHost host = hostWithPlayers(40, 20);
    assert(haHistCheckpoint(host, true));

    HaHost wrongHost = hostWithPlayers(777, 20);
    HaHistSession wrong = {};
    haHistFromHost(wrongHost, wrong);
    wrong.num = haHistActive.num;
    wrong.seq = haHistActive.seq + 1;
    if(!wrong.seq) wrong.seq = 1;
    wrong.restoredFrom = haHistActive.restoredFrom;
    wrong.archived = true;
    const std::string collision = archivePath(wrong.num);
    assert(haHistWriteRecord(collision.c_str(), wrong));
    assert(!haHistArchive(host));
    assert(haHistRt.resumeAvailable);
    assert(haHistActive.count == 2 && haHistActive.p[0].score == 40);

    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(haHistResumeAvailable());
    assert(haHistActive.p[0].score == 40);

    // Remove the deliberate immutable collision and prove an index install failure
    // never advances the active slot even though the archive itself is durable.
    assert(SD.remove(collision.c_str()));
    SD.failRenameDestination = HA_HIST_INDEX;
    assert(!haHistArchive(host));
    assert(haHistActive.count == 2);
    const uint32_t committedNumber = haHistActive.num;
    assert(SD.exists(archivePath(committedNumber).c_str()));
    SD.failRenameDestination.clear();
    assert(haHistArchive(host));
    assert(haHistActive.count == 0);
    assert(haHistIndexValidate(HA_HIST_INDEX));
}

static void testBootRepairsAStaleIndexBeforeSuppressingActive() {
    resetHistoryRuntime();
    assert(haHistBegin());
    HaHost host = hostWithPlayers(55, 11);
    assert(haHistCheckpoint(host, true));
    HaHistSession committed = {};
    haHistFromHost(host, committed);
    committed.num = haHistActive.num;
    committed.seq = haHistActive.seq + 1;
    if(!committed.seq) committed.seq = 1;
    committed.restoredFrom = haHistActive.restoredFrom;
    committed.archived = true;
    assert(haHistWriteRecord(archivePath(committed.num).c_str(), committed));
    assert(haHist.total == 0); // simulate power loss before index replacement

    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(haHist.total == 1);
    assert(haHistActive.count == 0);
    assert(!haHistResumeAvailable());
    assert(haHistCatalogNewest() && haHist.s[0].num == committed.num);
}

static void testIndexedNewestFirstBrowsingAndRebuild() {
    resetHistoryRuntime();
    assert(haHistBegin());
    HaHost host = hostWithPlayers();
    for(int session = 1; session <= 8; session++) {
        host.session[0].score = session * 100;
        host.session[1].score = session;
        assert(haHistArchive(host));
    }
    assert(haHist.total == 8);
    assert(haHistCatalogNewest());
    assert(haHist.count == HA_HIST_PAGE_MAX);
    assert(haHist.s[0].num == 8 && haHist.s[5].num == 3);
    assert(!haHist.hasNewer && haHist.hasOlder);
    HaHistSession detail = {};
    assert(haHistLoadSession(8, detail));
    haHistSetRestoreHandler(acceptRestore);
    assert(haHistRequestRestore(detail));
    assert(haHistCatalogOlder());
    assert(haHist.count == 2 && haHist.s[0].num == 2 && haHist.s[1].num == 1);
    assert(haHist.hasNewer && !haHist.hasOlder);
    assert(haHistCatalogNewer());
    assert(haHist.s[0].num == 8);

    SD.files[HA_HIST_INDEX][5] ^= 0x80;
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(haHist.total == 8 && haHistIndexValidate(HA_HIST_INDEX));

    // A corrupt immutable record is retained for recovery, but a rebuilt cache
    // excludes it and browsing continues with the next verified session.
    SD.files[archivePath(8)][30] ^= 1;
    SD.files[HA_HIST_INDEX][5] ^= 0x40;
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(SD.exists(archivePath(8).c_str()));
    assert(haHist.total == 7);
    assert(haHistCatalogNewest());
    assert(haHist.s[0].num == 7);
}

static const char* LEGACY_HISTORY =
    "SESSION 1 2\n"
    "10\tALICE\n"
    "5\tBOB\n"
    "SESSION 2 1\n"
    "20\tCAROL\n"
    "SESSION 3 1\n"
    "30\tDAVE\n";

static const char* LEGACY_CURRENT =
    "CURRENT 2\n"
    "31\tDAVE\n"
    "8\tALICE\n";

static void testMigrationResumesAfterPartialArchiveCommit() {
    resetHistoryRuntime();
    SD.putText(HA_HIST_LEGACY_ARCHIVE, LEGACY_HISTORY);
    SD.putText(HA_HIST_LEGACY_CURRENT, LEGACY_CURRENT);
    SD.failRenameDestination = archivePath(2);
    assert(!haHistBegin());
    assert(SD.exists(archivePath(1).c_str()));
    assert(!SD.exists(archivePath(2).c_str()));
    assert(SD.exists(HA_HIST_LEGACY_ARCHIVE));
    assert(SD.exists(HA_HIST_LEGACY_CURRENT));

    SD.failRenameDestination.clear();
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(!SD.exists(HA_HIST_LEGACY_ARCHIVE));
    assert(!SD.exists(HA_HIST_LEGACY_CURRENT));
    assert(SD.exists(HA_HIST_LEGACY_ARCHIVE_IMPORTED));
    assert(SD.exists(HA_HIST_LEGACY_CURRENT_IMPORTED));
    assert(haHist.total == 3);
    assert(haHistActive.count == 2);
    assert(std::strcmp(haHistActive.p[0].nick, "DAVE") == 0);

    const auto files = SD.files;
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(SD.files == files);
    assert(haHist.total == 3);
}

static void testMigrationRenamesAreVerifiedAndRetryable() {
    resetHistoryRuntime();
    SD.putText(HA_HIST_LEGACY_ARCHIVE, LEGACY_HISTORY);
    SD.failRenameDestination = HA_HIST_LEGACY_ARCHIVE_IMPORTED;
    assert(!haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_ARCHIVE));
    for(uint32_t id = 1; id <= 3; id++) assert(SD.exists(archivePath(id).c_str()));
    SD.failRenameDestination.clear();
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_ARCHIVE_IMPORTED));
    assert(haHist.total == 3);

    resetHistoryRuntime();
    SD.putText(HA_HIST_LEGACY_CURRENT, LEGACY_CURRENT);
    SD.failRenameDestination = HA_HIST_LEGACY_CURRENT_IMPORTED;
    assert(!haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_CURRENT));
    assert(SD.exists(HA_HIST_ACTIVE_A) || SD.exists(HA_HIST_ACTIVE_B));
    SD.failRenameDestination.clear();
    resetHistoryRuntime(false);
    assert(haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_CURRENT_IMPORTED));
    assert(haHistActive.count == 2);
}

static void testMigrationNeverOverwritesAConflictingArchive() {
    resetHistoryRuntime();
    assert(haHistStorageBegin());
    assert(SD.mkdir(HA_HIST_DIR));
    assert(SD.mkdir(HA_HIST_ARCHIVE_DIR));
    HaHost host = hostWithPlayers(999, 1);
    HaHistSession conflicting = {};
    haHistFromHost(host, conflicting);
    conflicting.num = 1;
    conflicting.seq = 1;
    conflicting.archived = true;
    assert(haHistWriteRecord(archivePath(1).c_str(), conflicting));
    SD.putText(HA_HIST_LEGACY_ARCHIVE, LEGACY_HISTORY);
    resetHistoryRuntime(false);
    assert(!haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_ARCHIVE));
    HaHistSession loaded = {};
    assert(haHistReadRecord(archivePath(1).c_str(), loaded, 1));
    assert(loaded.p[0].score == 999);
}

static void testMigrationRejectsMalformedHeadersAndNvsWriteFailures() {
    static const char* malformedArchives[] = {
        "SESSION 1junk 1\n10\tALICE\n",
        "SESSION 1 1 trailing\n10\tALICE\n",
        "SESSION 4294967296 1\n10\tALICE\n",
    };
    for(const char* input : malformedArchives) {
        resetHistoryRuntime();
        SD.putText(HA_HIST_LEGACY_ARCHIVE, input);
        assert(!haHistBegin());
        assert(SD.exists(HA_HIST_LEGACY_ARCHIVE));
        assert(!SD.exists(HA_HIST_LEGACY_ARCHIVE_IMPORTED));
    }

    resetHistoryRuntime();
    SD.putText(HA_HIST_LEGACY_CURRENT, "CURRENT 1 trailing\n10\tALICE\n");
    assert(!haHistBegin());
    assert(SD.exists(HA_HIST_LEGACY_CURRENT));

    resetHistoryRuntime();
    Preferences::failWrites = true;
    assert(!haHistBegin());
    assert(!haHistRt.begun);
    Preferences::failWrites = false;
    resetHistoryRuntime(false);
    assert(haHistBegin());
}

static void testEveryLegacyMigrationWriteIsRetryable() {
    resetHistoryRuntime();
    SD.putText(HA_HIST_LEGACY_ARCHIVE, LEGACY_HISTORY);
    SD.putText(HA_HIST_LEGACY_CURRENT, LEGACY_CURRENT);
    constexpr long budget = 100000;
    SD.writeRemaining = budget;
    assert(haHistBegin());
    const long writesUsed = budget - SD.writeRemaining;
    assert(writesUsed > 0 && writesUsed < budget);

    for(long cut = 0; cut < writesUsed; cut++) {
        resetHistoryRuntime();
        SD.putText(HA_HIST_LEGACY_ARCHIVE, LEGACY_HISTORY);
        SD.putText(HA_HIST_LEGACY_CURRENT, LEGACY_CURRENT);
        SD.writeRemaining = cut;
        assert(!haHistBegin());
        SD.writeRemaining = -1;
        resetHistoryRuntime(false);
        assert(haHistBegin());
        assert(SD.exists(HA_HIST_LEGACY_ARCHIVE_IMPORTED));
        assert(SD.exists(HA_HIST_LEGACY_CURRENT_IMPORTED));
        if(haHist.total != 3 || haHistActive.count != 2)
            std::cerr << "migration retry mismatch at cut " << cut
                      << ": total=" << haHist.total
                      << " active=" << (unsigned)haHistActive.count << '\n';
        assert(haHist.total == 3 && haHistActive.count == 2);
        for(uint32_t id = 1; id <= 3; id++) {
            HaHistSession loaded = {};
            assert(haHistReadRecord(archivePath(id).c_str(), loaded, 1));
            assert(loaded.num == id);
        }
    }
}

static void testGenerationExhaustionNeverWraps() {
    resetHistoryRuntime();
    assert(haHistBegin());
    HaHost baseline = hostWithPlayers();
    assert(haHistCheckpoint(baseline, true));
    haHistActive.seq = UINT32_MAX;
    // An unchanged no-op remains safe, but every operation requiring a durable
    // successor fails instead of reusing generation one.
    assert(haHistCheckpoint(baseline, false));
    HaHost changed = hostWithPlayers(99, 5);
    assert(!haHistCheckpoint(changed, true));
    assert(!haHistStartNewActive(changed));
    assert(!haHistArchive(changed));
    HaHistSession archive = haHistActive;
    archive.archived = true;
    archive.seq = 1;
    assert(!haHistArchiveCommitsActive(archive, haHistActive));
}

int main() {
    testKnownGameValidation();
    testStrictRecordsAndInterruptedActiveWrites();
    testArchiveEqualityAndIndexCommitFailure();
    testBootRepairsAStaleIndexBeforeSuppressingActive();
    testIndexedNewestFirstBrowsingAndRebuild();
    testMigrationResumesAfterPartialArchiveCommit();
    testMigrationRenamesAreVerifiedAndRetryable();
    testMigrationNeverOverwritesAConflictingArchive();
    testMigrationRejectsMalformedHeadersAndNvsWriteFailures();
    testEveryLegacyMigrationWriteIsRetryable();
    testGenerationExhaustionNeverWraps();
    resetHistoryRuntime();
    std::cout << "native history tests passed\n";
}
