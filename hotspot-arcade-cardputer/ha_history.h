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
static const char* const HA_HIST_LEGACY_CURRENT_IMPORTED =
    "/hotspot-arcade/current.txt.v1.imported";
static const char* const HA_HIST_LEGACY_ARCHIVE_IMPORTED =
    "/hotspot-arcade/history.txt.v1.imported";
static const char* const HA_HIST_INDEX = "/hotspot-arcade/history/index.bin";
static const char* const HA_HIST_INDEX_TEMP = "/hotspot-arcade/history/.index.tmp";
static const char* const HA_HIST_INDEX_OLD = "/hotspot-arcade/history/.index.old";

#define HA_HIST_INDEX_SCHEMA 2
#define HA_HIST_INDEX_HEADER_BYTES 16
#define HA_HIST_INDEX_REBUILD_BATCH 32

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
    uint32_t indexStart;
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
static HaHistSession* haHistVerify = nullptr;
#define haHistActive (*haHistRt.active)

// Active state plus scratch and verification records are allocated once at first
// SD use. This keeps the three session records out of static DRAM without risking
// the loop task's small stack. Every history API runs on loop(), never from
// AsyncTCP; callbacks receive caller-owned records.
static bool haHistStorageBegin() {
    if(haHistRt.active && haHistScratch && haHistVerify && haHistStorage) return true;
    HaHistSession* active = new(std::nothrow) HaHistSession{};
    HaHistSession* scratch = new(std::nothrow) HaHistSession{};
    HaHistSession* verify = new(std::nothrow) HaHistSession{};
    HaHistCatalog* catalog = new(std::nothrow) HaHistCatalog{};
    if(!active || !scratch || !verify || !catalog) {
        delete active;
        delete scratch;
        delete verify;
        delete catalog;
        return false;
    }
    haHistRt.active = active;
    haHistScratch = scratch;
    haHistVerify = verify;
    haHistStorage = catalog;
    return true;
}

static bool haHistStorageReady() {
    return haHistRt.active && haHistScratch && haHistVerify && haHistStorage;
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

// 1 = LF-terminated line, 2 = unterminated final line, 0 = clean EOF,
// -1 = line exceeded the fixed buffer. V2 records require status 1 for every line;
// legacy import tolerates status 2 because the old writer did not promise a final LF.
static int haHistReadLine(File& f, char* out, size_t cap) {
    if(!out || cap < 2) return -1;
    size_t n = 0;
    bool got = false;
    bool overflow = false;
    bool newline = false;
    while(f.available()) {
        int v = f.read();
        if(v < 0) break;
        got = true;
        char c = (char)v;
        if(c == '\n') { newline = true; break; }
        if(c == '\r') continue;
        if(n + 1 < cap) out[n++] = c;
        else overflow = true;
    }
    out[n] = '\0';
    if(overflow) return -1;
    return got ? (newline ? 1 : 2) : 0;
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

static bool haHistValidUtf8(const char* value, size_t capacity, bool allowEmpty) {
    if(!value || !capacity) return false;
    size_t length = strnlen(value, capacity);
    if(length >= capacity || (!allowEmpty && length == 0)) return false;
    const uint8_t* p = (const uint8_t*)value;
    const uint8_t* end = p + length;
    while(p < end) {
        if(*p < 0x20 || *p == 0x7F) return false;
        if(*p < 0x80) { p++; continue; }
        uint8_t needed = 0;
        uint32_t code = 0;
        if((*p & 0xE0) == 0xC0) {
            needed = 1;
            code = *p & 0x1F;
            if(code < 2) return false;
        } else if((*p & 0xF0) == 0xE0) {
            needed = 2;
            code = *p & 0x0F;
        } else if((*p & 0xF8) == 0xF0) {
            needed = 3;
            code = *p & 0x07;
        } else {
            return false;
        }
        p++;
        if((size_t)(end - p) < needed) return false;
        for(uint8_t i = 0; i < needed; i++, p++) {
            if((*p & 0xC0) != 0x80) return false;
            code = (code << 6) | (*p & 0x3F);
        }
        if((needed == 2 && code < 0x800) || (needed == 3 && code < 0x10000) ||
           (code >= 0x80 && code <= 0x9F) ||
           (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
    }
    return true;
}

static bool haHistIdentityValid(const char* identity) {
    if(!identity) return false;
    size_t length = strnlen(identity, HA_CLIENT_ID_LEN);
    if(length == 0) return true; // legacy v1 standings have no browser identity
    if(length != 32 || identity[32] != '\0') return false;
    for(uint8_t i = 0; i < 32; i++) {
        char c = identity[i];
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool haHistKnownGame(uint8_t game, bool allowNone) {
    if(game == HA_GAME_NONE) return allowNone;
    switch(game) {
    case HA_GAME_TRIVIA:
    case HA_GAME_CONNECT4:
    case HA_GAME_TICTACTOE:
    case HA_GAME_DOTS:
    case HA_GAME_DRAW:
    case HA_GAME_PONG:
    case HA_GAME_REACT:
    case HA_GAME_WYR:
    case HA_GAME_SCRAMBLE:
    case HA_GAME_REVERSI:
    case HA_GAME_GUESSCOLOR:
    case HA_GAME_BATTLESHIP:
    case HA_GAME_SPECTRUM:
    case HA_GAME_KMK:
    case HA_GAME_CHESS:
        return true;
    default:
        return false;
    }
}

static bool haHistRecordValid(const HaHistSession& session) {
    if(!session.num || !session.seq || session.count > HA_SESSION_MAX_PLAYERS ||
       session.gameCount > HA_SESSION_GAME_STATS_MAX ||
       !haHistKnownGame(session.game, true) || (session.archived && !session.count))
        return false;
    for(uint8_t i = 0; i < session.count; i++) {
        const HaHistPlayer& player = session.p[i];
        if(!haHistIdentityValid(player.clientId) ||
           !haHistValidUtf8(player.nick, sizeof(player.nick), false) ||
           !haHistValidUtf8(player.avatar, sizeof(player.avatar), true))
            return false;
        if(player.clientId[0]) {
            for(uint8_t prior = 0; prior < i; prior++)
                if(strcmp(player.clientId, session.p[prior].clientId) == 0) return false;
        }
    }
    for(uint8_t i = 0; i < session.gameCount; i++) {
        if(!haHistKnownGame(session.games[i].game, false) || !session.games[i].count)
            return false;
        for(uint8_t prior = 0; prior < i; prior++)
            if(session.games[i].game == session.games[prior].game) return false;
    }
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

static bool haHistSameRecord(const HaHistSession& a, const HaHistSession& b) {
    return a.num == b.num && a.seq == b.seq && a.archived == b.archived &&
           haHistSameState(a, b);
}

static bool haHistArchiveCommitsActive(
    const HaHistSession& archive,
    const HaHistSession& active) {
    if(active.seq == UINT32_MAX) return false;
    uint32_t expectedSeq = active.seq + 1;
    return archive.archived && !active.archived && archive.num == active.num &&
           archive.seq == expectedSeq && haHistSameState(archive, active);
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
            // Pre-v2 engine snapshots have no stable browser identity. Keep the
            // identity empty rather than persisting a PID that changes on resume.
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
    if(!path || !haHistRecordValid(s)) return false;
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
    if(ok) {
        haHistSortGames(s);
        ok = haHistRecordValid(s);
    }
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
    if(!haHistReadRecord(tempPath, *haHistVerify, 1) ||
       haHistVerify->num != expectedNum || haHistVerify->seq != expectedSeq ||
       haHistVerify->restoredFrom != expectedRestoredFrom ||
       haHistVerify->game != expectedGame || haHistVerify->count != expectedCount ||
       haHistVerify->gameCount != expectedGameCount ||
       !haHistSameRecord(source, *haHistVerify)) {
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

struct HaHistIndexMeta {
    uint32_t count;
    uint32_t crc;
};

static void haHistLe16(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void haHistLe32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t haHistFromLe16(const uint8_t in[2]) {
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t haHistFromLe32(const uint8_t in[4]) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static bool haHistReadBytes(File& file, uint8_t* out, size_t length) {
    for(size_t i = 0; i < length; i++) {
        int value = file.read();
        if(value < 0) return false;
        out[i] = (uint8_t)value;
    }
    return true;
}

static bool haHistIndexHeader(File& file, HaHistIndexMeta& meta) {
    uint8_t header[HA_HIST_INDEX_HEADER_BYTES];
    if(!file || file.isDirectory() || !haHistReadBytes(file, header, sizeof(header)) ||
       memcmp(header, "HAI2", 4) != 0 ||
       haHistFromLe16(header + 4) != HA_HIST_INDEX_SCHEMA ||
       haHistFromLe16(header + 6) != HA_HIST_INDEX_HEADER_BYTES)
        return false;
    meta.count = haHistFromLe32(header + 8);
    meta.crc = haHistFromLe32(header + 12);
    uint64_t expectedSize = (uint64_t)HA_HIST_INDEX_HEADER_BYTES +
                            (uint64_t)meta.count * sizeof(uint32_t);
    return expectedSize == file.size();
}

static bool haHistIndexReadId(File& file, uint32_t& id, uint32_t* crc = nullptr) {
    uint8_t encoded[4];
    if(!haHistReadBytes(file, encoded, sizeof(encoded))) return false;
    if(crc) *crc = haHistCrcUpdate(*crc, encoded, sizeof(encoded));
    id = haHistFromLe32(encoded);
    return id != 0;
}

static bool haHistIndexValidate(const char* path, HaHistIndexMeta* output = nullptr) {
    File file = SD.open(path, FILE_READ);
    HaHistIndexMeta meta = {};
    if(!haHistIndexHeader(file, meta)) { if(file) file.close(); return false; }
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t previous = UINT32_MAX;
    for(uint32_t i = 0; i < meta.count; i++) {
        uint32_t id = 0;
        if(!haHistIndexReadId(file, id, &crc) ||
           (i > 0 && id >= previous)) {
            file.close();
            return false;
        }
        previous = id;
    }
    bool ok = !file.available() && (crc ^ 0xFFFFFFFFUL) == meta.crc;
    file.close();
    if(ok && output) *output = meta;
    return ok;
}

static bool haHistIndexWriteHeader(File& file, uint32_t count, uint32_t crc) {
    uint8_t header[HA_HIST_INDEX_HEADER_BYTES] = {'H', 'A', 'I', '2'};
    haHistLe16(header + 4, HA_HIST_INDEX_SCHEMA);
    haHistLe16(header + 6, HA_HIST_INDEX_HEADER_BYTES);
    haHistLe32(header + 8, count);
    haHistLe32(header + 12, crc);
    return file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
}

static void haHistIndexCandidate(
    uint32_t candidates[HA_HIST_INDEX_REBUILD_BATCH],
    uint8_t& count,
    uint32_t id) {
    uint8_t position = 0;
    while(position < count && candidates[position] > id) position++;
    if(position < count && candidates[position] == id) return;
    if(position >= HA_HIST_INDEX_REBUILD_BATCH) return;
    uint8_t end = count < HA_HIST_INDEX_REBUILD_BATCH
                      ? count
                      : HA_HIST_INDEX_REBUILD_BATCH - 1;
    for(int index = end; index > position; index--)
        candidates[index] = candidates[index - 1];
    candidates[position] = id;
    if(count < HA_HIST_INDEX_REBUILD_BATCH) count++;
}

// Rebuild in bounded 32-id batches. This uses O(1) RAM regardless of retained
// history size; rebuilding is slower than normal indexed paging but is needed only
// after migration, archive creation, or a corrupt/missing cache.
static bool haHistIndexRebuild() {
    if(!haSdOk || !haHistStorageReady()) return false;
    SD.remove(HA_HIST_INDEX_TEMP);
    File output = SD.open(HA_HIST_INDEX_TEMP, FILE_WRITE);
    if(!output) return false;
    uint8_t blank[HA_HIST_INDEX_HEADER_BYTES] = {};
    if(output.write(blank, sizeof(blank)) != sizeof(blank)) {
        output.close();
        SD.remove(HA_HIST_INDEX_TEMP);
        return false;
    }

    uint32_t count = 0;
    uint32_t crc = 0xFFFFFFFFUL;
    uint64_t upperExclusive = (uint64_t)UINT32_MAX + 1ULL;
    bool ok = true;
    while(ok) {
        uint32_t candidates[HA_HIST_INDEX_REBUILD_BATCH] = {};
        uint8_t candidateCount = 0;
        File directory = SD.open(HA_HIST_ARCHIVE_DIR, FILE_READ);
        if(!directory || !directory.isDirectory()) {
            if(directory) directory.close();
            ok = false;
            break;
        }
        File entry = directory.openNextFile();
        while(entry) {
            char name[96];
            strlcpy(name, entry.name(), sizeof(name));
            bool isDirectory = entry.isDirectory();
            entry.close();
            uint32_t id = 0;
            if(!isDirectory && haHistParseArchiveName(name, id) && id < upperExclusive)
                haHistIndexCandidate(candidates, candidateCount, id);
            entry = directory.openNextFile();
        }
        directory.close();
        if(!candidateCount) break;

        for(uint8_t i = 0; i < candidateCount; i++) {
            char path[64];
            haHistArchivePath(candidates[i], path, sizeof(path));
            if(!haHistReadRecord(path, *haHistScratch, 1) ||
               haHistScratch->num != candidates[i])
                continue; // corrupt archives are retained but omitted from the cache
            uint8_t encoded[4];
            haHistLe32(encoded, candidates[i]);
            if(count == UINT32_MAX || output.write(encoded, sizeof(encoded)) != sizeof(encoded)) {
                ok = false;
                break;
            }
            crc = haHistCrcUpdate(crc, encoded, sizeof(encoded));
            count++;
        }
        upperExclusive = candidates[candidateCount - 1];
    }

    if(ok) ok = haHistIndexWriteHeader(output, count, crc ^ 0xFFFFFFFFUL);
    output.flush();
    output.close();
    if(!ok || !haHistIndexValidate(HA_HIST_INDEX_TEMP)) {
        SD.remove(HA_HIST_INDEX_TEMP);
        return false;
    }

    SD.remove(HA_HIST_INDEX_OLD);
    bool hadIndex = SD.exists(HA_HIST_INDEX);
    if(hadIndex && !SD.rename(HA_HIST_INDEX, HA_HIST_INDEX_OLD)) {
        SD.remove(HA_HIST_INDEX_TEMP);
        return false;
    }
    if(!SD.rename(HA_HIST_INDEX_TEMP, HA_HIST_INDEX)) {
        if(hadIndex) SD.rename(HA_HIST_INDEX_OLD, HA_HIST_INDEX);
        SD.remove(HA_HIST_INDEX_TEMP);
        return false;
    }
    if(!haHistIndexValidate(HA_HIST_INDEX)) {
        SD.remove(HA_HIST_INDEX);
        if(hadIndex) SD.rename(HA_HIST_INDEX_OLD, HA_HIST_INDEX);
        return false;
    }
    SD.remove(HA_HIST_INDEX_OLD);
    return true;
}

static bool haHistIndexEnsure() {
    if(haHistIndexValidate(HA_HIST_INDEX)) {
        SD.remove(HA_HIST_INDEX_TEMP);
        SD.remove(HA_HIST_INDEX_OLD);
        return true;
    }
    return haHistIndexRebuild();
}

static bool haHistIndexIdAt(uint32_t ordinal, uint32_t& id) {
    File file = SD.open(HA_HIST_INDEX, FILE_READ);
    HaHistIndexMeta meta = {};
    if(!haHistIndexHeader(file, meta) || ordinal >= meta.count ||
       !file.seek(HA_HIST_INDEX_HEADER_BYTES + ordinal * sizeof(uint32_t)) ||
       !haHistIndexReadId(file, id)) {
        if(file) file.close();
        return false;
    }
    file.close();
    return true;
}

static bool haHistRefreshStats() {
    haHist.total = 0;
    haHist.minNum = 0;
    haHist.maxNum = 0;
    if(!haSdOk || !haHistIndexEnsure()) return false;
    HaHistIndexMeta meta = {};
    if(!haHistIndexValidate(HA_HIST_INDEX, &meta)) return false;
    haHist.total = meta.count;
    if(meta.count) {
        if(!haHistIndexIdAt(0, haHist.maxNum) ||
           !haHistIndexIdAt(meta.count - 1, haHist.minNum)) {
            haHist.total = 0;
            haHist.minNum = 0;
            haHist.maxNum = 0;
            return false;
        }
    }
    return true;
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
        bool stored = p.putUInt("n", next) == sizeof(next);
        p.end();
        return stored ? next : 0;
    }
    return high == UINT32_MAX ? 0 : high + 1;
}

static bool haHistParseLegacyPlayer(char* line, HaHistPlayer& p) {
    char* tab = strchr(line, '\t');
    if(!tab) return false;
    *tab++ = '\0';
    p = HaHistPlayer{};
    return haHistParseI32(line, p.score) && tab[0] &&
           strlcpy(p.nick, tab, sizeof(p.nick)) < sizeof(p.nick) &&
           haHistValidUtf8(p.nick, sizeof(p.nick), false);
}

static char* haHistLegacyToken(char*& cursor) {
    while(*cursor == ' ') cursor++;
    if(!*cursor) return nullptr;
    char* token = cursor;
    while(*cursor && *cursor != ' ') cursor++;
    if(*cursor) *cursor++ = '\0';
    return token;
}

static bool haHistParseLegacySessionHeader(
    char* line,
    uint32_t& num,
    uint32_t& count,
    bool& haveCount) {
    if(strncmp(line, "SESSION ", 8) != 0) return false;
    char* cursor = line + 8;
    char* numberText = haHistLegacyToken(cursor);
    char* countText = haHistLegacyToken(cursor);
    char* extra = haHistLegacyToken(cursor);
    haveCount = countText != nullptr;
    count = 0;
    return numberText && !extra && haHistParseU32(numberText, num) && num != 0 &&
           (!haveCount || (haHistParseU32(countText, count) &&
                           count <= HA_SESSION_MAX_PLAYERS));
}

static bool haHistParseLegacyCurrentHeader(char* line, uint32_t& count) {
    if(strncmp(line, "CURRENT ", 8) != 0) return false;
    char* cursor = line + 8;
    char* countText = haHistLegacyToken(cursor);
    return countText && !haHistLegacyToken(cursor) &&
           haHistParseU32(countText, count) && count <= HA_SESSION_MAX_PLAYERS;
}

static bool haHistFileFingerprint(
    const char* path,
    size_t& outputSize,
    uint32_t& outputCrc) {
    File file = SD.open(path, FILE_READ);
    if(!file || file.isDirectory()) {
        if(file) file.close();
        return false;
    }
    outputSize = file.size();
    size_t read = 0;
    uint32_t crc = 0xFFFFFFFFUL;
    while(file.available()) {
        int value = file.read();
        if(value < 0) { file.close(); return false; }
        uint8_t byte = (uint8_t)value;
        crc = haHistCrcUpdate(crc, &byte, 1);
        read++;
    }
    file.close();
    outputCrc = crc ^ 0xFFFFFFFFUL;
    return read == outputSize;
}

static bool haHistPromoteLegacy(const char* source, const char* imported) {
    if(!SD.exists(source)) return true;
    // Never overwrite an earlier preserved original. Seeing both names requires
    // operator attention rather than guessing which legacy file is authoritative.
    if(SD.exists(imported)) return false;
    size_t sourceSize = 0;
    uint32_t sourceCrc = 0;
    if(!haHistFileFingerprint(source, sourceSize, sourceCrc) ||
       !SD.rename(source, imported))
        return false;
    size_t importedSize = 0;
    uint32_t importedCrc = 0;
    bool verified = !SD.exists(source) &&
                    haHistFileFingerprint(imported, importedSize, importedCrc) &&
                    importedSize == sourceSize && importedCrc == sourceCrc;
    if(!verified && !SD.exists(source)) SD.rename(imported, source);
    return verified;
}

static bool haHistCommitLegacyArchive(HaHistSession& session) {
    if(!session.num) return true;
    if(!session.count) return false;
    session.archived = true;
    session.restoredFrom = 0;
    session.game = HA_GAME_NONE;
    session.gameCount = 0;
    session.seq = session.num;
    haHistSort(session);
    if(!haHistRecordValid(session)) return false;
    char path[64];
    haHistArchivePath(session.num, path, sizeof(path));
    if(SD.exists(path)) {
        return haHistReadRecord(path, *haHistVerify, 1) &&
               haHistSameRecord(session, *haHistVerify);
    }
    return haHistWriteArchive(session);
}

static bool haHistMigrateLegacyArchive(bool& wroteArchive) {
    wroteArchive = false;
    if(!SD.exists(HA_HIST_LEGACY_ARCHIVE)) return true;
    File f = SD.open(HA_HIST_LEGACY_ARCHIVE, FILE_READ);
    if(!f) return false;
    HaHistSession& cur = haHistActive; // scratch remains available for verified reads
    cur = HaHistSession{};
    cur.archived = true;
    char line[HA_HIST_LINE_MAX];
    int lr = 0;
    bool ok = true;
    uint32_t declaredCount = 0;
    bool haveDeclaredCount = false;
    while((lr = haHistReadLine(f, line, sizeof(line))) == 1 || lr == 2) {
        bool finalLine = lr == 2;
        if(!line[0]) {
            if(finalLine) break;
            continue;
        }
        if(strncmp(line, "SESSION ", 8) == 0) {
            if(cur.num) {
                if((haveDeclaredCount && declaredCount != cur.count) ||
                   !haHistCommitLegacyArchive(cur)) {
                    ok = false;
                    break;
                }
                wroteArchive = true;
            }
            cur = HaHistSession{};
            cur.archived = true;
            uint32_t num = 0, count = 0;
            bool parsedCount = false;
            if(!haHistParseLegacySessionHeader(line, num, count, parsedCount)) {
                ok = false;
                break;
            }
            cur.num = num;
            cur.seq = cur.num;
            declaredCount = count;
            haveDeclaredCount = parsedCount;
        } else if(cur.num && cur.count < HA_SESSION_MAX_PLAYERS) {
            HaHistPlayer p;
            if(!haHistParseLegacyPlayer(line, p)) {
                ok = false;
                break;
            }
            cur.p[cur.count++] = p;
        } else {
            ok = false;
            break;
        }
        if(finalLine) break;
    }
    if(lr < 0) ok = false;
    if(ok && cur.num) {
        if((haveDeclaredCount && declaredCount != cur.count) ||
           !haHistCommitLegacyArchive(cur))
            ok = false;
        else
            wroteArchive = true;
    }
    f.close();
    if(!ok) return false;
    // Make the rebuildable cache reflect every verified immutable destination
    // before preserving the source name. If power fails here, history.txt remains
    // available and the whole transaction is safe to replay.
    if(wroteArchive && (!haHistIndexRebuild() || !haHistRefreshStats())) return false;
    return haHistPromoteLegacy(
        HA_HIST_LEGACY_ARCHIVE,
        HA_HIST_LEGACY_ARCHIVE_IMPORTED);
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
    uint32_t declared = 0;
    bool ok = lr == 1 && haHistParseLegacyCurrentHeader(line, declared);
    out = HaHistSession{};
    while(ok && ((lr = haHistReadLine(f, line, sizeof(line))) == 1 || lr == 2)) {
        bool finalLine = lr == 2;
        if(!line[0]) continue;
        if(out.count >= HA_SESSION_MAX_PLAYERS) { ok = false; break; }
        HaHistPlayer p;
        if(!haHistParseLegacyPlayer(line, p)) { ok = false; break; }
        out.p[out.count++] = p;
        if(finalLine) break;
    }
    if(lr < 0) ok = false;
    f.close();
    if(!ok || !out.count || out.count != declared) return false;
    haHistSort(out);
    return true;
}

static bool haHistSeqNewer(uint32_t a, uint32_t b) {
    // Durable generations never wrap: reusing a number could make an older A/B
    // slot win after an interrupted write. Exhaustion is therefore a hard error.
    return a > b;
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
    if(!haHistReadRecord(path, *haHistVerify, 0) ||
       haHistVerify->num != expectedNum || haHistVerify->seq != expectedSeq ||
       haHistVerify->restoredFrom != expectedRestoredFrom ||
       haHistVerify->game != expectedGame || haHistVerify->count != expectedCount ||
       haHistVerify->gameCount != expectedGameCount ||
       !haHistSameRecord(source, *haHistVerify))
        return false;
    haHistActive = *haHistVerify;
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
    if(!haHistRefreshStats()) return false;
    bool migratedArchive = false;
    if(!haHistMigrateLegacyArchive(migratedArchive)) return false;
    (void)migratedArchive; // migration installs and verifies its index before rename

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
        bool alreadyArchived =
            haHistReadRecord(archivedPath, *haHistScratch, 1) &&
            haHistArchiveCommitsActive(*haHistScratch, haHistActive);
        if(alreadyArchived) recoveryBaseSeq = haHistScratch->seq;
        haHistRt.resumeAvailable = haHistActive.count > 0 && !alreadyArchived;
        // The immutable rename may have landed just before power loss, leaving a
        // structurally valid but stale cache. Rebuild before suppressing active A/B.
        if(alreadyArchived && (!haHistIndexRebuild() || !haHistRefreshStats()))
            return false;

        // If power failed after importing current.txt into a verified active slot
        // but before preserving the source name, finish that exact transaction now.
        if(SD.exists(HA_HIST_LEGACY_CURRENT)) {
            if(!haHistReadLegacyCurrent(*haHistVerify) ||
               !haHistSameState(*haHistVerify, haHistActive) ||
               !haHistPromoteLegacy(
                   HA_HIST_LEGACY_CURRENT,
                   HA_HIST_LEGACY_CURRENT_IMPORTED))
                return false;
        }
        if(!alreadyArchived) {
            haHistRt.begun = true;
            return true; // an empty active session is still current
        }
    }
    bool importedCurrent = false;
    *haHistScratch = HaHistSession{};
    if(!haveA && !haveB && SD.exists(HA_HIST_LEGACY_CURRENT)) {
        if(!haHistReadLegacyCurrent(*haHistScratch)) return false;
        importedCurrent = true;
    }
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    uint32_t priorSeq = recoveryBaseSeq ? recoveryBaseSeq : haHistActive.seq;
    if((haveA || haveB) && priorSeq == UINT32_MAX) return false;
    haHistScratch->seq = (haveA || haveB) ? priorSeq + 1 : 1;
    haHistScratch->archived = false;
    if(!haHistWriteActive(*haHistScratch)) return false;
    if(importedCurrent && !haHistPromoteLegacy(
                              HA_HIST_LEGACY_CURRENT,
                              HA_HIST_LEGACY_CURRENT_IMPORTED))
        return false;
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
    haHistScratch->archived = false;
    if(!force && haHistActive.num == haHistScratch->num &&
       haHistSameState(haHistActive, *haHistScratch))
        return true;
    if(haHistActive.seq == UINT32_MAX) return false;
    haHistScratch->seq = haHistActive.seq + 1;
    return haHistWriteActive(*haHistScratch);
}

static bool haHistCheckpoint(const HaHost& host, bool force = false) {
    if(!haHistBegin()) return false;
    return haHistCheckpointPrepared(host, force, haHistActive.restoredFrom);
}

// Use after importing an archived record into a freshly-created active session.
// This records provenance without making it part of the mutable HaHost mirror.
[[maybe_unused]] static bool haHistCheckpointRestored(
    const HaHost& host,
    uint32_t sourceNum) {
    if(!sourceNum || !haHistBegin()) return false;
    return haHistCheckpointPrepared(host, true, sourceNum);
}

// Restoring immutable history always creates a distinct active session. This is
// intentionally different from haHistCheckpointRestored(), which updates the
// current session and remains available for migration compatibility. Reserving a
// new number matters when the current active session is empty: there is nothing
// to archive, but a restore must still have its own identity and provenance.
static bool haHistStartRestoredActive(const HaHost& host, uint32_t sourceNum) {
    if(!sourceNum || !haHistBegin() || haHistActive.seq == UINT32_MAX) return false;
    haHistFromHost(host, *haHistScratch);
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    haHistScratch->seq = haHistActive.seq + 1;
    haHistScratch->restoredFrom = sourceNum;
    haHistScratch->archived = false;
    return haHistWriteActive(*haHistScratch);
}

// Explicit discard path used only after the host confirms a second warning when
// immutable archiving failed. The prior valid A/B slot remains recoverable until
// this separately numbered empty active session is fully written and verified.
[[maybe_unused]] static bool haHistStartNewActive(const HaHost& host) {
    if(!haHistBegin() || haHistActive.seq == UINT32_MAX) return false;
    haHistFromHost(host, *haHistScratch);
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    haHistScratch->seq = haHistActive.seq + 1;
    haHistScratch->restoredFrom = 0;
    haHistScratch->archived = false;
    return haHistWriteActive(*haHistScratch);
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
    // Checkpoint, immutable archive, and replacement active slot may each consume
    // a generation. Fail before writing anything if all three cannot be unique.
    if(haHistActive.seq > UINT32_MAX - 3U) return false;
    // A prior call may already have committed the immutable archive and failed only
    // while rebuilding the cache or advancing active A/B. Do not create another
    // active generation in that case: exact equality below proves the retry instead.
    uint32_t currentNum = haHistActive.num ? haHistActive.num : haHistReserveNum();
    if(!currentNum) return false;
    char finalPath[64];
    haHistArchivePath(currentNum, finalPath, sizeof(finalPath));
    bool recoveringArchive = SD.exists(finalPath);
    if(!recoveringArchive) {
        // Make active A/B exactly match the standings about to be archived. If power
        // fails after rename, boot can prove equality before suppressing it.
        if(!haHistCheckpoint(host, true)) return false;
        currentNum = haHistActive.num;
        haHistArchivePath(currentNum, finalPath, sizeof(finalPath));
    }
    uint32_t finishedSeq = 0;
    uint32_t expectedSeq = haHistActive.seq + 1;
    haHistFromHost(host, *haHistScratch);
    if(!haHistScratch->count) return false;
    haHistScratch->restoredFrom = haHistActive.restoredFrom;
    haHistScratch->num = currentNum;
    haHistScratch->seq = expectedSeq;
    haHistScratch->archived = true;
    if(recoveringArchive) {
        // Recovery for a prior call that committed the immutable archive but lost
        // power (or an SD write) while advancing the active slot.
        if(!haHistReadRecord(finalPath, *haHistVerify, 1) ||
           !haHistSameRecord(*haHistScratch, *haHistVerify))
            return false;
        finishedSeq = haHistVerify->seq;
    } else {
        if(!haHistWriteArchive(*haHistScratch)) return false;
        finishedSeq = haHistScratch->seq;
    }
    if(!haHistIndexRebuild() || !haHistRefreshStats()) return false;

    *haHistScratch = HaHistSession{};
    haHistScratch->num = haHistReserveNum();
    if(!haHistScratch->num) return false;
    if(finishedSeq == UINT32_MAX) return false;
    haHistScratch->seq = finishedSeq + 1;
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

static bool haHistCatalogLoadPage(uint32_t start, bool allowRebuild = true) {
    if(!haHistBegin()) return false;
    haHist.count = 0;
    haHist.hasNewer = false;
    haHist.hasOlder = false;
    haHist.indexStart = start;
    if(start >= haHist.total) return false;

    File index = SD.open(HA_HIST_INDEX, FILE_READ);
    HaHistIndexMeta meta = {};
    bool ok = haHistIndexHeader(index, meta) && meta.count == haHist.total &&
              index.seek(HA_HIST_INDEX_HEADER_BYTES +
                         (size_t)start * sizeof(uint32_t));
    while(ok && haHist.count < HA_HIST_PAGE_MAX &&
          start + haHist.count < meta.count) {
        uint32_t id = 0;
        if(!haHistIndexReadId(index, id)) { ok = false; break; }
        char path[64];
        haHistArchivePath(id, path, sizeof(path));
        if(!haHistReadRecord(path, *haHistScratch, 1) || haHistScratch->num != id) {
            ok = false;
            break;
        }
        haHist.s[haHist.count++] = haHistSummary(*haHistScratch);
    }
    if(index) index.close();
    if(!ok) {
        haHist.count = 0;
        if(!allowRebuild || !haHistIndexRebuild() || !haHistRefreshStats()) return false;
        uint32_t retryStart = start < haHist.total ? start : 0;
        return haHistCatalogLoadPage(retryStart, false);
    }
    haHist.hasNewer = start > 0;
    haHist.hasOlder = start + haHist.count < haHist.total;
    return haHist.count != 0;
}

static bool haHistCatalogNewest() {
    if(!haHistBegin()) return false;
    return haHistCatalogLoadPage(0);
}

static bool haHistCatalogOlder() {
    if(!haHistBegin()) return false;
    if(!haHist.count || !haHist.hasOlder) return false;
    return haHistCatalogLoadPage(haHist.indexStart + haHist.count);
}

static bool haHistCatalogNewer() {
    if(!haHistBegin()) return false;
    if(!haHist.count || !haHist.hasNewer) return false;
    uint32_t start = haHist.indexStart > HA_HIST_PAGE_MAX
                         ? haHist.indexStart - HA_HIST_PAGE_MAX
                         : 0;
    return haHistCatalogLoadPage(start);
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
