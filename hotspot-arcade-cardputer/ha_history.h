// Versioned, power-loss-tolerant session persistence on the microSD card.
//
// Active state alternates between active.a and active.b. A torn write can damage at
// most the new slot; boot selects the valid record with the newest sequence. Finished
// sessions are immutable, one file per session, and the catalog loads only one small
// page of summaries at a time. All parsing is fixed-buffer and CRC checked.
//
// Format v2 (percent-encoded UTF-8 text, CRC32 over every line before crc=):
//   HA2|ACTIVE                 (or HA2|ARCHIVE)
//   seq=12
//   session=7
//   restored_from=0
//   game=1
//   gameplays=1
//   gameplay=1|3
//   players=2
//   player=<client-id>|120|<avatar>|ALICE
//   player=<client-id>|80|<avatar>|BOB
//   crc=1234ABCD
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <limits.h>
#include <stdarg.h>
#include "ha_host.h"

#ifndef HA_HIST_PAGE_MAX
#define HA_HIST_PAGE_MAX 6
#endif
#define HA_HIST_LINE_MAX 256
#define HA_HIST_RECORD_MAX 8192
static_assert(HA_HIST_PAGE_MAX > 0 && HA_HIST_PAGE_MAX <= 127, "history page bound");
static_assert(HA_HIST_LINE_MAX >= 256, "v2 player records require 256-byte lines");

static const char* const HA_HIST_DIR = "/hotspot-arcade";
static const char* const HA_HIST_ARCHIVE_DIR = "/hotspot-arcade/history";
static const char* const HA_HIST_ACTIVE_A = "/hotspot-arcade/active.a";
static const char* const HA_HIST_ACTIVE_B = "/hotspot-arcade/active.b";
static const char* const HA_HIST_LEGACY_CURRENT = "/hotspot-arcade/current.txt";
static const char* const HA_HIST_LEGACY_ARCHIVE = "/hotspot-arcade/history.txt";

struct HaHistPlayer {
    char clientId[HA_CLIENT_ID_LEN];
    char avatar[HA_AVATAR_LEN];
    char nick[HA_NICK_LEN];
    int32_t score;
};

struct HaHistGamePlay {
    uint8_t game;
    uint16_t count;
};

struct HaHistSession {
    uint32_t num;
    uint32_t seq;
    uint32_t restoredFrom; // source archive number, or 0 for an original session
    uint8_t game;
    uint8_t count;
    uint8_t gameCount;
    bool archived;
    HaHistPlayer p[HA_SESSION_MAX_PLAYERS];
    HaHistGamePlay games[HA_SESSION_GAME_STATS_MAX];
};

struct HaHistSummary {
    uint32_t num;
    uint8_t game;
    uint8_t count;
    char leader[HA_NICK_LEN];
    int32_t leaderScore;
};

struct HaHistCatalog {
    HaHistSummary s[HA_HIST_PAGE_MAX]; // newest..oldest within this page
    uint8_t count;
    uint32_t total;
    uint32_t minNum;
    uint32_t maxNum;
    bool hasNewer;
    bool hasOlder;
};

struct HaHistRuntime {
    bool begun;
    bool resumeAvailable;
    int8_t activeSlot; // -1 none, 0 A, 1 B
    HaHistSession* active;
};

static HaHistCatalog* haHistStorage = nullptr;
#define haHist (*haHistStorage)
static HaHistRuntime haHistRt = {false, false, -1, nullptr};
static HaHistSession* haHistScratch = nullptr;
#define haHistActive (*haHistRt.active)

// Active + one serialized work record are allocated once at first SD use. This
// keeps ~4KB out of static DRAM without risking the loop task's small stack. Every
// history API runs on loop(), never from AsyncTCP; callbacks receive caller-owned
// records so the one scratch buffer is never re-entered by restore logic.
static bool haHistStorageBegin() {
    if(haHistRt.active && haHistScratch && haHistStorage) return true;
    HaHistSession* active = new(std::nothrow) HaHistSession{};
    HaHistSession* scratch = new(std::nothrow) HaHistSession{};
    HaHistCatalog* catalog = new(std::nothrow) HaHistCatalog{};
    if(!active || !scratch || !catalog) {
        delete active;
        delete scratch;
        delete catalog;
        return false;
    }
    haHistRt.active = active;
    haHistScratch = scratch;
    haHistStorage = catalog;
    return true;
}

static bool haHistStorageReady() {
    return haHistRt.active && haHistScratch && haHistStorage;
}

extern bool haSdOk;

typedef bool (*HaHistRestoreHandler)(const HaHistSession& session);
static HaHistRestoreHandler haHistRestoreHandler = nullptr;

static uint32_t haHistCrcUpdate(uint32_t crc, const uint8_t* data, size_t len) {
    while(len--) {
        crc ^= *data++;
        for(uint8_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static uint32_t haHistCrcLine(uint32_t crc, const char* line) {
    crc = haHistCrcUpdate(crc, (const uint8_t*)line, strlen(line));
    const uint8_t nl = '\n';
    return haHistCrcUpdate(crc, &nl, 1);
}

// 1 = a complete line, 0 = clean EOF, -1 = line exceeded the fixed buffer.
static int haHistReadLine(File& f, char* out, size_t cap) {
    if(!out || cap < 2) return -1;
    size_t n = 0;
    bool got = false;
    bool overflow = false;
    while(f.available()) {
        int v = f.read();
        if(v < 0) break;
        got = true;
        char c = (char)v;
        if(c == '\n') break;
        if(c == '\r') continue;
        if(n + 1 < cap) out[n++] = c;
        else overflow = true;
    }
    out[n] = '\0';
    if(overflow) return -1;
    return got ? 1 : 0;
}

static bool haHistParseU32(const char* s, uint32_t& out) {
    if(!s || !s[0] || s[0] == '-') return false;
    char* end = nullptr;
    unsigned long long v = strtoull(s, &end, 10);
    if(!end || *end || v > UINT32_MAX) return false;
    out = (uint32_t)v;
    return true;
}

static bool haHistParseI32(const char* s, int32_t& out) {
    if(!s || !s[0]) return false;
    char* end = nullptr;
    long long v = strtoll(s, &end, 10);
    if(!end || *end || v < INT32_MIN || v > INT32_MAX) return false;
    out = (int32_t)v;
    return true;
}

static int haHistHex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool haHistEncode(const char* in, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    if(!in || !out || !cap) return false;
    size_t n = 0;
    for(const uint8_t* p = (const uint8_t*)in; *p; p++) {
        uint8_t c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        size_t need = plain ? 1 : 3;
        if(n + need >= cap) return false;
        if(plain) out[n++] = (char)c;
        else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    return true;
}

static bool haHistDecode(const char* in, char* out, size_t cap) {
    if(!in || !out || !cap) return false;
    size_t n = 0;
    for(size_t i = 0; in[i]; i++) {
        uint8_t c = (uint8_t)in[i];
        if(c == '%') {
            if(!in[i + 1] || !in[i + 2]) return false;
            int hi = haHistHex(in[i + 1]);
            int lo = haHistHex(in[i + 2]);
            if(hi < 0 || lo < 0) return false;
            c = (uint8_t)((hi << 4) | lo);
            if(c == 0) return false;
            i += 2;
        }
        if(n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return true;
}

static bool haHistWriteLine(File& f, uint32_t& crc, const char* fmt, ...) {
    char line[HA_HIST_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if(n < 0 || (size_t)n >= sizeof(line)) return false;
    if(f.write((const uint8_t*)line, (size_t)n) != (size_t)n) return false;
    if(f.write((uint8_t)'\n') != 1) return false;
    crc = haHistCrcLine(crc, line);
    return true;
}

static bool haHistSameState(const HaHistSession& a, const HaHistSession& b) {
    if(a.restoredFrom != b.restoredFrom || a.game != b.game ||
       a.count != b.count || a.gameCount != b.gameCount)
        return false;
    for(uint8_t i = 0; i < a.count; i++) {
        if(a.p[i].score != b.p[i].score ||
           strcmp(a.p[i].clientId, b.p[i].clientId) != 0 ||
           strcmp(a.p[i].avatar, b.p[i].avatar) != 0 ||
           strcmp(a.p[i].nick, b.p[i].nick) != 0)
            return false;
    }
    for(uint8_t i = 0; i < a.gameCount; i++)
        if(a.games[i].game != b.games[i].game ||
           a.games[i].count != b.games[i].count)
            return false;
    return true;
}

static void haHistSort(HaHistSession& s) {
    for(uint8_t i = 1; i < s.count; i++) {
        HaHistPlayer key = s.p[i];
        int j = i - 1;
        while(j >= 0 &&
              (s.p[j].score < key.score ||
               (s.p[j].score == key.score && strcmp(s.p[j].nick, key.nick) > 0))) {
            s.p[j + 1] = s.p[j];
            j--;
        }
        s.p[j + 1] = key;
    }
}

static void haHistSortGames(HaHistSession& s) {
    for(uint8_t i = 1; i < s.gameCount; i++) {
        HaHistGamePlay key = s.games[i];
        int j = i - 1;
        while(j >= 0 && s.games[j].game > key.game) {
            s.games[j + 1] = s.games[j];
            j--;
        }
        s.games[j + 1] = key;
    }
}

static void haHistFromHost(const HaHost& src, HaHistSession& dst) {
    dst = HaHistSession{};
    dst.game = src.activeGame;
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS && dst.count < HA_SESSION_MAX_PLAYERS; i++) {
        const HaHostSessionPlayer& p = src.session[i];
        if(!p.used) continue;
        HaHistPlayer& hp = dst.p[dst.count++];
        strlcpy(hp.clientId, p.clientId, sizeof(hp.clientId));
        strlcpy(hp.avatar, p.avatar, sizeof(hp.avatar));
        strlcpy(hp.nick, p.nick, sizeof(hp.nick));
        hp.score = p.score;
    }
    // Compatibility with snapshots made before the session ledger was initialized.
    if(dst.count == 0) {
        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS && dst.count < HA_SESSION_MAX_PLAYERS; pid++) {
            if(!src.p[pid].used) continue;
            HaHistPlayer& hp = dst.p[dst.count++];
            snprintf(hp.clientId, sizeof(hp.clientId), "pid-%u", (unsigned)pid);
            strlcpy(hp.nick, src.p[pid].nick, sizeof(hp.nick));
            hp.score = src.p[pid].score;
        }
    }
    for(uint8_t i = 0;
        i < src.gameCount && i < HA_SESSION_GAME_STATS_MAX &&
        dst.gameCount < HA_SESSION_GAME_STATS_MAX;
        i++) {
        if(src.games[i].game == HA_GAME_NONE || !src.games[i].count) continue;
        dst.games[dst.gameCount++] = HaHistGamePlay{
            src.games[i].game,
            src.games[i].count
        };
    }
    haHistSort(dst);
    haHistSortGames(dst);
}

static bool haHistWriteRecord(const char* path, const HaHistSession& s) {
    if(!path || !s.num || !s.seq || s.count > HA_SESSION_MAX_PLAYERS ||
       s.gameCount > HA_SESSION_GAME_STATS_MAX)
        return false;
    SD.remove(path); // callers only pass a disposable active slot or temp file
    File f = SD.open(path, FILE_WRITE);
    if(!f) return false;
    uint32_t crc = 0xFFFFFFFFUL;
    bool ok =
        haHistWriteLine(f, crc, "HA2|%s", s.archived ? "ARCHIVE" : "ACTIVE") &&
        haHistWriteLine(f, crc, "seq=%lu", (unsigned long)s.seq) &&
        haHistWriteLine(f, crc, "session=%lu", (unsigned long)s.num) &&
        haHistWriteLine(f, crc, "restored_from=%lu", (unsigned long)s.restoredFrom) &&
        haHistWriteLine(f, crc, "game=%u", (unsigned)s.game) &&
        haHistWriteLine(f, crc, "gameplays=%u", (unsigned)s.gameCount) &&
        haHistWriteLine(f, crc, "players=%u", (unsigned)s.count);
    for(uint8_t i = 0; ok && i < s.gameCount; i++) {
        if(s.games[i].game == HA_GAME_NONE || !s.games[i].count) {
            ok = false;
            break;
        }
        ok = haHistWriteLine(
            f,
            crc,
            "gameplay=%u|%u",
            (unsigned)s.games[i].game,
            (unsigned)s.games[i].count);
    }
    for(uint8_t i = 0; ok && i < s.count; i++) {
        char id[HA_CLIENT_ID_LEN * 3];
        char avatar[HA_AVATAR_LEN * 3];
        char nick[HA_NICK_LEN * 3];
        ok = haHistEncode(s.p[i].clientId, id, sizeof(id)) &&
             haHistEncode(s.p[i].avatar, avatar, sizeof(avatar)) &&
             haHistEncode(s.p[i].nick, nick, sizeof(nick)) &&
             haHistWriteLine(
                 f,
                 crc,
                 "player=%s|%ld|%s|%s",
                 id,
                 (long)s.p[i].score,
                 avatar,
                 nick);
    }
    if(ok) {
        char tail[24];
        int n = snprintf(tail, sizeof(tail), "crc=%08lX\n", (unsigned long)~crc);
        ok = n > 0 && (size_t)n < sizeof(tail) &&
             f.write((const uint8_t*)tail, (size_t)n) == (size_t)n;
    }
    f.flush();
    if(!f.size() || f.size() > HA_HIST_RECORD_MAX) ok = false;
    f.close();
    if(!ok) SD.remove(path);
    return ok;
}

static bool haHistReadRecord(const char* path, HaHistSession& out, int expectedArchive = -1) {
    File f = SD.open(path, FILE_READ);
    if(!f) return false;
    if(!f.size() || f.size() > HA_HIST_RECORD_MAX) {
        f.close();
        return false;
    }
    out = HaHistSession{};
    HaHistSession& s = out;
    char line[HA_HIST_LINE_MAX];
    int lr = haHistReadLine(f, line, sizeof(line));
    bool ok = lr == 1;
    if(ok && strcmp(line, "HA2|ACTIVE") == 0) s.archived = false;
    else if(ok && strcmp(line, "HA2|ARCHIVE") == 0) s.archived = true;
    else ok = false;
    if(ok && expectedArchive >= 0 && s.archived != (expectedArchive != 0)) ok = false;

    uint32_t crc = 0xFFFFFFFFUL;
    if(ok) crc = haHistCrcLine(crc, line);
    bool haveSeq = false, haveSession = false, haveRestoredFrom = false;
    bool haveGame = false, haveGameplays = false, havePlayers = false;
    uint32_t declaredPlayers = 0, declaredGameplays = 0;
    bool haveCrc = false;
    uint16_t lines = 0;

    while(ok && (lr = haHistReadLine(f, line, sizeof(line))) == 1) {
        if(++lines > HA_SESSION_MAX_PLAYERS + HA_SESSION_GAME_STATS_MAX + 16) {
            ok = false;
            break;
        }
        if(strncmp(line, "crc=", 4) == 0) {
            if(strlen(line + 4) != 8) { ok = false; break; }
            char* end = nullptr;
            unsigned long stored = strtoul(line + 4, &end, 16);
            ok = end && !*end && (uint32_t)stored == ~crc;
            haveCrc = ok;
            if(ok) ok = haHistReadLine(f, line, sizeof(line)) == 0;
            break;
        }

        crc = haHistCrcLine(crc, line);
        if(strncmp(line, "seq=", 4) == 0 && !haveSeq) {
            haveSeq = haHistParseU32(line + 4, s.seq) && s.seq != 0;
            ok = haveSeq;
        } else if(strncmp(line, "session=", 8) == 0 && !haveSession) {
            haveSession = haHistParseU32(line + 8, s.num) && s.num != 0;
            ok = haveSession;
        } else if(strncmp(line, "restored_from=", 14) == 0 && !haveRestoredFrom) {
            haveRestoredFrom = haHistParseU32(line + 14, s.restoredFrom);
            ok = haveRestoredFrom;
        } else if(strncmp(line, "game=", 5) == 0 && !haveGame) {
            uint32_t v = 0;
            haveGame = haHistParseU32(line + 5, v) && v <= UINT8_MAX;
            if(haveGame) s.game = (uint8_t)v;
            ok = haveGame;
        } else if(strncmp(line, "gameplays=", 10) == 0 && !haveGameplays) {
            haveGameplays = haHistParseU32(line + 10, declaredGameplays) &&
                            declaredGameplays <= HA_SESSION_GAME_STATS_MAX;
            ok = haveGameplays;
        } else if(strncmp(line, "gameplay=", 9) == 0 &&
                  s.gameCount < HA_SESSION_GAME_STATS_MAX) {
            char* gameText = line + 9;
            char* sep = strchr(gameText, '|');
            if(!sep || strchr(sep + 1, '|')) { ok = false; break; }
            *sep = '\0';
            uint32_t game = 0, count = 0;
            ok = haHistParseU32(gameText, game) && game > HA_GAME_NONE &&
                 game <= UINT8_MAX && haHistParseU32(sep + 1, count) &&
                 count > 0 && count <= UINT16_MAX;
            for(uint8_t i = 0; ok && i < s.gameCount; i++)
                if(s.games[i].game == (uint8_t)game) ok = false;
            if(ok) {
                s.games[s.gameCount++] = HaHistGamePlay{
                    (uint8_t)game,
                    (uint16_t)count
                };
            }
        } else if(strncmp(line, "players=", 8) == 0 && !havePlayers) {
            havePlayers = haHistParseU32(line + 8, declaredPlayers) &&
                          declaredPlayers <= HA_SESSION_MAX_PLAYERS;
            ok = havePlayers;
        } else if(strncmp(line, "player=", 7) == 0 &&
                  s.count < HA_SESSION_MAX_PLAYERS) {
            char* id = line + 7;
            char* sep1 = strchr(id, '|');
            char* sep2 = sep1 ? strchr(sep1 + 1, '|') : nullptr;
            if(!sep1 || !sep2) { ok = false; break; }
            *sep1 = '\0';
            *sep2 = '\0';
            HaHistPlayer& p = s.p[s.count];
            char* avatarOrNick = sep2 + 1;
            char* sep3 = strchr(avatarOrNick, '|');
            if(sep3) *sep3 = '\0';
            ok = (!sep3 || !strchr(sep3 + 1, '|')) &&
                 haHistDecode(id, p.clientId, sizeof(p.clientId)) &&
                 haHistParseI32(sep1 + 1, p.score);
            if(ok && sep3)
                ok = haHistDecode(avatarOrNick, p.avatar, sizeof(p.avatar)) &&
                     haHistDecode(sep3 + 1, p.nick, sizeof(p.nick));
            else if(ok)
                ok = haHistDecode(avatarOrNick, p.nick, sizeof(p.nick));
            ok = ok && p.nick[0];
            if(ok) s.count++;
        } else {
            ok = false;
        }
    }
    f.close();
    ok = ok && haveCrc && haveSeq && haveSession && haveGame && havePlayers &&
         declaredPlayers == s.count &&
         (!haveGameplays ? s.gameCount == 0 : declaredGameplays == s.gameCount);
    // `restored_from` and `gameplays` were added without changing the HA2 marker;
    // their absence therefore means zero when reading an earlier v2 record.
    (void)haveRestoredFrom;
    if(ok) haHistSortGames(s);
    return ok;
}

static void haHistArchivePath(uint32_t num, char* out, size_t cap) {
    snprintf(out, cap, "%s/S%08lu.ha", HA_HIST_ARCHIVE_DIR, (unsigned long)num);
}

static bool haHistWriteArchive(const HaHistSession& source) {
    if(!haSdOk || !source.num || !source.count) return false;
    if(!source.archived) return false;
    char finalPath[64];
    char tempPath[64];
    haHistArchivePath(source.num, finalPath, sizeof(finalPath));
    snprintf(tempPath, sizeof(tempPath), "%s/.archive.tmp", HA_HIST_ARCHIVE_DIR);
    if(SD.exists(finalPath)) return false; // immutable; caller handles commit recovery
    uint32_t expectedNum = source.num;
    uint32_t expectedSeq = source.seq;
    uint32_t expectedRestoredFrom = source.restoredFrom;
    uint8_t expectedGame = source.game;
    uint8_t expectedCount = source.count;
    uint8_t expectedGameCount = source.gameCount;
    SD.remove(tempPath);
    if(!haHistWriteRecord(tempPath, source)) return false;
    if(!haHistReadRecord(tempPath, *haHistScratch, 1) ||
       haHistScratch->num != expectedNum || haHistScratch->seq != expectedSeq ||
       haHistScratch->restoredFrom != expectedRestoredFrom ||
       haHistScratch->game != expectedGame || haHistScratch->count != expectedCount ||
       haHistScratch->gameCount != expectedGameCount) {
        SD.remove(tempPath);
        return false;
    }
    if(!SD.rename(tempPath, finalPath)) {
        SD.remove(tempPath);
        return false;
    }
    return true;
}

static bool haHistParseArchiveName(const char* path, uint32_t& num) {
    if(!path) return false;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if(n < 5 || base[0] != 'S' || strcmp(base + n - 3, ".ha") != 0) return false;
    uint32_t v = 0;
    for(size_t i = 1; i < n - 3; i++) {
        if(base[i] < '0' || base[i] > '9') return false;
        uint8_t d = (uint8_t)(base[i] - '0');
        if(v > (UINT32_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    if(!v) return false;
    num = v;
    return true;
}

static void haHistRefreshStats() {
    haHist.total = 0;
    haHist.minNum = 0;
    haHist.maxNum = 0;
    if(!haSdOk) return;
    File dir = SD.open(HA_HIST_ARCHIVE_DIR);
    if(!dir || !dir.isDirectory()) { if(dir) dir.close(); return; }
    File entry = dir.openNextFile();
    while(entry) {
        char name[96];
        strlcpy(name, entry.name(), sizeof(name));
        bool isDir = entry.isDirectory();
        entry.close();
        uint32_t num = 0;
        if(!isDir && haHistParseArchiveName(name, num)) {
            haHist.total++;
            if(!haHist.minNum || num < haHist.minNum) haHist.minNum = num;
            if(num > haHist.maxNum) haHist.maxNum = num;
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

static uint32_t haHistReserveNum() {
    uint32_t high = haHist.maxNum;
    if(haHistActive.num > high) high = haHistActive.num;
    Preferences p;
    if(p.begin("ha_hist", false)) {
        uint32_t saved = p.getUInt("n", 0);
        if(saved > high) high = saved;
        if(high == UINT32_MAX) { p.end(); return 0; }
        uint32_t next = high + 1;
        p.putUInt("n", next);
        p.end();
        return next;
    }
    return high == UINT32_MAX ? 0 : high + 1;
}

static bool haHistParseLegacyPlayer(char* line, HaHistPlayer& p) {
    char* tab = strchr(line, '\t');
    if(!tab) return false;
    *tab++ = '\0';
    p = HaHistPlayer{};
    return haHistParseI32(line, p.score) && tab[0] &&
           strlcpy(p.nick, tab, sizeof(p.nick)) < sizeof(p.nick);
}

static void haHistMigrateLegacyArchive() {
    if(!SD.exists(HA_HIST_LEGACY_ARCHIVE) || haHist.total) return;
    File f = SD.open(HA_HIST_LEGACY_ARCHIVE, FILE_READ);
    if(!f) return;
    HaHistSession& cur = *haHistScratch;
    cur = HaHistSession{};
    cur.archived = true;
    char line[HA_HIST_LINE_MAX];
    int lr = 0;
    while((lr = haHistReadLine(f, line, sizeof(line))) == 1) {
        if(strncmp(line, "SESSION ", 8) == 0) {
            if(cur.num && cur.count) haHistWriteArchive(cur);
            cur = HaHistSession{};
            cur.archived = true;
            unsigned long num = 0, ignoredCount = 0;
            if(sscanf(line, "SESSION %lu %lu", &num, &ignoredCount) >= 1 && num > 0) {
                cur.num = (uint32_t)num;
                cur.seq = cur.num;
            }
        } else if(cur.num && cur.count < HA_SESSION_MAX_PLAYERS) {
            HaHistPlayer p;
            if(haHistParseLegacyPlayer(line, p)) cur.p[cur.count++] = p;
        }
    }
    if(lr == 0 && cur.num && cur.count) haHistWriteArchive(cur);
    f.close();
    haHistRefreshStats();
}

static bool haHistReadLegacyCurrent(HaHistSession& out) {
    File f = SD.open(HA_HIST_LEGACY_CURRENT, FILE_READ);
    if(!f) return false;
    if(!f.size() || f.size() > HA_HIST_RECORD_MAX) {
        f.close();
        return false;
    }
    char line[HA_HIST_LINE_MAX];
    int lr = haHistReadLine(f, line, sizeof(line));
    bool ok = lr == 1 &&
              strncmp(line, "CURRENT ", 8) == 0;
    out = HaHistSession{};
    while(ok && (lr = haHistReadLine(f, line, sizeof(line))) == 1) {
        if(!line[0]) continue;
        if(out.count >= HA_SESSION_MAX_PLAYERS) { ok = false; break; }
        HaHistPlayer p;
        if(!haHistParseLegacyPlayer(line, p)) { ok = false; break; }
        out.p[out.count++] = p;
    }
    if(lr < 0) ok = false;
    f.close();
    if(!ok || !out.count) return false;
    haHistSort(out);
    return true;
}

static bool haHistSeqNewer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static bool haHistWriteActive(const HaHistSession& source) {
    if(source.archived) return false;
    int8_t slot = haHistRt.activeSlot == 0 ? 1 : 0;
    const char* path = slot == 0 ? HA_HIST_ACTIVE_A : HA_HIST_ACTIVE_B;
    uint32_t expectedNum = source.num;
    uint32_t expectedSeq = source.seq;
    uint32_t expectedRestoredFrom = source.restoredFrom;
    uint8_t expectedGame = source.game;
    uint8_t expectedCount = source.count;
    uint8_t expectedGameCount = source.gameCount;
    if(!haHistWriteRecord(path, source)) return false;
    if(!haHistReadRecord(path, *haHistScratch, 0) ||
       haHistScratch->num != expectedNum || haHistScratch->seq != expectedSeq ||
       haHistScratch->restoredFrom != expectedRestoredFrom ||
       haHistScratch->game != expectedGame || haHistScratch->count != expectedCount ||
       haHistScratch->gameCount != expectedGameCount)
        return false;
    haHistActive = *haHistScratch;
    haHistRt.activeSlot = slot;
    haHistRt.resumeAvailable = haHistActive.count > 0;
    return true;
}

static bool haHistBegin() {
    if(!haSdOk) return false;
    if(haHistRt.begun) return true;
    if(!haHistStorageBegin()) return false;
    if(!SD.exists(HA_HIST_DIR) && !SD.mkdir(HA_HIST_DIR)) return false;
    if(!SD.exists(HA_HIST_ARCHIVE_DIR) && !SD.mkdir(HA_HIST_ARCHIVE_DIR)) return false;
    haHistRefreshStats();
    haHistMigrateLegacyArchive();

    uint32_t recoveryBaseSeq = 0;
    bool haveA = haHistReadRecord(HA_HIST_ACTIVE_A, haHistActive, 0);
    bool haveB = haHistReadRecord(HA_HIST_ACTIVE_B, *haHistScratch, 0);
    if(haveA || haveB) {
        bool useB = haveB && (!haveA || haHistSeqNewer(haHistScratch->seq, haHistActive.seq));
        if(useB) haHistActive = *haHistScratch;
        haHistRt.activeSlot = useB ? 1 : 0;
        // If power failed after the immutable archive rename but before the new
        // empty active slot landed, never resume that already-finished session.
        char archivedPath[64];
        haHistArchivePath(haHistActive.num, archivedPath, sizeof(archivedPath));
        uint32_t expectedSeq = haHistActive.seq + 1;
        if(!expectedSeq) expectedSeq = 1;
        bool alreadyArchived =
            haHistReadRecord(archivedPath, *haHistScratch, 1) &&
            haHistScratch->num == haHistActive.num && haHistScratch->seq == expectedSeq;
        if(alreadyArchived) recoveryBaseSeq = haHistScratch->seq;
        haHistRt.resumeAvailable = haHistActive.count > 0 && !alreadyArchived;
        if(!alreadyArchived) {
            haHistRt.begun = true;
            return true; // an empty active session is still current
        }
    }
    if(!haveA && !haveB) haHistActive = HaHistSession{};

    *haHistScratch = HaHistSession{};
    if(!haveA && !haveB) haHistReadLegacyCurrent(*haHistScratch);
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    haHistScratch->seq = (haveA || haveB)
                             ? (recoveryBaseSeq ? recoveryBaseSeq : haHistActive.seq) + 1
                             : 1;
    if(!haHistScratch->seq) haHistScratch->seq = 1;
    haHistScratch->archived = false;
    if(!haHistWriteActive(*haHistScratch)) return false;
    haHistRt.begun = true;
    return true;
}

static bool haHistCheckpointPrepared(
    const HaHost& host,
    bool force,
    uint32_t restoredFrom) {
    haHistFromHost(host, *haHistScratch);
    haHistScratch->restoredFrom = restoredFrom;
    haHistScratch->num = haHistActive.num ? haHistActive.num : haHistReserveNum();
    if(!haHistScratch->num) return false;
    haHistScratch->seq = haHistActive.seq + 1;
    if(!haHistScratch->seq) haHistScratch->seq = 1;
    haHistScratch->archived = false;
    if(!force && haHistActive.num == haHistScratch->num &&
       haHistSameState(haHistActive, *haHistScratch))
        return true;
    return haHistWriteActive(*haHistScratch);
}

static bool haHistCheckpoint(const HaHost& host, bool force = false) {
    if(!haHistBegin()) return false;
    return haHistCheckpointPrepared(host, force, haHistActive.restoredFrom);
}

// Use after importing an archived record into a freshly-created active session.
// This records provenance without making it part of the mutable HaHost mirror.
static bool haHistCheckpointRestored(const HaHost& host, uint32_t sourceNum) {
    if(!sourceNum || !haHistBegin()) return false;
    return haHistCheckpointPrepared(host, true, sourceNum);
}

// UI compatibility wrapper: callers pass an already locked snapshot, so SD I/O is
// never performed while ENGINE_LOCK is held.
[[maybe_unused]] static bool haHistSaveCurrent(const HaHost& snapshot) {
    return haSdOk && haHistCheckpoint(snapshot);
}

// Archive the current cumulative standings, then establish a new empty active slot.
// The caller should only reset engine/host scores after this returns true.
static bool haHistArchive(const HaHost& host) {
    if(!haHistBegin()) return false;
    uint32_t currentNum = haHistActive.num ? haHistActive.num : haHistReserveNum();
    if(!currentNum) return false;
    uint32_t finishedSeq = 0;
    uint32_t expectedSeq = haHistActive.seq + 1;
    if(!expectedSeq) expectedSeq = 1;
    char finalPath[64];
    haHistArchivePath(currentNum, finalPath, sizeof(finalPath));
    if(SD.exists(finalPath)) {
        // Recovery for a prior call that committed the immutable archive but lost
        // power (or an SD write) while advancing the active slot.
        if(!haHistReadRecord(finalPath, *haHistScratch, 1) ||
           haHistScratch->num != currentNum || haHistScratch->seq != expectedSeq)
            return false;
        finishedSeq = haHistScratch->seq;
    } else {
        haHistFromHost(host, *haHistScratch);
        if(!haHistScratch->count) return false;
        haHistScratch->restoredFrom = haHistActive.restoredFrom;
        haHistScratch->num = currentNum;
        haHistScratch->seq = expectedSeq;
        haHistScratch->archived = true;
        if(!haHistWriteArchive(*haHistScratch)) return false;
        finishedSeq = haHistScratch->seq;
    }
    haHistRefreshStats();

    *haHistScratch = HaHistSession{};
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    haHistScratch->seq = finishedSeq + 1;
    if(!haHistScratch->seq) haHistScratch->seq = 1;
    haHistScratch->archived = false;
    return haHistWriteActive(*haHistScratch);
}

static bool haHistLoadSession(uint32_t num, HaHistSession& out) {
    if(!haHistBegin() || !num) return false;
    char path[64];
    haHistArchivePath(num, path, sizeof(path));
    return haHistReadRecord(path, out, 1);
}

static HaHistSummary haHistSummary(const HaHistSession& s) {
    HaHistSummary out = {};
    out.num = s.num;
    out.game = s.game;
    out.count = s.count;
    if(s.count) {
        strlcpy(out.leader, s.p[0].nick, sizeof(out.leader));
        out.leaderScore = s.p[0].score;
        for(uint8_t i = 1; i < s.count; i++) {
            if(s.p[i].score <= out.leaderScore) continue;
            strlcpy(out.leader, s.p[i].nick, sizeof(out.leader));
            out.leaderScore = s.p[i].score;
        }
    }
    return out;
}

static void haHistInsertCandidate(
    uint32_t* ids,
    uint8_t& count,
    uint8_t cap,
    uint32_t num,
    bool ascending) {
    uint8_t pos = 0;
    while(pos < count && (ascending ? ids[pos] < num : ids[pos] > num)) pos++;
    if(pos >= cap) return;
    uint8_t end = count < cap ? count : cap - 1;
    for(int i = end; i > pos; i--) ids[i] = ids[i - 1];
    ids[pos] = num;
    if(count < cap) count++;
}

// `newer=false`: keep the largest ids below bound. `newer=true`: keep the smallest
// ids above bound, then reverse them so every displayed page is newest first.
static bool haHistCatalogLoadPage(bool newer, uint32_t bound) {
    if(!haHistBegin()) return false;
    haHist.count = 0;
    haHist.hasNewer = false;
    haHist.hasOlder = false;
    // Keep a few extra ids so a corrupt/torn archive does not blank an otherwise
    // valid page. Only these candidates are opened and CRC-checked; catalog paging
    // never reads every historical record into memory or from the card.
    static const uint8_t CANDIDATE_MAX = HA_HIST_PAGE_MAX * 2;
    uint32_t candidates[CANDIDATE_MAX];
    uint8_t candidateCount = 0;
    File dir = SD.open(HA_HIST_ARCHIVE_DIR);
    if(!dir || !dir.isDirectory()) { if(dir) dir.close(); return false; }
    File entry = dir.openNextFile();
    while(entry) {
        char name[96];
        strlcpy(name, entry.name(), sizeof(name));
        bool isDir = entry.isDirectory();
        entry.close();
        uint32_t num = 0;
        bool eligible = !isDir && haHistParseArchiveName(name, num) &&
                        (newer ? num > bound : num < bound);
        if(eligible)
            haHistInsertCandidate(
                candidates,
                candidateCount,
                CANDIDATE_MAX,
                num,
                newer);
        entry = dir.openNextFile();
    }
    dir.close();
    if(newer) {
        for(uint8_t i = 0; i < candidateCount / 2; i++) {
            uint32_t tmp = candidates[i];
            candidates[i] = candidates[candidateCount - 1 - i];
            candidates[candidateCount - 1 - i] = tmp;
        }
    }
    for(uint8_t i = 0; i < candidateCount && haHist.count < HA_HIST_PAGE_MAX; i++) {
        if(haHistLoadSession(candidates[i], *haHistScratch))
            haHist.s[haHist.count++] = haHistSummary(*haHistScratch);
    }
    if(haHist.count) {
        haHist.hasNewer = haHist.s[0].num < haHist.maxNum;
        haHist.hasOlder = haHist.s[haHist.count - 1].num > haHist.minNum;
    }
    return haHist.count > 0;
}

static bool haHistCatalogNewest() {
    if(!haHistBegin()) return false;
    haHistRefreshStats();
    return haHistCatalogLoadPage(false, UINT32_MAX);
}

static bool haHistCatalogOlder() {
    if(!haHistBegin()) return false;
    if(!haHist.count || !haHist.hasOlder) return false;
    return haHistCatalogLoadPage(false, haHist.s[haHist.count - 1].num);
}

static bool haHistCatalogNewer() {
    if(!haHistBegin()) return false;
    if(!haHist.count || !haHist.hasNewer) return false;
    return haHistCatalogLoadPage(true, haHist.s[0].num);
}

static bool haHistResumeAvailable() {
    return haSdOk && haHistBegin() && haHistRt.resumeAvailable;
}

[[maybe_unused]] static const HaHistSession* haHistResumeSession() {
    return haHistResumeAvailable() ? haHistRt.active : nullptr;
}

static void haHistSetRestoreHandler(HaHistRestoreHandler handler) {
    haHistRestoreHandler = handler;
}

static bool haHistCanRestore() {
    return haSdOk && haHistRestoreHandler != nullptr;
}

static bool haHistRequestRestore(const HaHistSession& session) {
    return haHistCanRestore() && session.archived && haHistRestoreHandler(session);
}
