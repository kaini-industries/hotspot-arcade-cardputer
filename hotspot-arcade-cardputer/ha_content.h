// Transactionally load only the selected game's baked content into the engine.
//
// This is a straight port of content_stream_pack() in
// flipper/hotspot-arcade/helpers/ha_session.c, minus the UART: the grammar is the
// contract ("Key: value" lines, a "---" or blank line ends a block, a "Pack:" key
// names the pack and is not part of an item), and the engine still receives each
// block as a JSON object of the file's own lowercased keys. Keeping the parse
// identical is the point -- pack files stay portable between the Flipper build and
// this one, and all the game semantics stay where they already live, in ha_games.h.
#pragma once
#ifndef HA_CONTENT_NATIVE_TEST
#include <Arduino.h>
#include "ha_games.h"
#include "ha_json.h"
#include "ha_bundle.h"
#endif

// copy_trim(): leading and trailing blanks off a [start,end) slice. `lower`
// case-folds ASCII only, matching the Flipper's byte loop -- a UTF-8 lead or
// continuation byte must not be touched by a locale-aware tolower().
//
// Keep this parser on the small Arduino String surface mirrored by the native
// loader regression and by the engine simulator, so its transaction grammar is
// exercised off-target as well as in the pinned board build.
static void haTrimTo(const char* s, const char* e, String& out, bool lower = false) {
    while(s < e && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
    while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    out = "";
    out.reserve((size_t)(e - s) + 1);
    for(const char* p = s; p < e; p++) {
        char c = *p;
        if(lower && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out += c;
    }
}

static bool haContentFinishItem(
    Engine& engine,
    String& object,
    bool& any,
    uint16_t& itemCount) {
    if(!any) return true;
    object += "}";
    if(itemCount == UINT16_MAX || !engine.contentItem(object.c_str())) return false;
    itemCount++;
    object = "{";
    any = false;
    return true;
}

static bool haContentLoadPack(
    Engine& engine,
    uint8_t game,
    const char* text,
    const char* fallback,
    uint16_t& itemCount) {
    itemCount = 0;
    if(!text || !fallback) return false;
    // Pass one: the pack name, so contentPack() goes first (it opens the pack the
    // items then attach to).
    String name = fallback;
    for(const char* p = text; p && *p;) {
        const char* eol = strchr(p, '\n');
        if(!eol) eol = p + strlen(p);
        if(strncmp(p, "Pack:", 5) == 0) {
            String v;
            haTrimTo(p + 5, eol, v);
            if(v.length()) name = v;
            break;
        }
        p = (*eol) ? eol + 1 : eol;
    }
    if(!engine.contentPack(game, name.c_str())) return false;

    // Pass two: blocks. Duplicate keys are deliberately retained: Spyfall sends
    // one Loc followed by multiple R values and the engine consumes them with
    // ha_json_str_nth(). The build-time parser has already proved every key shape
    // and per-pack capacity, but this runtime parser still fails closed.
    String obj = "{";
    String key, val;
    bool any = false;
    for(const char* p = text; p && *p;) {
        const char* eol = strchr(p, '\n');
        if(!eol) eol = p + strlen(p);

        const char* s = p;
        const char* e = eol;
        while(s < e && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;

        bool sep = (s == e) || (e - s == 3 && strncmp(s, "---", 3) == 0);
        if(sep) {
            if(!haContentFinishItem(engine, obj, any, itemCount)) return false;
        } else {
            const char* colon = (const char*)memchr(s, ':', (size_t)(e - s));
            if(!colon) return false;
            haTrimTo(s, colon, key, true);
            haTrimTo(colon + 1, e, val);
            if(!key.length() || !val.length()) return false;
            if(strcmp(key.c_str(), "pack") != 0) {
                if(any) obj += ",";
                obj += "\"";
                obj += ha_json_escape(key.c_str());
                obj += "\":\"";
                obj += ha_json_escape(val.c_str());
                obj += "\"";
                any = true;
            }
        }
        p = (*eol) ? eol + 1 : eol;
    }
    return haContentFinishItem(engine, obj, any, itemCount);
}

static const HaGeneratedLanguage* haContentLanguage(const char* code) {
    if(!code) return nullptr;
    for(size_t i = 0; i < HA_GENERATED_LANGUAGE_COUNT; i++)
        if(strcmp(HA_GENERATED_LANGUAGES[i].code, code) == 0)
            return &HA_GENERATED_LANGUAGES[i];
    return nullptr;
}

static bool haContentHasPacks(uint8_t game, const char* language = nullptr) {
    for(size_t i = 0; i < HA_BAKED_PACK_COUNT; i++) {
        const HaBakedPack& bp = HA_BAKED_PACKS[i];
        if(bp.game == game && (!language || strcmp(bp.lang, language) == 0)) return true;
    }
    return false;
}

static const char* haContentSourceLanguage(uint8_t game, const char* requested) {
    const HaGeneratedLanguage* language = haContentLanguage(requested);
    for(size_t depth = 0; language && depth < HA_GENERATED_LANGUAGE_COUNT; depth++) {
        if(haContentHasPacks(game, language->code)) return language->code;
        language = language->fallback[0] ? haContentLanguage(language->fallback) : nullptr;
    }
    return nullptr;
}

// Commit one active-game bank. A missing translation follows the manifest's
// reviewed fallback chain for pack bytes while the requested locale is retained
// in the bank, so the phone UI remains Portuguese even when a new game's content
// is currently English. Packless games commit an empty typed bank.
static bool haContentLoadGame(
    Engine& engine,
    uint8_t game,
    const char* language,
    uint32_t rawNow) {
    if(!haContentLanguage(language) || !engine.contentBegin(game, language)) return false;
    const bool gameHasPacks = haContentHasPacks(game);
    const char* sourceLanguage = gameHasPacks
                                     ? haContentSourceLanguage(game, language)
                                     : nullptr;
    if(gameHasPacks && !sourceLanguage) {
        engine.contentAbort();
        return false;
    }
    uint16_t packCount = 0;
    uint16_t itemCount = 0;
    for(size_t i = 0; i < HA_BAKED_PACK_COUNT; i++) {
        const HaBakedPack& bp = HA_BAKED_PACKS[i];
        if(bp.game != game || !sourceLanguage || strcmp(bp.lang, sourceLanguage) != 0)
            continue;
        if(packCount == UINT16_MAX) {
            engine.contentAbort();
            return false;
        }
        packCount++;
        uint16_t loaded = 0;
        if(!haContentLoadPack(engine, bp.game, bp.text, bp.fallback, loaded) ||
           itemCount > UINT16_MAX - loaded) {
            engine.contentAbort();
            return false;
        }
        itemCount += loaded;
    }
    if(gameHasPacks && (!packCount || !itemCount)) {
        engine.contentAbort();
        return false;
    }
    bool committed = engine.contentCommit(packCount, itemCount, rawNow);
    if(!committed) engine.contentAbort(); // idempotent after commit's checked abort path
    return committed;
}
