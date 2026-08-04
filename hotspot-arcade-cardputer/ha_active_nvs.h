// Bounded active-session fallback for boards running without a usable microSD card.
//
// This deliberately stores less than HaHost: only durable identity digests, display
// names/avatars, cumulative scores, sparse game-play counts, and the metadata needed
// to identify an active session. Socket ids, connection state, event logs, raw browser
// resume tokens, and per-game live state never enter NVS.
//
// Records alternate between two independently CRC-checked slots. Each slot is split
// into conservative 1 KiB Preferences blobs so the 32-participant maximum remains
// safe for ESP NVS implementations without relying on one oversized blob write.
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <limits.h>
#include <new>
#include <stddef.h>
#include "ha_metadata.h"

#ifndef HA_CLIENT_ID_LEN
#define HA_CLIENT_ID_LEN 33
#endif
#ifndef HA_NICK_LEN
#define HA_NICK_LEN 20
#endif
#ifndef HA_AVATAR_LEN
#define HA_AVATAR_LEN 8
#endif

#define HA_ACTIVE_NVS_SCHEMA 1
#define HA_ACTIVE_NVS_MAX_PLAYERS 32
#define HA_ACTIVE_NVS_MAX_GAME_COUNTS 32
#define HA_ACTIVE_NVS_CHUNK_BYTES 1024
#define HA_ACTIVE_NVS_MIN_CHECKPOINT_MS 30000UL

static_assert(HA_CLIENT_ID_LEN >= 33, "active fallback requires a 128-bit hex identity");
static_assert(HA_ACTIVE_NVS_MAX_PLAYERS == 32, "roadmap active-session capacity is 32");
static_assert(HA_ACTIVE_NVS_MAX_GAME_COUNTS <= UINT8_MAX, "game count must fit uint8_t");

static const char* const HA_ACTIVE_NVS_NAMESPACE = "ha_active2";
static const uint32_t HA_ACTIVE_NVS_MAGIC = 0x314E4148UL; // little-endian "HAN1"

struct HaActiveNvsPlayer {
    char identity[HA_CLIENT_ID_LEN]; // SHA-256-derived 128-bit id, 32 lowercase hex chars
    char name[HA_NICK_LEN];
    char avatar[HA_AVATAR_LEN];
    int32_t cumulativeScore;
};

struct HaActiveNvsGameCount {
    uint8_t game;
    uint16_t count;
};

struct HaActiveNvsRecord {
    uint32_t generation;
    uint32_t sessionNumber;
    uint32_t restoredFrom;
    uint8_t activeGame;
    uint8_t participantCount;
    uint8_t gameCount;
    uint8_t reserved;
    HaActiveNvsPlayer participants[HA_ACTIVE_NVS_MAX_PLAYERS];
    HaActiveNvsGameCount games[HA_ACTIVE_NVS_MAX_GAME_COUNTS];
};

struct HaActiveNvsEnvelope {
    uint32_t magic;
    uint16_t schema;
    uint16_t bytes;
    HaActiveNvsRecord record;
    uint32_t crc;
};

static_assert(sizeof(HaActiveNvsEnvelope) <= 4096, "active NVS record must remain bounded");
static_assert(sizeof(HaActiveNvsEnvelope) > HA_ACTIVE_NVS_CHUNK_BYTES,
              "32-player coverage must exercise chunked storage");
static_assert(sizeof(HaActiveNvsEnvelope) <= UINT16_MAX, "envelope size field overflow");

static const size_t HA_ACTIVE_NVS_CHUNK_COUNT =
    (sizeof(HaActiveNvsEnvelope) + HA_ACTIVE_NVS_CHUNK_BYTES - 1) /
    HA_ACTIVE_NVS_CHUNK_BYTES;
static_assert(HA_ACTIVE_NVS_CHUNK_COUNT > 1 && HA_ACTIVE_NVS_CHUNK_COUNT <= 8,
              "unexpected active NVS chunk count");

enum HaActiveNvsSource : uint8_t {
    HaActiveNvsSourceNone = 0,
    HaActiveNvsSourceNvs = 1,
    HaActiveNvsSourceSd = 2,
};

enum HaActiveNvsCheckpointResult : uint8_t {
    HaActiveNvsCheckpointDeferred = 0,
    HaActiveNvsCheckpointWritten = 1,
    HaActiveNvsCheckpointInvalid = 2,
    HaActiveNvsCheckpointFailed = 3,
};

struct HaActiveNvsRuntime {
    bool checkpointed;
    uint32_t lastCheckpointMs;
};

static HaActiveNvsRuntime haActiveNvsRt = {};
static HaActiveNvsEnvelope* haActiveNvsScratch = nullptr;

// Persistence APIs are loop-task-only, like the SD history APIs. One reusable heap
// envelope avoids placing several 2.3 KiB A/B/write buffers on the ESP loop stack.
static bool haActiveNvsStorageBegin() {
    if(haActiveNvsScratch) return true;
    haActiveNvsScratch = new(std::nothrow) HaActiveNvsEnvelope{};
    return haActiveNvsScratch != nullptr;
}

static uint32_t haActiveNvsCrcUpdate(uint32_t crc, const uint8_t* data, size_t length) {
    while(length--) {
        crc ^= *data++;
        for(uint8_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc;
}

static bool haActiveNvsIdentityValid(const char* identity) {
    if(!identity || strnlen(identity, HA_CLIENT_ID_LEN) != 32) return false;
    for(uint8_t i = 0; i < 32; i++) {
        char c = identity[i];
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return identity[32] == '\0';
}

static bool haActiveNvsUtf8Valid(const char* value, size_t capacity, bool allowEmpty) {
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
            if(code < 2) return false; // overlong two-byte form
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
           (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
    }
    return true;
}

static bool haActiveNvsGameValid(uint8_t game, bool allowNone) {
    if(game == HA_GAME_NONE) return allowNone;
    for(size_t i = 0; i < HA_GENERATED_GAME_COUNT; i++)
        if(HA_GENERATED_GAMES[i].id == game && game != HA_GAME_NONE) return true;
    return false;
}

// Validate only the logical payload. A freshly captured record may have generation
// zero; storage assigns a monotonic generation immediately before sealing it.
static bool haActiveNvsPayloadValid(const HaActiveNvsRecord& record) {
    if(!record.sessionNumber || !haActiveNvsGameValid(record.activeGame, true) ||
       record.participantCount > HA_ACTIVE_NVS_MAX_PLAYERS ||
       record.gameCount > HA_ACTIVE_NVS_MAX_GAME_COUNTS || record.reserved != 0)
        return false;
    for(uint8_t i = 0; i < record.participantCount; i++) {
        const HaActiveNvsPlayer& player = record.participants[i];
        if(!haActiveNvsIdentityValid(player.identity) ||
           !haActiveNvsUtf8Valid(player.name, sizeof(player.name), false) ||
           !haActiveNvsUtf8Valid(player.avatar, sizeof(player.avatar), true))
            return false;
        for(uint8_t prior = 0; prior < i; prior++)
            if(strcmp(player.identity, record.participants[prior].identity) == 0)
                return false;
    }
    for(uint8_t i = 0; i < record.gameCount; i++) {
        const HaActiveNvsGameCount& game = record.games[i];
        if(!haActiveNvsGameValid(game.game, false) || game.count == 0) return false;
        for(uint8_t prior = 0; prior < i; prior++)
            if(game.game == record.games[prior].game) return false;
    }
    return true;
}

static bool haActiveNvsRecordValid(const HaActiveNvsRecord& record) {
    return record.generation != 0 && haActiveNvsPayloadValid(record);
}

// Rebuild into a zeroed destination rather than persisting caller padding or unused
// array entries. This is the hard boundary enforcing the intentionally narrow schema.
static bool haActiveNvsCanonicalize(
    const HaActiveNvsRecord& source,
    HaActiveNvsRecord& destination) {
    if(!haActiveNvsPayloadValid(source)) return false;
    memset(&destination, 0, sizeof(destination));
    destination.generation = source.generation;
    destination.sessionNumber = source.sessionNumber;
    destination.restoredFrom = source.restoredFrom;
    destination.activeGame = source.activeGame;
    destination.participantCount = source.participantCount;
    destination.gameCount = source.gameCount;
    for(uint8_t i = 0; i < source.participantCount; i++) {
        const HaActiveNvsPlayer& input = source.participants[i];
        HaActiveNvsPlayer& output = destination.participants[i];
        strlcpy(output.identity, input.identity, sizeof(output.identity));
        strlcpy(output.name, input.name, sizeof(output.name));
        strlcpy(output.avatar, input.avatar, sizeof(output.avatar));
        output.cumulativeScore = input.cumulativeScore;
    }
    for(uint8_t i = 0; i < source.gameCount; i++) {
        destination.games[i].game = source.games[i].game;
        destination.games[i].count = source.games[i].count;
    }
    return true;
}

static uint32_t haActiveNvsEnvelopeCrc(const HaActiveNvsEnvelope& envelope) {
    return haActiveNvsCrcUpdate(
               0xFFFFFFFFUL,
               (const uint8_t*)&envelope,
               offsetof(HaActiveNvsEnvelope, crc)) ^
           0xFFFFFFFFUL;
}

static bool haActiveNvsEnvelopeValid(const HaActiveNvsEnvelope& envelope) {
    return envelope.magic == HA_ACTIVE_NVS_MAGIC &&
           envelope.schema == HA_ACTIVE_NVS_SCHEMA &&
           envelope.bytes == sizeof(HaActiveNvsEnvelope) &&
           envelope.crc == haActiveNvsEnvelopeCrc(envelope) &&
           haActiveNvsRecordValid(envelope.record);
}

static size_t haActiveNvsChunkLength(size_t index) {
    const size_t offset = index * HA_ACTIVE_NVS_CHUNK_BYTES;
    const size_t remaining = sizeof(HaActiveNvsEnvelope) - offset;
    return remaining < HA_ACTIVE_NVS_CHUNK_BYTES ? remaining : HA_ACTIVE_NVS_CHUNK_BYTES;
}

static void haActiveNvsChunkKey(uint8_t slot, size_t index, char out[8]) {
    snprintf(out, 8, "%c%u", slot ? 'b' : 'a', (unsigned)index);
}

static bool haActiveNvsReadSlot(
    Preferences& preferences,
    uint8_t slot,
    HaActiveNvsEnvelope& envelope) {
    memset(&envelope, 0, sizeof(envelope));
    uint8_t* bytes = (uint8_t*)&envelope;
    for(size_t index = 0; index < HA_ACTIVE_NVS_CHUNK_COUNT; index++) {
        char key[8];
        haActiveNvsChunkKey(slot, index, key);
        size_t length = haActiveNvsChunkLength(index);
        if(preferences.getBytesLength(key) != length ||
           preferences.getBytes(key, bytes + index * HA_ACTIVE_NVS_CHUNK_BYTES, length) !=
               length)
            return false;
    }
    return haActiveNvsEnvelopeValid(envelope);
}

static int8_t haActiveNvsNewestSlot(
    bool validA,
    uint32_t generationA,
    bool validB,
    uint32_t generationB) {
    if(validA && validB)
        return generationB > generationA ? 1 : 0;
    if(validA) return 0;
    if(validB) return 1;
    return -1;
}

static bool haActiveNvsRead(HaActiveNvsRecord& out, int8_t* selectedSlot = nullptr) {
    if(!haActiveNvsStorageBegin()) return false;
    Preferences preferences;
    if(!preferences.begin(HA_ACTIVE_NVS_NAMESPACE, true)) return false;
    bool validA = haActiveNvsReadSlot(preferences, 0, *haActiveNvsScratch);
    uint32_t generationA = validA ? haActiveNvsScratch->record.generation : 0;
    if(validA) out = haActiveNvsScratch->record;
    bool validB = haActiveNvsReadSlot(preferences, 1, *haActiveNvsScratch);
    uint32_t generationB = validB ? haActiveNvsScratch->record.generation : 0;
    if(validB && (!validA || generationB > generationA)) out = haActiveNvsScratch->record;
    preferences.end();
    int8_t slot = haActiveNvsNewestSlot(validA, generationA, validB, generationB);
    if(slot < 0) return false;
    if(selectedSlot) *selectedSlot = slot;
    return true;
}

static bool haActiveNvsWriteGeneration(
    const HaActiveNvsRecord& input,
    uint32_t generation) {
    if(!generation || !haActiveNvsStorageBegin()) return false;
    Preferences preferences;
    if(!preferences.begin(HA_ACTIVE_NVS_NAMESPACE, false)) return false;
    bool validA = haActiveNvsReadSlot(preferences, 0, *haActiveNvsScratch);
    uint32_t generationA = validA ? haActiveNvsScratch->record.generation : 0;
    bool validB = haActiveNvsReadSlot(preferences, 1, *haActiveNvsScratch);
    uint32_t generationB = validB ? haActiveNvsScratch->record.generation : 0;
    int8_t newest = haActiveNvsNewestSlot(validA, generationA, validB, generationB);
    if(newest >= 0) {
        uint32_t current = newest == 0 ? generationA : generationB;
        if(generation <= current) {
            preferences.end();
            return false;
        }
    }

    memset(haActiveNvsScratch, 0, sizeof(*haActiveNvsScratch));
    haActiveNvsScratch->magic = HA_ACTIVE_NVS_MAGIC;
    haActiveNvsScratch->schema = HA_ACTIVE_NVS_SCHEMA;
    haActiveNvsScratch->bytes = sizeof(*haActiveNvsScratch);
    if(!haActiveNvsCanonicalize(input, haActiveNvsScratch->record)) {
        preferences.end();
        return false;
    }
    haActiveNvsScratch->record.generation = generation;
    haActiveNvsScratch->crc = haActiveNvsEnvelopeCrc(*haActiveNvsScratch);
    uint8_t target = newest == 0 ? 1 : 0;
    const uint8_t* bytes = (const uint8_t*)haActiveNvsScratch;
    // Chunk zero contains the schema/generation and is the commit marker. Write it
    // last so an interrupted multi-key update cannot look current with stale tails.
    for(size_t index = HA_ACTIVE_NVS_CHUNK_COUNT; index-- > 1;) {
        char key[8];
        haActiveNvsChunkKey(target, index, key);
        size_t length = haActiveNvsChunkLength(index);
        if(preferences.putBytes(
               key,
               bytes + index * HA_ACTIVE_NVS_CHUNK_BYTES,
               length) != length) {
            preferences.end();
            return false;
        }
    }
    char firstKey[8];
    haActiveNvsChunkKey(target, 0, firstKey);
    size_t firstLength = haActiveNvsChunkLength(0);
    if(preferences.putBytes(firstKey, bytes, firstLength) != firstLength) {
        preferences.end();
        return false;
    }

    bool ok = haActiveNvsReadSlot(preferences, target, *haActiveNvsScratch) &&
              haActiveNvsScratch->record.generation == generation;
    preferences.end();
    return ok;
}

[[maybe_unused]] static bool haActiveNvsWrite(const HaActiveNvsRecord& input) {
    return haActiveNvsWriteGeneration(input, input.generation);
}

static bool haActiveNvsLatestGeneration(uint32_t& generation) {
    if(!haActiveNvsStorageBegin()) return false;
    Preferences preferences;
    if(!preferences.begin(HA_ACTIVE_NVS_NAMESPACE, true)) return false;
    bool validA = haActiveNvsReadSlot(preferences, 0, *haActiveNvsScratch);
    uint32_t generationA = validA ? haActiveNvsScratch->record.generation : 0;
    bool validB = haActiveNvsReadSlot(preferences, 1, *haActiveNvsScratch);
    uint32_t generationB = validB ? haActiveNvsScratch->record.generation : 0;
    preferences.end();
    int8_t newest = haActiveNvsNewestSlot(validA, generationA, validB, generationB);
    if(newest < 0) return false;
    generation = newest == 0 ? generationA : generationB;
    return true;
}

static bool haActiveNvsErase() {
    Preferences preferences;
    if(!preferences.begin(HA_ACTIVE_NVS_NAMESPACE, false)) return false;
    bool ok = preferences.clear();
    preferences.end();
    if(ok) {
        haActiveNvsRt = HaActiveNvsRuntime{};
        if(haActiveNvsScratch) memset(haActiveNvsScratch, 0, sizeof(*haActiveNvsScratch));
    }
    return ok;
}

// Highest generation wins between media; the SD copy wins an equal-generation tie.
static HaActiveNvsSource haActiveNvsChooseSource(
    bool sdValid,
    uint32_t sdGeneration,
    bool nvsValid,
    uint32_t nvsGeneration) {
    sdValid = sdValid && sdGeneration != 0;
    nvsValid = nvsValid && nvsGeneration != 0;
    if(sdValid && (!nvsValid || sdGeneration >= nvsGeneration)) return HaActiveNvsSourceSd;
    if(nvsValid) return HaActiveNvsSourceNvs;
    return HaActiveNvsSourceNone;
}

static bool haActiveNvsCheckpointDue(uint32_t now, bool force = false) {
    return force || !haActiveNvsRt.checkpointed ||
           (uint32_t)(now - haActiveNvsRt.lastCheckpointMs) >=
               HA_ACTIVE_NVS_MIN_CHECKPOINT_MS;
}

static void haActiveNvsResetCheckpointRateLimit() {
    haActiveNvsRt = HaActiveNvsRuntime{};
}

static void haActiveNvsSetCheckpointBaseline(uint32_t now) {
    haActiveNvsRt.checkpointed = true;
    haActiveNvsRt.lastCheckpointMs = now;
}

// Build the narrow persistence record from a HaHost-compatible snapshot without
// including ha_host.h here. Keeping this a template lets native tests use a small
// structural stub and avoids forcing the full game engine into persistence-only TUs.
template <typename THost>
static bool haActiveNvsCaptureHost(
    const THost& host,
    uint32_t sessionNumber,
    uint32_t restoredFrom,
    HaActiveNvsRecord& out,
    uint32_t generation = 0) {
    memset(&out, 0, sizeof(out));
    out.generation = generation; // pass the selected SD seq when transitioning off-card
    out.sessionNumber = sessionNumber;
    out.restoredFrom = restoredFrom;
    out.activeGame = host.activeGame;
    static_assert(
        sizeof(host.session) / sizeof(host.session[0]) <= HA_ACTIVE_NVS_MAX_PLAYERS,
        "host session ledger exceeds active fallback capacity");
    const size_t hostPlayerCapacity = sizeof(host.session) / sizeof(host.session[0]);
    for(size_t i = 0; i < hostPlayerCapacity; i++) {
        const auto& source = host.session[i];
        if(!source.used) continue;
        if(out.participantCount >= HA_ACTIVE_NVS_MAX_PLAYERS ||
           !haActiveNvsIdentityValid(source.clientId) ||
           !haActiveNvsUtf8Valid(source.nick, sizeof(source.nick), false) ||
           !haActiveNvsUtf8Valid(source.avatar, sizeof(source.avatar), true))
            return false;
        HaActiveNvsPlayer& destination = out.participants[out.participantCount++];
        strlcpy(destination.identity, source.clientId, sizeof(destination.identity));
        strlcpy(destination.name, source.nick, sizeof(destination.name));
        strlcpy(destination.avatar, source.avatar, sizeof(destination.avatar));
        destination.cumulativeScore = source.score;
    }
    static_assert(
        sizeof(host.games) / sizeof(host.games[0]) <= HA_ACTIVE_NVS_MAX_GAME_COUNTS,
        "host sparse game table exceeds active fallback capacity");
    if(host.gameCount > sizeof(host.games) / sizeof(host.games[0])) return false;
    for(uint8_t i = 0; i < host.gameCount; i++) {
        const auto& source = host.games[i];
        if(source.game == 0 || source.count == 0) return false;
        HaActiveNvsGameCount& destination = out.games[out.gameCount++];
        destination.game = source.game;
        destination.count = source.count;
    }
    return haActiveNvsPayloadValid(out);
}

// No-SD write path. First state is immediate; later writes are limited to one every
// 30 seconds unless force=true. The timer advances only after a verified NVS commit.
static HaActiveNvsCheckpointResult haActiveNvsCheckpointNoSd(
    const HaActiveNvsRecord& snapshot,
    uint32_t now,
    bool force = false) {
    if(!haActiveNvsPayloadValid(snapshot)) return HaActiveNvsCheckpointInvalid;
    if(!haActiveNvsCheckpointDue(now, force)) return HaActiveNvsCheckpointDeferred;

    uint32_t floor = 0;
    bool haveCurrent = haActiveNvsLatestGeneration(floor);
    if(haveCurrent && floor == UINT32_MAX) return HaActiveNvsCheckpointFailed;
    uint32_t generation = snapshot.generation > floor ? snapshot.generation : floor + 1;
    if(!generation || !haActiveNvsWriteGeneration(snapshot, generation))
        return HaActiveNvsCheckpointFailed;
    haActiveNvsRt.checkpointed = true;
    haActiveNvsRt.lastCheckpointMs = now;
    return HaActiveNvsCheckpointWritten;
}

// Mirror a verified SD active record without inventing a second generation. The
// two media then describe one logical commit and SD wins their intentional tie at
// boot. An equal-generation record is replaced as well: equal but divergent media
// are resolved in SD's favor, so leaving the old NVS payload would not be a repair.
[[maybe_unused]] static HaActiveNvsCheckpointResult haActiveNvsCheckpointSdMirror(
    const HaActiveNvsRecord& snapshot,
    uint32_t now,
    bool force = false) {
    if(!haActiveNvsRecordValid(snapshot)) return HaActiveNvsCheckpointInvalid;
    if(!haActiveNvsCheckpointDue(now, force)) return HaActiveNvsCheckpointDeferred;

    uint32_t current = 0;
    bool haveCurrent = haActiveNvsLatestGeneration(current);
    if(haveCurrent && current > snapshot.generation)
        return HaActiveNvsCheckpointInvalid;
    if(haveCurrent && current == snapshot.generation && !haActiveNvsErase())
        return HaActiveNvsCheckpointFailed;
    if(!haActiveNvsWriteGeneration(snapshot, snapshot.generation))
        return HaActiveNvsCheckpointFailed;
    haActiveNvsSetCheckpointBaseline(now);
    return HaActiveNvsCheckpointWritten;
}
