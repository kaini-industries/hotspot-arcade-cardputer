// Host-side mirror of the session, for the Cardputer's own screen.
//
// The engine owns a small, connection-shaped roster indexed by its transient pid.
// The Cardputer additionally owns a session ledger: scores in that ledger survive a
// socket disconnect and are archived as one cumulative host session. A future phone
// protocol can pass its durable client id to haHostJoinStable(); the current engine
// remains compatible through haHostJoin(), which cautiously reconnects a single
// disconnected player with the same nickname.
//
// Every writer below runs under the .ino's ENGINE_LOCK. Readers must copy the whole
// HaHost under that same lock via haHostSnapshot(); no field is safe to poll directly
// from loop() while AsyncTCP can be writing it.
#pragma once
#include <Arduino.h>
#include <limits.h>
#include <new>
#include "ha_games.h"

#define HA_EV_MAX 24 // console scrollback
#define HA_EV_LEN 44 // fits the 240px screen at the 6px font

// 128-bit random phone ids are rendered as 32 hexadecimal bytes. Keep the buffer
// overridable so a generated protocol can raise the bound without editing this file.
#ifndef HA_CLIENT_ID_LEN
#define HA_CLIENT_ID_LEN 33
#endif

// Matches the engine Player avatar buffer (one UTF-8 emoji plus terminator).
// Kept overridable so the durable-session layer can follow a future protocol bump.
#ifndef HA_AVATAR_LEN
#define HA_AVATAR_LEN 8
#endif

// More people can take turns in one session than can be connected concurrently.
// This is deliberately independent from HA_MAX_PLAYERS (the engine/AP capacity).
#ifndef HA_SESSION_MAX_PLAYERS
#define HA_SESSION_MAX_PLAYERS 32
#endif

// Game ids are sparse protocol values, so a session stores only games that were
// actually launched instead of indexing an array by the largest possible id.
#ifndef HA_SESSION_GAME_STATS_MAX
#define HA_SESSION_GAME_STATS_MAX 32
#endif

#define HA_SESSION_INDEX_NONE 0xFF
static_assert(HA_SESSION_MAX_PLAYERS <= HA_SESSION_INDEX_NONE, "session index must fit uint8_t");
static_assert(HA_CLIENT_ID_LEN >= 33, "client id buffer must hold a 128-bit hex identity");
static_assert(HA_AVATAR_LEN >= 2, "avatar buffer must include a terminator");
static_assert(HA_SESSION_GAME_STATS_MAX > 0 && HA_SESSION_GAME_STATS_MAX <= UINT8_MAX,
              "game stat count must fit uint8_t");

struct HaHostPlayer {
    bool used; // currently occupies this engine pid
    char nick[HA_NICK_LEN];
    int32_t score; // cumulative host-session score, not the engine's per-game score
    uint8_t sessionIndex; // HA_SESSION_INDEX_NONE if the bounded ledger is full
};

struct HaHostSessionPlayer {
    bool used;
    bool connected;
    uint8_t pid; // 0 while disconnected
    char clientId[HA_CLIENT_ID_LEN]; // empty until the stable-id protocol is available
    char avatar[HA_AVATAR_LEN];
    char nick[HA_NICK_LEN];
    int32_t score;
};

struct HaHostGamePlay {
    uint8_t game;
    uint16_t count;
};

struct HaHost {
    HaHostPlayer p[HA_MAX_PLAYERS + 1]; // 1-based, matching engine pids
    HaHostSessionPlayer session[HA_SESSION_MAX_PLAYERS];
    uint8_t sessionCount;
    HaHostGamePlay games[HA_SESSION_GAME_STATS_MAX];
    uint8_t gameCount;
    char ev[HA_EV_MAX][HA_EV_LEN];
    uint32_t evTotal; // events ever logged; ring slot = evTotal % HA_EV_MAX
    char lastEvent[HA_EV_LEN];
    uint8_t activeGame;
    bool portalRunning;
    uint32_t rev; // bumped on every change; read only from a locked snapshot
};

// HaHost is several kilobytes at the default 32-person session capacity. Allocate
// it once from the general heap instead of permanently consuming scarce static
// DRAM; setup's first haHostReset() initializes it before the portal can run.
static HaHost* haHostStorage = nullptr;
#define haHost (*haHostStorage)

static inline bool haHostBegin() {
    if(haHostStorage) return true;
    haHostStorage = new(std::nothrow) HaHost{};
    return haHostStorage != nullptr;
}

static inline void haHostTouch() {
    if(!haHostStorage) return;
    haHost.rev++;
}

static inline void haHostLog(const char* s) {
    if(!haHostStorage) return;
    const char* safe = s ? s : "";
    strlcpy(haHost.ev[haHost.evTotal % HA_EV_MAX], safe, HA_EV_LEN);
    haHost.evTotal++;
    haHostTouch();
}

static inline void haHostClearPid(uint8_t pid) {
    if(!haHostStorage || pid > HA_MAX_PLAYERS) return;
    haHost.p[pid] = HaHostPlayer{};
    haHost.p[pid].sessionIndex = HA_SESSION_INDEX_NONE;
}

// End the current host session completely. AP restarts should instead call
// haHostSuspendConnections(), so the cumulative ledger remains available.
static inline void haHostReset() {
    if(!haHostBegin()) return;
    for(uint8_t i = 0; i <= HA_MAX_PLAYERS; i++) haHostClearPid(i);
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        haHost.session[i] = HaHostSessionPlayer{};
    haHost.sessionCount = 0;
    for(uint8_t i = 0; i < HA_SESSION_GAME_STATS_MAX; i++)
        haHost.games[i] = HaHostGamePlay{};
    haHost.gameCount = 0;
    haHost.lastEvent[0] = '\0';
    haHost.activeGame = HA_GAME_NONE;
    haHostTouch();
}

// Drop transient pid mappings while retaining the cumulative session ledger. This
// is the mirror-side operation for a planned AP restart or temporary network loss.
static inline void haHostSuspendConnections() {
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) haHostClearPid(pid);
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++) {
        if(!haHost.session[i].used) continue;
        haHost.session[i].connected = false;
        haHost.session[i].pid = 0;
    }
    haHostTouch();
}

static inline int haHostFindClient(const char* clientId) {
    if(!clientId || !clientId[0]) return -1;
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        if(haHost.session[i].used &&
           strcmp(haHost.session[i].clientId, clientId) == 0)
            return i;
    return -1;
}

static inline bool haHostClientIdStable(const char* clientId) {
    if(!clientId || strnlen(clientId, HA_CLIENT_ID_LEN) != 32) return false;
    for(uint8_t i = 0; i < 32; i++) {
        char c = clientId[i];
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Compatibility fallback for the current protocol. Only an unambiguous,
// disconnected legacy nickname is reused; duplicate names never silently merge
// scores, and a row already owned by a stable identity is never claimed by name.
static inline int haHostFindDisconnectedNick(
    const char* nick,
    bool requireLegacyClientId = false) {
    if(!nick || !nick[0]) return -1;
    int found = -1;
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++) {
        const HaHostSessionPlayer& p = haHost.session[i];
        if(!p.used || p.connected ||
           (requireLegacyClientId && haHostClientIdStable(p.clientId)) ||
           strcmp(p.nick, nick) != 0)
            continue;
        if(found >= 0) return -1;
        found = i;
    }
    return found;
}

static inline int haHostAllocSessionPlayer() {
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++) {
        if(haHost.session[i].used) continue;
        haHost.session[i] = HaHostSessionPlayer{};
        haHost.session[i].used = true;
        haHost.sessionCount++;
        return i;
    }
    return -1;
}

// Stable-id entry point for the next protocol. A re-hello on an occupied pid is a
// rename; a new pid first seeks its durable client id, then the safe legacy-nick
// fallback, then creates a ledger entry. Returns true for a new connection (so the
// caller can jingle) and false for an in-place rename or invalid pid.
static inline bool haHostJoinStable(
    uint8_t pid,
    const char* clientId,
    const char* nick,
    const char* avatar) {
    if(pid < 1 || pid > HA_MAX_PLAYERS) return false;
    const char* durableId = haHostClientIdStable(clientId) ? clientId : nullptr;
    const char* safeNick = (nick && nick[0]) ? nick : "PLAYER";
    bool isConnection = !haHost.p[pid].used;
    int si = isConnection ? -1 : haHost.p[pid].sessionIndex;
    bool stableCollision = false;

    if(isConnection) {
        si = haHostFindClient(durableId);
        // Refuse to alias one durable identity to two simultaneous pids.
        if(si >= 0 && haHost.session[si].connected) {
            si = -1;
            stableCollision = true;
        }
        if(si < 0 && !stableCollision)
            si = haHostFindDisconnectedNick(
                safeNick,
                true); // nickname fallback is only for pre-identity ledger rows
        if(si < 0 && !stableCollision) si = haHostAllocSessionPlayer();
    }

    int32_t cumulative = 0;
    if(si >= 0 && si < HA_SESSION_MAX_PLAYERS && haHost.session[si].used) {
        HaHostSessionPlayer& sp = haHost.session[si];
        if(durableId && !haHostClientIdStable(sp.clientId))
            strlcpy(sp.clientId, durableId, sizeof(sp.clientId));
        if(avatar && avatar[0]) strlcpy(sp.avatar, avatar, sizeof(sp.avatar));
        strlcpy(sp.nick, safeNick, sizeof(sp.nick));
        sp.connected = true;
        sp.pid = pid;
        cumulative = sp.score;
    } else {
        si = HA_SESSION_INDEX_NONE;
    }

    HaHostPlayer& live = haHost.p[pid];
    live.used = true;
    live.sessionIndex = (uint8_t)si;
    live.score = cumulative;
    strlcpy(live.nick, safeNick, sizeof(live.nick));

    char line[HA_EV_LEN];
    snprintf(line, sizeof(line), "%s %s", isConnection ? "JOIN" : "NAME", safeNick);
    haHostLog(line);
    return isConnection;
}

static inline bool haHostJoinStable(uint8_t pid, const char* clientId, const char* nick) {
    return haHostJoinStable(pid, clientId, nick, nullptr);
}

// Current-engine compatibility entry point.
static inline bool haHostJoin(uint8_t pid, const char* nick) {
    return haHostJoinStable(pid, nullptr, nick, nullptr);
}

static inline void haHostLeave(uint8_t pid) {
    if(pid < 1 || pid > HA_MAX_PLAYERS || !haHost.p[pid].used) return;
    char line[HA_EV_LEN];
    snprintf(line, sizeof(line), "LEAVE %s", haHost.p[pid].nick);
    uint8_t si = haHost.p[pid].sessionIndex;
    if(si < HA_SESSION_MAX_PLAYERS && haHost.session[si].used) {
        haHost.session[si].connected = false;
        haHost.session[si].pid = 0;
    }
    haHostClearPid(pid);
    haHostLog(line);
}

static inline void haHostScore(uint8_t pid, int delta) {
    if(pid < 1 || pid > HA_MAX_PLAYERS || !haHost.p[pid].used) return;
    HaHostPlayer& live = haHost.p[pid];
    int64_t sum = (int64_t)live.score + delta;
    if(sum > INT32_MAX) sum = INT32_MAX;
    if(sum < INT32_MIN) sum = INT32_MIN;
    live.score = (int32_t)sum;
    uint8_t si = live.sessionIndex;
    if(si < HA_SESSION_MAX_PLAYERS && haHost.session[si].used)
        haHost.session[si].score = live.score;
    haHostTouch();
}

// Mirror half of starting a new scoring session. The .ino must call this beside
// engine.resetScores() (under ENGINE_LOCK) instead of zeroing only haHost.p[].
static inline void haHostResetSessionScores() {
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        if(haHost.session[i].used) haHost.session[i].score = 0;
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
        if(haHost.p[pid].used) haHost.p[pid].score = 0;
    for(uint8_t i = 0; i < HA_SESSION_GAME_STATS_MAX; i++)
        haHost.games[i] = HaHostGamePlay{};
    haHost.gameCount = 0;
    haHostTouch();
}

static inline int haHostFindGamePlay(const HaHost& host, uint8_t game) {
    if(game == HA_GAME_NONE) return -1;
    for(uint8_t i = 0; i < host.gameCount && i < HA_SESSION_GAME_STATS_MAX; i++)
        if(host.games[i].game == game) return i;
    return -1;
}

static inline uint16_t haHostGamePlayCount(const HaHost& host, uint8_t game) {
    int index = haHostFindGamePlay(host, game);
    return index >= 0 ? host.games[index].count : 0;
}

// Call once from haHostSelectGame(), under ENGINE_LOCK, whenever a non-lobby game
// is launched. Counts saturate instead of wrapping and persist with the session.
static inline bool haHostGamePlayed(uint8_t game) {
    if(!haHostStorage || game == HA_GAME_NONE) return false;
    int index = haHostFindGamePlay(haHost, game);
    if(index < 0) {
        if(haHost.gameCount >= HA_SESSION_GAME_STATS_MAX) return false;
        index = haHost.gameCount++;
        haHost.games[index] = HaHostGamePlay{game, 0};
    }
    if(haHost.games[index].count != UINT16_MAX) haHost.games[index].count++;
    haHostTouch();
    return true;
}

// Import helpers used by a history-restore integration. They intentionally do not
// touch the engine; the .ino owns the surrounding lock and engine lifecycle.
static inline void haHostImportSessionBegin() {
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) haHostClearPid(pid);
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        haHost.session[i] = HaHostSessionPlayer{};
    haHost.sessionCount = 0;
    for(uint8_t i = 0; i < HA_SESSION_GAME_STATS_MAX; i++)
        haHost.games[i] = HaHostGamePlay{};
    haHost.gameCount = 0;
    haHost.activeGame = HA_GAME_NONE;
    haHostTouch();
}

static inline bool haHostImportSessionPlayer(
    const char* clientId,
    const char* nick,
    int32_t score,
    const char* avatar) {
    int si = haHostAllocSessionPlayer();
    if(si < 0) return false;
    HaHostSessionPlayer& p = haHost.session[si];
    if(clientId) strlcpy(p.clientId, clientId, sizeof(p.clientId));
    if(avatar) strlcpy(p.avatar, avatar, sizeof(p.avatar));
    strlcpy(p.nick, (nick && nick[0]) ? nick : "PLAYER", sizeof(p.nick));
    p.score = score;
    p.connected = false;
    p.pid = 0;
    haHostTouch();
    return true;
}

static inline bool haHostImportSessionPlayer(
    const char* clientId,
    const char* nick,
    int32_t score) {
    return haHostImportSessionPlayer(clientId, nick, score, nullptr);
}

static inline bool haHostImportSessionGamePlay(uint8_t game, uint16_t count) {
    if(!haHostStorage || game == HA_GAME_NONE || !count) return false;
    int index = haHostFindGamePlay(haHost, game);
    if(index < 0) {
        if(haHost.gameCount >= HA_SESSION_GAME_STATS_MAX) return false;
        index = haHost.gameCount++;
    }
    haHost.games[index] = HaHostGamePlay{game, count};
    haHostTouch();
    return true;
}

static inline void haHostImportSessionGame(uint8_t game) {
    haHost.activeGame = game;
    haHostTouch();
}

static inline void haHostSetEvent(const char* s) {
    strlcpy(haHost.lastEvent, s ? s : "", HA_EV_LEN);
    haHostLog(s);
}

static inline int haHostPlayerCount() {
    int n = 0;
    for(uint8_t i = 1; i <= HA_MAX_PLAYERS; i++)
        if(haHost.p[i].used) n++;
    return n;
}

static inline int haHostSessionPlayerCount() {
    return haHost.sessionCount;
}
