#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

class String {
  public:
    String() = default;
    String(const char* text) : value_(text ? text : "") {}

    String& operator=(const char* text) {
        value_ = text ? text : "";
        return *this;
    }
    String& operator+=(const char* text) {
        value_ += text ? text : "";
        return *this;
    }
    String& operator+=(const String& other) {
        value_ += other.value_;
        return *this;
    }
    String& operator+=(char character) {
        value_ += character;
        return *this;
    }
    void reserve(size_t bytes) { value_.reserve(bytes); }
    const char* c_str() const { return value_.c_str(); }
    size_t length() const { return value_.size(); }

  private:
    std::string value_;
};

static String ha_json_escape(const char* text) {
    String escaped;
    for(const char* p = text ? text : ""; *p; p++) {
        if(*p == '\\' || *p == '"') escaped += "\\";
        char one[2] = {*p, '\0'};
        escaped += one;
    }
    return escaped;
}

struct HaGeneratedLanguage {
    const char* code;
    const char* label;
    const char* fallback;
};

static const HaGeneratedLanguage HA_GENERATED_LANGUAGES[] = {
    {"en", "English", ""},
    {"de", "Deutsch", "en"},
    {"pt-br", "Portuguese", "en"},
};
static const size_t HA_GENERATED_LANGUAGE_COUNT =
    sizeof(HA_GENERATED_LANGUAGES) / sizeof(HA_GENERATED_LANGUAGES[0]);

struct HaBakedPack {
    uint8_t game;
    const char* lang;
    const char* fallback;
    const char* text;
};

static const HaBakedPack HA_BAKED_PACKS[] = {
    {1, "en", "first", "Pack: English One\nWord: one\n"},
    {1, "en", "second", "Pack: English Two\nWord: two\n"},
    {1, "de", "eins", "Pack: Deutsch\nWord: eins\n"},
    {19, "en", "places", "Pack: Places\nLoc: Beach\nR: Lifeguard\nR: Surfer\n"},
};
static const size_t HA_BAKED_PACK_COUNT =
    sizeof(HA_BAKED_PACKS) / sizeof(HA_BAKED_PACKS[0]);

class Engine {
  public:
    bool failBegin = false;
    bool failPack = false;
    bool failItem = false;
    bool failCommit = false;
    bool began = false;
    bool committed = false;
    unsigned aborts = 0;
    uint8_t targetGame = 0;
    std::string locale;
    uint16_t expectedPacks = 0;
    uint16_t expectedItems = 0;
    uint32_t committedAt = 0;
    std::vector<std::string> packs;
    std::vector<std::string> items;

    bool contentBegin(uint8_t game, const char* language) {
        began = true;
        targetGame = game;
        locale = language ? language : "";
        packs.clear();
        items.clear();
        return !failBegin;
    }
    bool contentPack(uint8_t game, const char* name) {
        if(failPack || game != targetGame) return false;
        packs.emplace_back(name ? name : "");
        return true;
    }
    bool contentItem(const char* json) {
        if(failItem) return false;
        items.emplace_back(json ? json : "");
        return true;
    }
    bool contentCommit(uint16_t packCount, uint16_t itemCount, uint32_t rawNow) {
        expectedPacks = packCount;
        expectedItems = itemCount;
        committedAt = rawNow;
        committed = !failCommit && packCount == packs.size() && itemCount == items.size();
        return committed;
    }
    void contentAbort() { aborts++; }
};

#define HA_CONTENT_NATIVE_TEST 1
#include "ha_content.h"

static void testWholeGameLocaleSelection() {
    Engine engine;
    assert(haContentLoadGame(engine, 1, "de", 100));
    assert(engine.locale == "de");
    assert(engine.packs.size() == 1);
    assert(engine.packs[0] == "Deutsch");
    assert(engine.items.size() == 1);
    assert(engine.items[0] == "{\"word\":\"eins\"}");
    assert(engine.expectedPacks == 1);
    assert(engine.expectedItems == 1);
    assert(engine.committedAt == 100);

    Engine fallback;
    assert(haContentLoadGame(fallback, 19, "pt-br", 200));
    assert(fallback.locale == "pt-br");
    assert(fallback.packs.size() == 1);
    assert(fallback.packs[0] == "Places");
    assert(fallback.items.size() == 1);
    assert(fallback.items[0] ==
           "{\"loc\":\"Beach\",\"r\":\"Lifeguard\",\"r\":\"Surfer\"}");
}

static void testActiveGameAndPacklessTransactions() {
    Engine english;
    assert(haContentLoadGame(english, 1, "en", 300));
    assert(english.packs.size() == 2);
    assert(english.items.size() == 2);

    Engine packless;
    assert(haContentLoadGame(packless, 18, "de", 400));
    assert(packless.targetGame == 18);
    assert(packless.locale == "de");
    assert(packless.expectedPacks == 0);
    assert(packless.expectedItems == 0);
}

static void testFailureAbortsStaging() {
    Engine invalidLocale;
    assert(!haContentLoadGame(invalidLocale, 1, "fr", 1));
    assert(!invalidLocale.began);

    Engine packFailure;
    packFailure.failPack = true;
    assert(!haContentLoadGame(packFailure, 1, "en", 2));
    assert(packFailure.aborts == 1);

    Engine itemFailure;
    itemFailure.failItem = true;
    assert(!haContentLoadGame(itemFailure, 1, "en", 3));
    assert(itemFailure.aborts == 1);

    Engine commitFailure;
    commitFailure.failCommit = true;
    assert(!haContentLoadGame(commitFailure, 1, "en", 4));
    assert(commitFailure.aborts == 1);

    Engine malformed;
    uint16_t itemCount = 0;
    assert(!haContentLoadPack(
        malformed,
        1,
        "Pack: Broken\nthis line has no colon\n",
        "fallback",
        itemCount));
}

int main() {
    testWholeGameLocaleSelection();
    testActiveGameAndPacklessTransactions();
    testFailureAbortsStaging();
    std::cout << "native active-game content-loader tests passed\n";
}
