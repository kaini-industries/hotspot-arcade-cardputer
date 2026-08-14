// Allocation-free Cardputer host-UI localization.
//
// The content manifest remains authoritative for language order and game metadata.
// This layer keys localized game text by the generated stable game key and selects
// German only when the active generated language code is exactly "de". Every other
// language, invalid index, missing translation, and unknown game falls back to the
// generated English text.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ha_metadata.h"

extern uint8_t haLang;

enum HaUiLocale : uint8_t {
    HaUiEnglish = 0,
    HaUiGerman = 1,
};

enum HaUiTextKey : uint8_t {
    HaUiTextNone = 0,
    HaUiTextJoin,
    HaUiTextStorageSdOk,
    HaUiTextStorageSdError,
    HaUiTextPresenceFormat,
    HaUiTextGameFormat,
    HaUiTextWaitingForPhones,
    HaUiTextMoreFormat,
    HaUiTextDashboardFooter,
    HaUiTextGamesTitleFormat,
    HaUiTextMostPlayed,
    HaUiTextGamesFooter,
    HaUiTextSessionLeaderboard,
    HaUiTextNoPlayers,
    HaUiTextBoardFooterSd,
    HaUiTextBoardFooterNoSd,
    HaUiTextHistoryTitleFormat,
    HaUiTextMicroSdUnavailable,
    HaUiTextNoArchivedSessions,
    HaUiTextHistoryEmptyHint,
    HaUiTextHistoryFooter,
    HaUiTextSessionTitleFormat,
    HaUiTextHistoryDetailFooter,
    HaUiTextHistoryDetailOfflineFooter,
    HaUiTextRestoreSessionTitle,
    HaUiTextSessionNumberFormat,
    HaUiTextCurrentPlayArchivedFirst,
    HaUiTextPhonesReconnectScores,
    HaUiTextRestoreFailed,
    HaUiTextConfirmCancelFooter,
    HaUiTextNewSessionTitle,
    HaUiTextArchiveScores,
    HaUiTextResetCumulativeScores,
    HaUiTextNoSdCannotArchive,
    HaUiTextArchiveFailedTitle,
    HaUiTextDiscardScores,
    HaUiTextActiveCouldNotArchive,
    HaUiTextSecondYDiscards,
    HaUiTextResetFailed,
    HaUiTextDiscardFooter,
    HaUiTextSettingsTitle,
    HaUiTextSettingLanguage,
    HaUiTextSettingAccessPoint,
    HaUiTextSettingEventLog,
    HaUiTextSettingDiagnostics,
    HaUiTextAudioOff,
    HaUiTextAudioLow,
    HaUiTextAudioHigh,
    HaUiTextOn,
    HaUiTextOff,
    HaUiTextGo,
    HaUiTextSettingsFooter,
    HaUiTextDiagnosticsTitle,
    HaUiTextDiagDeviceFormat,
    HaUiTextDiagHeapFormat,
    HaUiTextDiagWsFormat,
    HaUiTextDiagRateFormat,
    HaUiTextDiagFlowFormat,
    HaUiTextDiagLoopFormat,
    HaUiTextDiagSdFormat,
    HaUiTextHighWaterMarks,
    HaUiTextBackFooter,
    HaUiTextEventLogTitle,
    HaUiTextNothingYet,
    HaUiTextEventLogFooter,
    HaUiTextApNameTitle,
    HaUiTextTypeNewSsid,
    HaUiTextApplyingRestartsAp,
    HaUiTextDropsConnectedPhones,
    HaUiTextSsidFooter,
    HaUiTextMemoryUnavailable,
    HaUiTextEventJoin,
    HaUiTextEventName,
    HaUiTextEventLeave,
    HaUiTextEventApUp,
    HaUiTextEventSessionResumed,
    HaUiTextEventSessionPaused,
    HaUiTextEventApStopped,
    HaUiTextEventGameChanged,
    HaUiTextEventRoundEnded,
    HaUiTextEventResumeApBeforeRename,
    HaUiTextEventArchiveUnavailable,
    HaUiTextEventArchiveFailed,
    HaUiTextEventArchiveSkipped,
    HaUiTextEventGenerationExhausted,
    HaUiTextEventSessionIdFailed,
    HaUiTextEventCheckpointFailed,
    HaUiTextEventNewSession,
    HaUiTextEventRestoreStorageFailed,
    HaUiTextEventHistoryRestored,
    HaUiTextEventPacksLoaded,
    HaUiTextEventLanguageLoadFailed,
    HaUiTextEventLanguageFormat,
    HaUiTextMatchStartedFormat,
    HaUiTextRoleFormat,
    HaUiTextRoundWinFormat,
    HaUiTextRoundWinDetailFormat,
    HaUiTextRoundDrawFormat,
    HaUiTextRoundDrawDetailFormat,
    HaUiTextRoundCompleteFormat,
    HaUiTextGameCompleteFormat,
    HaUiTextCount,
};

struct HaUiTextEntry {
    const char* en;
    const char* de;
};

// German strings use ASCII spellings because the Cardputer's built-in GLCD font
// does not render umlauts or sharp-s. Keep footer strings within the 240px panel.
static constexpr HaUiTextEntry HA_UI_TEXT[HaUiTextCount] = {
    {"None", "Keins"},
    {"JOIN", "CODE"},
    {"SD OK", "SD OK"},
    {"SD ERR", "SD FEHL"},
    {"%s  %d on/%d off", "%s  %d da/%d weg"},
    {"Game: %s", "Spiel: %s"},
    {"waiting for phones to join...", "warte auf Handys..."},
    {"+%d more", "+%d weitere"},
    {"G game L board H history D diagnostics", "G Spiel L Rang H Verlauf D Diagnose"},
    {"GAMES - %s", "SPIELE - %s"},
    {"MOST PLAYED", "MEISTGESPIELT"},
    {";/. move  S sort  ENTER pick  ESC back", ";/. Wahl S Sort ENTER Start ESC zurueck"},
    {"SESSION LEADERBOARD", "SESSION-RANGLISTE"},
    {"no players yet", "noch keine Spieler"},
    {";/. scroll R new session ESC", ";/. scroll R neue Session ESC"},
    {";/. scroll R reset (no SD)", ";/. scroll R reset (keine SD)"},
    {"HISTORY - %lu", "VERLAUF - %lu"},
    {"microSD unavailable", "microSD nicht verfuegbar"},
    {"no archived sessions", "keine archivierten Sessions"},
    {"R on the leaderboard starts one", "R in der Rangliste startet eine"},
    {";/. move ,// page ENTER view ESC", ";/. Wahl ,// Seite ENTER ansehen ESC"},
    {"SESSION #%lu", "SESSION #%lu"},
    {";/. scroll Y restore ESC", ";/. scroll Y laden ESC"},
    {";/. scroll ESC (restore offline)", ";/. scroll ESC (Laden nur offline)"},
    {"RESTORE SESSION?", "SESSION LADEN?"},
    {"Session #%lu", "Session #%lu"},
    {"Current play is archived first.", "Aktuelles Spiel wird erst archiviert."},
    {"Phones reconnect to restored scores.", "Handys verbinden sich mit alten Punkten."},
    {"Restore failed; active play unchanged.", "Laden fehlgeschlagen; Spiel bleibt."},
    {"Y confirm   ESC cancel", "Y bestaetigen   ESC abbrechen"},
    {"NEW SESSION?", "NEUE SESSION?"},
    {"Archive scores", "Punkte archivieren"},
    {"Then reset cumulative session scores.", "Dann werden Gesamtpunkte geloescht."},
    {"No SD: this session cannot be archived.", "Keine SD: Session nicht archivierbar."},
    {"ARCHIVE FAILED", "ARCHIV FEHLGESCHLAGEN"},
    {"Discard scores?", "Punkte verwerfen?"},
    {"The active session could not be archived.", "Aktive Session nicht archiviert."},
    {"A second Y permanently discards it.", "Ein zweites Y verwirft sie dauerhaft."},
    {"Reset failed; active play is unchanged.", "Reset fehlgeschlagen; Spiel bleibt."},
    {"Y DISCARD   ESC keep session", "Y VERWERFEN   ESC Session behalten"},
    {"SETTINGS", "OPTIONEN"},
    {"Language", "Sprache"},
    {"Access Point", "Netzwerk"},
    {"Event log", "Ereignisse"},
    {"Diagnostics", "Diagnose"},
    {"off", "aus"},
    {"low", "leise"},
    {"high", "laut"},
    {"on", "an"},
    {"off", "aus"},
    {"GO >", "LOS >"},
    {";/. move   ,// change   ENTER open   ESC back", ";/. Wahl ,// aendern ENTER oeffnen ESC"},
    {"DIAGNOSTICS", "DIAGNOSE"},
    {"device %s (board %u)", "Geraet %s (Board %u)"},
    {"heap %lu min %lu block %lu", "Heap %lu min %lu Block %lu"},
    {"ws %u auth %u pending %u qmax %u", "WS %u auth %u offen %u qmax %u"},
    {"rate ctl %lu draw %lu chat %lu emoji %lu", "Rate ctl %lu Bild %lu Chat %lu Emoji %lu"},
    {"flow coalesce %lu drop %lu close %lu", "Fluss mix %lu weg %lu zu %lu"},
    {"loop %lums lock %luus sound drop %lu", "Loop %lums Lock %luus Ton weg %lu"},
    {"SD failures %lu checkpoint %lu", "SD Fehler %lu Checkpoint %lu"},
    {"High-water marks since boot.", "Hoechstwerte seit dem Start."},
    {"ESC back", "ESC zurueck"},
    {"EVENT LOG", "EREIGNISSE"},
    {"nothing yet", "noch nichts"},
    {"; older  . newer  ,// page  ESC", "; aelter . neuer ,// Seite ESC"},
    {"AP NAME", "AP-NAME"},
    {"Type a new SSID:", "Neue SSID eingeben:"},
    {"Applying restarts the access point,", "Uebernehmen startet den AP neu,"},
    {"which drops every connected phone.", "dadurch verlieren Handys die Verbindung."},
    {"ENTER apply   DEL erase   ESC cancel", "ENTER OK   DEL loeschen   ESC abbrechen"},
    {"UI memory unavailable", "UI-Speicher nicht verfuegbar"},
    {"JOIN", "DA"},
    {"NAME", "NAME"},
    {"LEAVE", "WEG"},
    {"AP up", "AP an"},
    {"session resumed", "Session fortgesetzt"},
    {"session paused", "Session pausiert"},
    {"AP stopped", "AP aus"},
    {"game changed", "Spiel gewechselt"},
    {"round ended", "Runde beendet"},
    {"resume AP before SSID rename", "AP vor SSID-Aenderung starten"},
    {"archive unavailable", "Archiv nicht verfuegbar"},
    {"archive failed", "Archiv fehlgeschlagen"},
    {"archive skipped by host", "Archiv vom Host uebersprungen"},
    {"generation exhausted", "Generation aufgebraucht"},
    {"session id failed", "Session-ID fehlgeschlagen"},
    {"checkpoint failed", "Checkpoint fehlgeschlagen"},
    {"new session", "neue Session"},
    {"restore storage failed", "Ladespeicher fehlgeschlagen"},
    {"history restored", "Verlauf geladen"},
    {"packs loaded", "Packs geladen"},
    {"language load failed", "Sprache konnte nicht geladen werden"},
    {"Language: %s", "Sprache: %s"},
    {"%s: %s vs %s", "%s: %s gegen %s"},
    {"%s: %s %s", "%s: %s %s"},
    {"%s: %s beat %s", "%s: %s besiegt %s"},
    {"%s: %s beat %s (%s)", "%s: %s besiegt %s (%s)"},
    {"%s: %s / %s draw", "%s: %s / %s unentschieden"},
    {"%s: %s / %s draw (%s)", "%s: %s / %s unentschieden (%s)"},
    {"%s: round %d complete", "%s: Runde %d beendet"},
    {"%s: game complete", "%s: Spiel beendet"},
};

static_assert(sizeof(HA_UI_TEXT) / sizeof(HA_UI_TEXT[0]) == HaUiTextCount,
              "host UI translation table must cover every key");

static inline HaUiLocale haUiLocaleFromCode(const char* code) {
    return code && strcmp(code, "de") == 0 ? HaUiGerman : HaUiEnglish;
}

static inline HaUiLocale haUiActiveLocale() {
    if(haLang >= HA_GENERATED_LANGUAGE_COUNT) return HaUiEnglish;
    return haUiLocaleFromCode(HA_GENERATED_LANGUAGES[haLang].code);
}

static inline const char* haUiTextForLocale(HaUiTextKey key, HaUiLocale locale) {
    const size_t index = (size_t)key;
    if(index >= (size_t)HaUiTextCount) return "";
    const HaUiTextEntry& entry = HA_UI_TEXT[index];
    return locale == HaUiGerman && entry.de && entry.de[0] ? entry.de : entry.en;
}

static inline const char* haUiT(HaUiTextKey key) {
    return haUiTextForLocale(key, haUiActiveLocale());
}

static inline bool haUiHasGermanText(HaUiTextKey key) {
    const size_t index = (size_t)key;
    return index < (size_t)HaUiTextCount && HA_UI_TEXT[index].de && HA_UI_TEXT[index].de[0];
}

struct HaUiGameTranslation {
    const char* key;
    const char* labelDe;
    const char* descDe;
};

static constexpr HaUiGameTranslation HA_UI_GAMES_DE[] = {
    {"trivia", "Trivia", "Quiz, schnellste Antwort gewinnt"},
    {"wyr", "Entweder oder", "Gruppenwahl, A oder B"},
    {"scramble", "Wortsalat", "Wort entwirren"},
    {"spectrum", "Spektrum", "Hinweis geben, Regler raten"},
    {"kmk", "Kiss Marry Kill", "Errate die Wahl eines Spielers"},
    {"react", "Reaktionsduell", "Bei Gruen tippen, Schnellster gewinnt"},
    {"connect4", "Vier gewinnt", "Vier in einer Reihe"},
    {"tictactoe", "Tic-Tac-Toe", "Drei in einer Reihe"},
    {"dots", "Kaestchen", "Die meisten Kaestchen schliessen"},
    {"reversi", "Reversi", "Steine drehen, meiste gewinnt"},
    {"draw", "Malen", "Malen, andere raten"},
    {"pong", "Pong", "Klassisches Paddel-Duell"},
    {"guesscolor", "Farbe raten", "RGB-Farbe treffen"},
    {"battleship", "Schiffe versenken", "Flotte verstecken und versenken"},
    {"chess", "Schach", "1v1, volle Schachregeln"},
    {"none", "Keins (Lobby)", "Nur die Lobby"},
};

static inline const HaUiGameTranslation* haUiFindGameTranslation(const char* key) {
    if(!key) return nullptr;
    for(size_t i = 0; i < sizeof(HA_UI_GAMES_DE) / sizeof(HA_UI_GAMES_DE[0]); i++)
        if(strcmp(HA_UI_GAMES_DE[i].key, key) == 0) return &HA_UI_GAMES_DE[i];
    return nullptr;
}

static inline bool haUiHasGermanGame(const char* key) {
    return haUiFindGameTranslation(key) != nullptr;
}

static inline const char* haUiGameLabelForLocale(
    const char* key,
    const char* fallback,
    HaUiLocale locale) {
    const HaUiGameTranslation* translation =
        locale == HaUiGerman ? haUiFindGameTranslation(key) : nullptr;
    return translation && translation->labelDe && translation->labelDe[0]
               ? translation->labelDe
               : (fallback ? fallback : "");
}

static inline const char* haUiGameDescForLocale(
    const char* key,
    const char* fallback,
    HaUiLocale locale) {
    const HaUiGameTranslation* translation =
        locale == HaUiGerman ? haUiFindGameTranslation(key) : nullptr;
    return translation && translation->descDe && translation->descDe[0]
               ? translation->descDe
               : (fallback ? fallback : "");
}
