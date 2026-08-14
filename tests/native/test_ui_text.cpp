#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "ha_ui_text.h"

uint8_t haLang = 0;

static bool isAscii(const char* text) {
    if(!text) return false;
    for(const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; p++)
        if(*p > 0x7f) return false;
    return true;
}

static uint8_t languageIndex(const char* code) {
    for(size_t i = 0; i < HA_GENERATED_LANGUAGE_COUNT; i++)
        if(std::strcmp(HA_GENERATED_LANGUAGES[i].code, code) == 0) return (uint8_t)i;
    return UINT8_MAX;
}

static void testLanguageSelectionAndFallback() {
    assert(haUiLocaleFromCode("en") == HaUiEnglish);
    assert(haUiLocaleFromCode("de") == HaUiGerman);
    assert(haUiLocaleFromCode("pt-br") == HaUiEnglish);
    assert(haUiLocaleFromCode(nullptr) == HaUiEnglish);

    const uint8_t english = languageIndex("en");
    const uint8_t german = languageIndex("de");
    const uint8_t portuguese = languageIndex("pt-br");
    assert(english != UINT8_MAX);
    assert(german != UINT8_MAX);
    assert(portuguese != UINT8_MAX);

    haLang = german;
    assert(haUiActiveLocale() == HaUiGerman);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "OPTIONEN") == 0);

    haLang = english;
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);

    haLang = portuguese;
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);

    haLang = UINT8_MAX;
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);
    assert(std::strcmp(
               haUiTextForLocale((HaUiTextKey)UINT8_MAX, HaUiGerman),
               "") == 0);
}

static void testUiKeyCoverageAndFontSafety() {
    for(size_t i = 0; i < (size_t)HaUiTextCount; i++) {
        const HaUiTextKey key = (HaUiTextKey)i;
        assert(haUiHasGermanText(key));
        assert(haUiTextForLocale(key, HaUiEnglish)[0] != '\0');
        assert(haUiTextForLocale(key, HaUiGerman)[0] != '\0');
        assert(isAscii(haUiTextForLocale(key, HaUiEnglish)));
        assert(isAscii(haUiTextForLocale(key, HaUiGerman)));
    }
}

static void testGeneratedGameCoverageAndFallback() {
    for(size_t i = 0; i < HA_GENERATED_GAME_COUNT; i++) {
        const HaGeneratedGame& game = HA_GENERATED_GAMES[i];
        assert(haUiHasGermanGame(game.key));
        assert(std::strcmp(
                   haUiGameLabelForLocale(game.key, game.label, HaUiEnglish),
                   game.label) == 0);
        assert(std::strcmp(
                   haUiGameDescForLocale(game.key, game.desc, HaUiEnglish),
                   game.desc) == 0);
        assert(haUiGameLabelForLocale(game.key, game.label, HaUiGerman)[0] != '\0');
        assert(haUiGameDescForLocale(game.key, game.desc, HaUiGerman)[0] != '\0');
        assert(isAscii(haUiGameLabelForLocale(game.key, game.label, HaUiGerman)));
        assert(isAscii(haUiGameDescForLocale(game.key, game.desc, HaUiGerman)));
    }

    assert(std::strcmp(
               haUiGameLabelForLocale("future-game", "Future Game", HaUiGerman),
               "Future Game") == 0);
    assert(std::strcmp(
               haUiGameDescForLocale("future-game", "Future description", HaUiGerman),
               "Future description") == 0);
    assert(std::strcmp(haUiGameLabelForLocale(nullptr, nullptr, HaUiGerman), "") == 0);
}

int main() {
    testLanguageSelectionAndFallback();
    testUiKeyCoverageAndFontSafety();
    testGeneratedGameCoverageAndFallback();
    std::cout << "native host-UI localization tests passed\n";
}
