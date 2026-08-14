#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "ha_ui_text.h"

std::atomic<uint32_t> haUiLocaleCache{(uint32_t)HaUiEnglish};

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

    haUiSetLocaleFromLanguage(german);
    assert(haUiActiveLocale() == HaUiGerman);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "OPTIONEN") == 0);

    haUiSetLocaleFromLanguage(english);
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);

    haUiSetLocaleFromLanguage(portuguese);
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);

    haUiSetLocaleFromLanguage(UINT8_MAX);
    assert(haUiActiveLocale() == HaUiEnglish);
    assert(std::strcmp(haUiT(HaUiTextSettingsTitle), "SETTINGS") == 0);
    assert(std::strcmp(
               haUiTextForLocale((HaUiTextKey)UINT8_MAX, HaUiGerman),
               "") == 0);
}

static size_t glcdWidth(const char* text, unsigned textSize = 1) {
    return std::strlen(text) * 6U * textSize;
}

static void expectFits(const char* text, unsigned x, unsigned textSize = 1) {
    assert(x < 240);
    if(glcdWidth(text, textSize) > 240U - x) {
        std::cerr << "text exceeds Cardputer width budget: \"" << text << "\" is "
                  << glcdWidth(text, textSize) << "px, budget " << 240U - x << "px\n";
        assert(false);
    }
}

static void testPhysicalWidthBudgets() {
    static constexpr HaUiTextKey FOOTERS[] = {
        HaUiTextDashboardFooter,
        HaUiTextGamesFooter,
        HaUiTextBoardFooterSd,
        HaUiTextBoardFooterNoSd,
        HaUiTextHistoryFooter,
        HaUiTextHistoryDetailFooter,
        HaUiTextHistoryDetailOfflineFooter,
        HaUiTextConfirmCancelFooter,
        HaUiTextDiscardFooter,
        HaUiTextSettingsFooter,
        HaUiTextBackFooter,
        HaUiTextEventLogFooter,
        HaUiTextSsidFooter,
    };
    static constexpr HaUiTextKey HEADERS[] = {
        HaUiTextSessionLeaderboard,
        HaUiTextRestoreSessionTitle,
        HaUiTextNewSessionTitle,
        HaUiTextArchiveFailedTitle,
        HaUiTextSettingsTitle,
        HaUiTextDiagnosticsTitle,
        HaUiTextEventLogTitle,
        HaUiTextApNameTitle,
    };
    static constexpr HaUiTextKey BODY_X3[] = {
        HaUiTextWaitingForPhones,
        HaUiTextMicroSdUnavailable,
        HaUiTextNoArchivedSessions,
        HaUiTextHistoryEmptyHint,
        HaUiTextHighWaterMarks,
        HaUiTextNothingYet,
        HaUiTextTypeNewSsid,
        HaUiTextApplyingRestartsAp,
        HaUiTextDropsConnectedPhones,
        HaUiTextMemoryUnavailable,
    };
    static constexpr HaUiTextKey BODY_X10[] = {
        HaUiTextCurrentPlayArchivedFirst,
        HaUiTextPhonesReconnectScores,
        HaUiTextRestoreFailed,
    };
    static constexpr HaUiTextKey BODY_X12[] = {
        HaUiTextArchiveScores,
        HaUiTextResetCumulativeScores,
        HaUiTextNoSdCannotArchive,
    };
    static constexpr HaUiTextKey BODY_X8[] = {
        HaUiTextActiveCouldNotArchive,
        HaUiTextSecondYDiscards,
        HaUiTextResetFailed,
    };
    static constexpr HaUiTextKey SETTING_LABELS[] = {
        HaUiTextSettingLanguage,
        HaUiTextSettingAccessPoint,
        HaUiTextSettingEventLog,
        HaUiTextSettingDiagnostics,
    };

    for(HaUiLocale locale : {HaUiEnglish, HaUiGerman}) {
        for(HaUiTextKey key : FOOTERS) expectFits(haUiTextForLocale(key, locale), 3);
        // The battery begins at x=213 for "100%"; reserve a 3px gap before it.
        for(HaUiTextKey key : HEADERS)
            assert(glcdWidth(haUiTextForLocale(key, locale)) <= 207U);
        for(HaUiTextKey key : BODY_X3) expectFits(haUiTextForLocale(key, locale), 3);
        for(HaUiTextKey key : BODY_X10) expectFits(haUiTextForLocale(key, locale), 10);
        for(HaUiTextKey key : BODY_X12) expectFits(haUiTextForLocale(key, locale), 12);
        for(HaUiTextKey key : BODY_X8) expectFits(haUiTextForLocale(key, locale), 8);
        for(HaUiTextKey key : SETTING_LABELS)
            assert(glcdWidth(haUiTextForLocale(key, locale)) <= 84U);

        char formatted[96];
        std::snprintf(
            formatted,
            sizeof(formatted),
            haUiTextForLocale(HaUiTextPresenceFormat, locale),
            haUiTextForLocale(HaUiTextStorageSdError, locale),
            10,
            22);
        expectFits(formatted, 132);
        std::snprintf(
            formatted,
            sizeof(formatted),
            haUiTextForLocale(HaUiTextGamesTitleFormat, locale),
            haUiTextForLocale(HaUiTextMostPlayed, locale));
        assert(glcdWidth(formatted) <= 207U);
        std::snprintf(
            formatted,
            sizeof(formatted),
            haUiTextForLocale(HaUiTextHistoryTitleFormat, locale),
            (unsigned long)UINT32_MAX);
        assert(glcdWidth(formatted) <= 207U);
        std::snprintf(
            formatted,
            sizeof(formatted),
            haUiTextForLocale(HaUiTextSessionTitleFormat, locale),
            (unsigned long)UINT32_MAX);
        assert(glcdWidth(formatted) <= 207U);
        std::snprintf(
            formatted,
            sizeof(formatted),
            haUiTextForLocale(HaUiTextSessionNumberFormat, locale),
            (unsigned long)UINT32_MAX);
        expectFits(formatted, 10, 2);

        for(size_t i = 0; i < HA_GENERATED_GAME_COUNT; i++) {
            const HaGeneratedGame& game = HA_GENERATED_GAMES[i];
            const char* label = haUiGameLabelForLocale(game.key, game.label, locale);
            const char* desc = haUiGameDescForLocale(game.key, game.desc, locale);
            std::snprintf(formatted, sizeof(formatted), "*%s", label);
            // Reserve the rightmost 24px used by the 1v1 marker.
            assert(glcdWidth(formatted, 2) <= 216U);
            expectFits(desc, 3);
            std::snprintf(
                formatted,
                sizeof(formatted),
                haUiTextForLocale(HaUiTextGameFormat, locale),
                label);
            expectFits(formatted, 3);
        }
    }

    const HaUiGameTranslation* reversi = haUiFindGameTranslation("reversi");
    assert(reversi);
    assert(std::strcmp(reversi->descDe, "Steine drehen, Mehrheit gewinnt") == 0);
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
    testPhysicalWidthBudgets();
    std::cout << "native host-UI localization tests passed\n";
}
