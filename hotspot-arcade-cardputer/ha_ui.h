// Cardputer host UI: the screens the Flipper build draws with ViewDispatcher +
// SceneManager, redone for a 240x135 colour panel and a 56-key keyboard.
//
// Drawing happens only from loop(), never from an async callback: the UI takes a
// snapshot of the host mirror under the engine lock, releases it, and draws from
// the copy. That keeps the ~10ms sprite push off the lock, so the WebSocket task
// is never blocked by the screen.
#pragma once
#include <M5Cardputer.h>
#include "ha_host.h"
#include "ha_history.h"
#include "ha_metadata.h"

// ---- implemented in the .ino (they touch the engine / WiFi under the lock) ----
void haHostSelectGame(uint8_t game);
void haHostResetScores();
void haHostRoundEnd();
void haHostCheckpoint();
void haHostApplySsid(const char* ssid);
void haHostTogglePortal();
void haCfgSave(); // persist SSID/audio/language to the redundant config stores
const char* haHostSsid();
String haHostIp();
void haHostSnapshot(HaHost& dst);

// Game/language metadata is generated from tools/content-manifest.json. The UI is
// therefore capacity-safe when a new game id is added; nothing indexes a fixed
// game-id-sized array.
using HaGameItem = HaGeneratedGame;
#define HA_UI_GAMES HA_GENERATED_GAMES
static const int HA_UI_GAME_COUNT = (int)HA_GENERATED_GAME_COUNT;

static const char* haUiGameLabel(uint8_t id) {
    for(int i = 0; i < HA_UI_GAME_COUNT; i++)
        if(HA_UI_GAMES[i].id == id) return HA_UI_GAMES[i].label;
    return "None";
}

enum HaUiView {
    HA_VIEW_DASH,
    HA_VIEW_GAMES,
    HA_VIEW_BOARD,
    HA_VIEW_HISTORY,
    HA_VIEW_HISTORY_DETAIL,
    HA_VIEW_HISTORY_RESTORE,
    HA_VIEW_CONSOLE,
    HA_VIEW_SSID,
    HA_VIEW_SETTINGS,
    HA_VIEW_RESET_CONFIRM
};

// Audio level (0 off / 1 low / 2 high) lives in the .ino (the speaker jingles are
// there); the settings screen reads and cycles it.
extern uint8_t haAudioLevel;

// Content language, selected once in Settings. The firmware bakes every language's
// packs; the host streams only the selected one (English fallback per game). haLang
// indexes the tables below; the .ino owns the variable, persists it, and re-streams
// the packs when haLangDirty is set. Interface text stays English for now.
extern uint8_t haLang;
extern bool haLangDirty; // set by Settings; the .ino reloads packs and clears it
#define HA_LANG_COUNT ((int)HA_GENERATED_LANGUAGE_COUNT)
struct HaLanguageCodeTable {
    const char* operator[](size_t i) const { return HA_GENERATED_LANGUAGES[i].code; }
};
struct HaLanguageNameTable {
    const char* operator[](size_t i) const { return HA_GENERATED_LANGUAGES[i].label; }
};
static const HaLanguageCodeTable HA_LANG_CODE;
static const HaLanguageNameTable HA_LANG_NAME;

#define HA_SET_COUNT 5 // settings rows: SSID, Audio, Language, AP, Event log

static M5Canvas* haUiCanvasStorage = nullptr;
#define haUiCanvas (*haUiCanvasStorage)
static bool haUiSprite = false;
static HaUiView haUiView = HA_VIEW_DASH;
static int haUiCursor = 0;
static int haUiScroll = 0;
static char haUiEdit[33] = "";
static uint8_t haGameSort = 0;       // game picker order: 0 alphabetical, 1 most played
static int haGamesOrder[HA_UI_GAME_COUNT]; // display order, filled per sort mode
static HaHistSession* haUiHistDetailStorage = nullptr;
#define haUiHistDetail (*haUiHistDetailStorage)
static bool haUiRestoreFailed = false;
static HaUiView haUiSettingsReturn = HA_VIEW_SETTINGS;
static HaUiView haUiResetReturn = HA_VIEW_DASH;
static HaHost* haUiSnapStorage = nullptr; // locked copy; never touched by AsyncTCP
#define haUiSnap (*haUiSnapStorage)
static uint32_t haUiDrawnRev = 0xFFFFFFFF;
static uint32_t haUiLastDraw = 0;
static uint32_t haUiLastProbe = 0;
static bool haUiForce = true;

#define HA_UI_W 240
#define HA_UI_H 135
#define HA_UI_ROW 10 // px per list row at the 6x8 font

static lgfx::LovyanGFX* haUiG() {
    return haUiSprite && haUiCanvasStorage
               ? (lgfx::LovyanGFX*)haUiCanvasStorage
               : (lgfx::LovyanGFX*)&M5Cardputer.Display;
}

static void haUiBegin() {
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(90);
    haUiSnapStorage = new(std::nothrow) HaHost{};
    haUiCanvasStorage = new(std::nothrow) M5Canvas(&M5Cardputer.Display);
    if(!haUiSnapStorage) {
        Serial.println("[ha] UI: snapshot allocation failed");
        return;
    }
    // 8bpp keeps the off-screen buffer at ~32KB. 16bpp would be 65KB, which is a
    // lot to hold alongside the WiFi stack and eight WebSocket clients on a board
    // with no PSRAM. If it still can't be had, fall back to drawing direct (which
    // flickers, but works).
    if(haUiCanvasStorage) {
        haUiCanvas.setPsram(false);
        haUiCanvas.setColorDepth(8);
        haUiSprite = haUiCanvas.createSprite(HA_UI_W, HA_UI_H) != nullptr;
    }
    haUiG()->setTextFont(1);
    haUiG()->setTextSize(1);
}

static bool haUiEnsureHistDetail() {
    if(haUiHistDetailStorage) return true;
    haUiHistDetailStorage = new(std::nothrow) HaHistSession{};
    return haUiHistDetailStorage != nullptr;
}

// ---- drawing ---------------------------------------------------------------

// The phone client's palette is monochrome + one hot orange (#FF8200). Match it on
// the host screen: orange is the single accent (title bar, selection, leader) on a
// black field with white text. 0xFC00 is #FF8200 in RGB565. AP up/down stays
// green/red -- that's a status, and the web uses the same good/bad colours.
#define HA_ORANGE 0xFC00

static void haUiHeader(lgfx::LovyanGFX* g, const char* title) {
    g->fillRect(0, 0, HA_UI_W, 12, HA_ORANGE);
    g->setTextColor(TFT_BLACK, HA_ORANGE);
    g->drawString(title, 3, 2);
    char bat[8];
    snprintf(bat, sizeof(bat), "%d%%", (int)M5Cardputer.Power.getBatteryLevel());
    g->drawString(bat, HA_UI_W - 6 * (int)strlen(bat) - 3, 2);
}

static void haUiFooter(lgfx::LovyanGFX* g, const char* hint) {
    g->fillRect(0, HA_UI_H - 11, HA_UI_W, 11, TFT_DARKGREY);
    g->setTextColor(TFT_WHITE, TFT_DARKGREY);
    g->drawString(hint, 3, HA_UI_H - 9);
}

// Player order for both the dashboard and the leaderboard: score desc, then pid,
// so the board doesn't reshuffle on every tie.
static int haUiSorted(uint8_t* out) {
    int n = 0;
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
        if(haUiSnap.p[pid].used) out[n++] = pid;
    for(int i = 1; i < n; i++) {
        uint8_t k = out[i];
        int j = i - 1;
        while(j >= 0 && haUiSnap.p[out[j]].score < haUiSnap.p[k].score) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

// Cumulative session order includes people who temporarily disconnected. It is
// distinct from the live pid roster shown on the dashboard.
static int haUiSortedSession(uint8_t* out) {
    int n = 0;
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        if(haUiSnap.session[i].used) out[n++] = i;
    for(int i = 1; i < n; i++) {
        uint8_t k = out[i];
        int j = i - 1;
        while(j >= 0 &&
              (haUiSnap.session[out[j]].score < haUiSnap.session[k].score ||
               (haUiSnap.session[out[j]].score == haUiSnap.session[k].score &&
                strcmp(haUiSnap.session[out[j]].nick, haUiSnap.session[k].nick) > 0))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

// Two-column live scoreboard at the small font, so all 10 (the softAP max) fit on
// one screen. Columns fill in rank order down the left, then down the right; each
// cell reads "rank.nick:score".
static void haUiDrawScoreCols(lgfx::LovyanGFX* g, uint8_t* order, int n, int top, int rowsPerCol) {
    g->setTextSize(1);
    const int rowH = 13;
    for(int i = 0; i < n && i < rowsPerCol * 2; i++) {
        int col = i / rowsPerCol, row = i % rowsPerCol;
        int x = col ? HA_UI_W / 2 + 4 : 3;
        int y = top + row * rowH;
        const HaHostPlayer& p = haUiSnap.p[order[i]];
        g->setTextColor(i == 0 ? HA_ORANGE : TFT_WHITE, TFT_BLACK);
        char nk[10], cell[24];
        snprintf(nk, sizeof(nk), "%s", p.nick); // clip nick to ~9 chars per column
        snprintf(cell, sizeof(cell), "%d.%s:%ld", i + 1, nk, (long)p.score);
        g->drawString(cell, x, y);
    }
}

static void haUiDrawSessionRows(lgfx::LovyanGFX* g, uint8_t* order, int n) {
    const int rows = 10;
    int maxScroll = n > rows ? n - rows : 0;
    if(haUiScroll < 0) haUiScroll = 0;
    if(haUiScroll > maxScroll) haUiScroll = maxScroll;
    for(int row = 0; row < rows && haUiScroll + row < n; row++) {
        int rank = haUiScroll + row;
        const HaHostSessionPlayer& p = haUiSnap.session[order[rank]];
        char line[40];
        snprintf(
            line,
            sizeof(line),
            "%2d %c %-18.18s %ld",
            rank + 1,
            p.connected ? '*' : ' ',
            p.nick,
            (long)p.score);
        g->setTextColor(rank == 0 ? HA_ORANGE : TFT_WHITE, TFT_BLACK);
        g->drawString(line, 3, 15 + row * HA_UI_ROW);
    }
}

static void haUiDrawDash(lgfx::LovyanGFX* g) {
    haUiHeader(g, "HOTSPOT ARCADE");
    g->setTextFont(1);
    g->setTextSize(1);

    // Line 1: SSID + the join URL, on one line.
    char line[80];
    g->setTextColor(haUiSnap.portalRunning ? TFT_GREEN : TFT_RED, TFT_BLACK);
    snprintf(line, sizeof(line), "%s  http://%s", haHostSsid(), haHostIp().c_str());
    g->drawString(line, 3, 15);

    // Line 2: active game + player count.
    uint8_t order[HA_MAX_PLAYERS + 1];
    int n = haUiSorted(order);
    g->setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(line, sizeof(line), "Game: %s", haUiGameLabel(haUiSnap.activeGame));
    g->drawString(line, 3, 27);
    char pl[20]; // players pinned to the right edge so the two never crowd
    snprintf(pl, sizeof(pl), "Players: %d", n);
    g->drawString(pl, HA_UI_W - 6 * (int)strlen(pl) - 3, 27);

    g->drawFastHLine(0, 38, HA_UI_W, TFT_DARKGREY);

    if(n == 0) {
        g->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g->drawString("waiting for phones to join...", 3, 46);
    } else {
        haUiDrawScoreCols(g, order, n, 44, 5); // 2 columns x 5 = up to 10
        if(n > 10) {
            g->setTextColor(TFT_DARKGREY, TFT_BLACK);
            snprintf(line, sizeof(line), "+%d more", n - 10);
            g->drawString(line, 3, HA_UI_H - 22);
        }
    }

    if(haUiSnap.lastEvent[0]) {
        g->setTextFont(1);
        g->setTextSize(1);
        g->setTextColor(HA_ORANGE, TFT_BLACK);
        g->drawString(haUiSnap.lastEvent, 3, HA_UI_H - 22);
    }
    haUiFooter(g, "G game L board H history S settings");
}

#define HA_GAMES_ROW 16 // px per game row at text size 2

// Fill haGamesOrder for the current sort mode. "None (lobby)" is always kept last.
static void haUiComputeGamesOrder() {
    int m = 0;
    for(int i = 0; i < HA_UI_GAME_COUNT; i++)
        if(HA_UI_GAMES[i].id != HA_GAME_NONE) haGamesOrder[m++] = i;
    // insertion sort: alphabetical by label, or by play count desc
    for(int a = 1; a < m; a++) {
        int k = haGamesOrder[a], j = a - 1;
        while(j >= 0) {
            bool swap;
            if(haGameSort == 1) {
                uint16_t left = haHostGamePlayCount(
                    haUiSnap,
                    HA_UI_GAMES[haGamesOrder[j]].id);
                uint16_t right = haHostGamePlayCount(haUiSnap, HA_UI_GAMES[k].id);
                swap = left < right ||
                       (left == right &&
                        strcmp(HA_UI_GAMES[haGamesOrder[j]].label,
                               HA_UI_GAMES[k].label) > 0);
            } else {
                swap = strcmp(HA_UI_GAMES[haGamesOrder[j]].label, HA_UI_GAMES[k].label) > 0;
            }
            if(!swap) break;
            haGamesOrder[j + 1] = haGamesOrder[j];
            j--;
        }
        haGamesOrder[j + 1] = k;
    }
    for(int i = 0; i < HA_UI_GAME_COUNT; i++) // append None last
        if(HA_UI_GAMES[i].id == HA_GAME_NONE) haGamesOrder[m++] = i;
}

static void haUiDrawGames(lgfx::LovyanGFX* g) {
    haUiComputeGamesOrder();
    char title[32];
    snprintf(title, sizeof(title), "GAMES - %s", haGameSort == 1 ? "MOST PLAYED" : "A-Z");
    haUiHeader(g, title);

    int descY = HA_UI_H - 22;
    int rows = (descY - 14) / HA_GAMES_ROW;
    if(rows < 1) rows = 1;
    if(haUiCursor < haUiScroll) haUiScroll = haUiCursor;
    if(haUiCursor >= haUiScroll + rows) haUiScroll = haUiCursor - rows + 1;

    g->setTextSize(2);
    int y = 15;
    for(int i = haUiScroll; i < HA_UI_GAME_COUNT && i < haUiScroll + rows; i++) {
        const HaGameItem& it = HA_UI_GAMES[haGamesOrder[i]];
        bool sel = (i == haUiCursor);
        bool live = (it.id == haUiSnap.activeGame);
        if(sel) g->fillRect(0, y - 1, HA_UI_W, HA_GAMES_ROW, HA_ORANGE);
        g->setTextColor(sel ? TFT_BLACK : (live ? HA_ORANGE : TFT_WHITE), sel ? HA_ORANGE : TFT_BLACK);
        char nm[18];
        snprintf(nm, sizeof(nm), "%s%s", live ? "*" : "", it.label);
        g->drawString(nm, 3, y);
        if(it.duel) {
            g->setTextSize(1);
            g->setTextColor(sel ? TFT_BLACK : HA_ORANGE, sel ? HA_ORANGE : TFT_BLACK);
            g->drawString("1v1", HA_UI_W - 22, y + 4);
            g->setTextSize(2);
        }
        y += HA_GAMES_ROW;
    }
    g->setTextSize(1);

    // Selected game's one-line description.
    g->fillRect(0, descY - 2, HA_UI_W, 12, TFT_BLACK);
    g->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g->drawString(HA_UI_GAMES[haGamesOrder[haUiCursor]].desc, 3, descY);

    haUiFooter(g, ";/. move  S sort  ENTER pick  ESC back");
}

// The leaderboard is the cumulative host session, including temporarily offline
// players. Opening it requests a durable checkpoint; R archives it before reset.
static void haUiDrawBoard(lgfx::LovyanGFX* g) {
    haUiHeader(g, "SESSION LEADERBOARD");
    uint8_t order[HA_SESSION_MAX_PLAYERS];
    int n = haUiSortedSession(order);
    if(n == 0) {
        g->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g->drawString("no players yet", 3, 18);
    } else {
        haUiDrawSessionRows(g, order, n);
    }
    haUiFooter(g, haSdOk ? ";/. scroll R new session ESC" : ";/. scroll R reset (no SD)");
}

static void haUiDrawHistory(lgfx::LovyanGFX* g) {
    char title[32];
    uint32_t total = haHistStorageReady() ? haHist.total : 0;
    snprintf(title, sizeof(title), "HISTORY - %lu", (unsigned long)total);
    haUiHeader(g, title);
    if(!haSdOk) {
        g->setTextColor(TFT_RED, TFT_BLACK);
        g->drawString("microSD unavailable", 3, 20);
    } else if(!haHistStorageReady() || !haHist.count) {
        g->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g->drawString("no archived sessions", 3, 20);
        g->drawString("R on the leaderboard starts one", 3, 32);
    } else {
        if(haUiCursor < 0) haUiCursor = 0;
        if(haUiCursor >= haHist.count) haUiCursor = haHist.count - 1;
        for(uint8_t i = 0; i < haHist.count; i++) {
            const HaHistSummary& s = haHist.s[i];
            int y = 15 + i * 17;
            bool selected = i == haUiCursor;
            if(selected) g->fillRect(0, y - 1, HA_UI_W, 16, HA_ORANGE);
            g->setTextColor(selected ? TFT_BLACK : TFT_WHITE, selected ? HA_ORANGE : TFT_BLACK);
            char line[44];
            snprintf(
                line,
                sizeof(line),
                "#%-5lu %-15.15s %uP",
                (unsigned long)s.num,
                haUiGameLabel(s.game),
                (unsigned)s.count);
            g->drawString(line, 3, y + 1);
            char score[14];
            snprintf(score, sizeof(score), "%ld", (long)s.leaderScore);
            g->drawString(score, HA_UI_W - 6 * (int)strlen(score) - 3, y + 1);
        }
    }
    haUiFooter(g, ";/. move ,// page ENTER view ESC");
}

static void haUiDrawHistoryDetail(lgfx::LovyanGFX* g) {
    if(!haUiHistDetailStorage) return;
    char title[32];
    snprintf(title, sizeof(title), "SESSION #%lu", (unsigned long)haUiHistDetail.num);
    haUiHeader(g, title);
    const int rows = 9;
    int maxScroll = haUiHistDetail.count > rows ? haUiHistDetail.count - rows : 0;
    if(haUiScroll < 0) haUiScroll = 0;
    if(haUiScroll > maxScroll) haUiScroll = maxScroll;
    g->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g->drawString(haUiGameLabel(haUiHistDetail.game), 3, 15);
    for(int row = 0; row < rows && haUiScroll + row < haUiHistDetail.count; row++) {
        int rank = haUiScroll + row;
        const HaHistPlayer& p = haUiHistDetail.p[rank];
        char line[40];
        snprintf(line, sizeof(line), "%2d  %-20.20s %ld", rank + 1, p.nick, (long)p.score);
        g->setTextColor(rank == 0 ? HA_ORANGE : TFT_WHITE, TFT_BLACK);
        g->drawString(line, 3, 27 + row * HA_UI_ROW);
    }
    haUiFooter(
        g,
        haHistCanRestore() ? ";/. scroll Y restore ESC" : ";/. scroll ESC (restore offline)");
}

static void haUiDrawHistoryRestore(lgfx::LovyanGFX* g) {
    if(!haUiHistDetailStorage) return;
    haUiHeader(g, "RESTORE SESSION?");
    g->setTextSize(2);
    g->setTextColor(HA_ORANGE, TFT_BLACK);
    char line[28];
    snprintf(line, sizeof(line), "Session #%lu", (unsigned long)haUiHistDetail.num);
    g->drawString(line, 10, 26);
    g->setTextSize(1);
    g->setTextColor(TFT_WHITE, TFT_BLACK);
    g->drawString("Current play is archived first.", 10, 55);
    g->drawString("Phones reconnect to restored scores.", 10, 67);
    if(haUiRestoreFailed) {
        g->setTextColor(TFT_RED, TFT_BLACK);
        g->drawString("Restore failed; active play unchanged.", 10, 83);
    }
    haUiFooter(g, "Y confirm   ESC cancel");
}

static void haUiDrawResetConfirm(lgfx::LovyanGFX* g) {
    haUiHeader(g, "NEW SESSION?");
    g->setTextSize(2);
    g->setTextColor(HA_ORANGE, TFT_BLACK);
    g->drawString("Archive scores", 12, 28);
    g->setTextSize(1);
    g->setTextColor(TFT_WHITE, TFT_BLACK);
    g->drawString("Then reset cumulative session scores.", 12, 57);
    if(!haSdOk) {
        g->setTextColor(TFT_RED, TFT_BLACK);
        g->drawString("No SD: this session cannot be archived.", 12, 73);
    }
    haUiFooter(g, "Y confirm   ESC cancel");
}

// One option of a multi-choice setting (audio off/low/high, AP on/off). The current
// choice is filled -- orange on the selected (editable) row, grey otherwise; the rest
// are outlined. Returns the x just past the pill, so options tile left to right.
static int haUiOptPill(lgfx::LovyanGFX* g, int x, int y, const char* txt, bool current, bool rowSel) {
    int w = (int)g->textWidth(txt) + 8;
    if(current) {
        uint16_t bg = rowSel ? HA_ORANGE : TFT_DARKGREY;
        g->fillRoundRect(x, y, w, 13, 2, bg);
        g->setTextColor(rowSel ? TFT_BLACK : TFT_WHITE, bg);
    } else {
        g->drawRoundRect(x, y, w, 13, 2, TFT_DARKGREY);
        g->setTextColor(TFT_DARKGREY, TFT_BLACK);
    }
    g->drawString(txt, x + 4, y + 3);
    return x + w + 4;
}

// A single value pill (language, SSID, event-log). Orange fill on the selected row.
// `arrows` frames it with < > (a cycle-able value like language) when it's selected.
static void haUiValPill(lgfx::LovyanGFX* g, int x, int y, const char* txt, bool rowSel, bool arrows) {
    int tx = x;
    if(arrows && rowSel) {
        g->setTextColor(HA_ORANGE, TFT_BLACK);
        g->drawString("<", x, y + 3);
        tx = x + 10;
    }
    int w = (int)g->textWidth(txt) + 8;
    if(rowSel) {
        g->fillRoundRect(tx, y, w, 13, 2, HA_ORANGE);
        g->setTextColor(TFT_BLACK, HA_ORANGE);
    } else {
        g->drawRoundRect(tx, y, w, 13, 2, TFT_DARKGREY);
        g->setTextColor(TFT_WHITE, TFT_BLACK);
    }
    g->drawString(txt, tx + 4, y + 3);
    if(arrows && rowSel) {
        g->setTextColor(HA_ORANGE, TFT_BLACK);
        g->drawString(">", tx + w + 3, y + 3);
    }
}

static void haUiDrawSettings(lgfx::LovyanGFX* g) {
    haUiHeader(g, "SETTINGS");
    g->setTextSize(1);
    const int VALX = 92, y0 = 20, rowH = 20;
    const char* labels[HA_SET_COUNT] = {"SSID", "Audio", "Language", "Access Point", "Event log"};
    for(int i = 0; i < HA_SET_COUNT; i++) {
        bool sel = (i == haUiCursor);
        int y = y0 + i * rowH;
        g->setTextColor(sel ? HA_ORANGE : TFT_WHITE, TFT_BLACK);
        g->drawString(labels[i], 4, y + 3);
        int cx = VALX;
        switch(i) {
        case 0: { // SSID -- value shown; ENTER opens the text editor
            char s[16];
            snprintf(s, sizeof(s), "%.12s", haHostSsid());
            haUiValPill(g, cx, y, s, sel, false);
            break;
        }
        case 1: { // Audio -- off / low / high
            const char* o[3] = {"off", "low", "high"};
            for(int a = 0; a < 3; a++) cx = haUiOptPill(g, cx, y, o[a], haAudioLevel == a, sel);
            break;
        }
        case 2: // Language -- one box, cycles with < >
            haUiValPill(g, cx, y, HA_LANG_NAME[haLang % HA_LANG_COUNT], sel, true);
            break;
        case 3: { // Access Point -- on / off
            bool up = haUiSnap.portalRunning;
            cx = haUiOptPill(g, cx, y, "on", up, sel);
            cx = haUiOptPill(g, cx, y, "off", !up, sel);
            break;
        }
        case 4: // Event log -- opens a sub-screen
            haUiValPill(g, cx, y, "GO >", sel, false);
            break;
        }
    }
    haUiFooter(g, ";/. move   ,// change   ENTER open   ESC back");
}

static void haUiDrawConsole(lgfx::LovyanGFX* g) {
    haUiHeader(g, "EVENT LOG");
    int rows = 11;
    uint32_t total = haUiSnap.evTotal;
    uint32_t have = total < HA_EV_MAX ? total : HA_EV_MAX;
    int maxScroll = have > (uint32_t)rows ? (int)have - rows : 0;
    if(haUiScroll < 0) haUiScroll = 0;
    if(haUiScroll > maxScroll) haUiScroll = maxScroll;
    int y = 14;
    g->setTextColor(TFT_WHITE, TFT_BLACK);
    for(uint32_t i = (uint32_t)haUiScroll;
        i < have && i < (uint32_t)(haUiScroll + rows);
        i++) {
        // newest first
        uint32_t idx = (total - 1 - i) % HA_EV_MAX;
        g->drawString(haUiSnap.ev[idx], 3, y);
        y += HA_UI_ROW;
    }
    if(have == 0) {
        g->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g->drawString("nothing yet", 3, 16);
    }
    haUiFooter(g, "; older  . newer  ,// page  ESC");
}

static void haUiDrawSsid(lgfx::LovyanGFX* g) {
    haUiHeader(g, "AP NAME");
    g->setTextColor(TFT_WHITE, TFT_BLACK);
    g->drawString("Type a new SSID:", 3, 20);
    g->fillRect(3, 34, HA_UI_W - 6, 14, TFT_DARKGREY);
    g->setTextColor(TFT_WHITE, TFT_DARKGREY);
    char shown[40];
    snprintf(shown, sizeof(shown), "%s_", haUiEdit);
    g->drawString(shown, 6, 37);
    g->setTextColor(TFT_DARKGREY, TFT_BLACK);
    g->drawString("Applying restarts the access point,", 3, 58);
    g->drawString("which drops every connected phone.", 3, 68);
    haUiFooter(g, "ENTER apply   DEL erase   ESC cancel");
}

static void haUiDraw(bool snapshot = true) {
    if(!haUiSnapStorage || !haHostStorage) {
        M5Cardputer.Display.fillScreen(TFT_BLACK);
        M5Cardputer.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5Cardputer.Display.drawString("UI memory unavailable", 3, 20);
        return;
    }
    if(snapshot) haHostSnapshot(haUiSnap);
    lgfx::LovyanGFX* g = haUiG();
    if(haUiSprite) haUiCanvas.fillSprite(TFT_BLACK);
    else g->fillScreen(TFT_BLACK);
    g->setTextFont(1);
    g->setTextSize(1);
    switch(haUiView) {
    case HA_VIEW_GAMES:
        haUiDrawGames(g);
        break;
    case HA_VIEW_BOARD:
        haUiDrawBoard(g);
        break;
    case HA_VIEW_HISTORY:
        haUiDrawHistory(g);
        break;
    case HA_VIEW_HISTORY_DETAIL:
        haUiDrawHistoryDetail(g);
        break;
    case HA_VIEW_HISTORY_RESTORE:
        haUiDrawHistoryRestore(g);
        break;
    case HA_VIEW_CONSOLE:
        haUiDrawConsole(g);
        break;
    case HA_VIEW_SSID:
        haUiDrawSsid(g);
        break;
    case HA_VIEW_SETTINGS:
        haUiDrawSettings(g);
        break;
    case HA_VIEW_RESET_CONFIRM:
        haUiDrawResetConfirm(g);
        break;
    default:
        haUiDrawDash(g);
        break;
    }
    if(haUiSprite) haUiCanvas.pushSprite(0, 0);
    haUiDrawnRev = haUiSnap.rev;
    haUiLastDraw = millis();
    haUiForce = false;
}

// ---- input -----------------------------------------------------------------

static void haUiOpen(HaUiView v) {
    haUiView = v;
    haUiCursor = 0;
    haUiScroll = 0;
    if(v == HA_VIEW_GAMES) {
        haUiComputeGamesOrder(); // cursor is a position in the sorted display order
        for(int i = 0; i < HA_UI_GAME_COUNT; i++)
            if(HA_UI_GAMES[haGamesOrder[i]].id == haUiSnap.activeGame) haUiCursor = i;
    } else if(v == HA_VIEW_BOARD) {
        haHostSnapshot(haUiSnap);
        haHostCheckpoint(); // forced only when durable host fields have changed
    } else if(v == HA_VIEW_HISTORY) {
        haHistCatalogNewest();
    }
    haUiForce = true;
}

static void haUiBack() {
    switch(haUiView) {
    case HA_VIEW_SSID:
    case HA_VIEW_CONSOLE:
        haUiOpen(haUiSettingsReturn);
        break;
    case HA_VIEW_HISTORY_DETAIL:
        haUiView = HA_VIEW_HISTORY;
        haUiScroll = 0;
        haUiForce = true;
        break;
    case HA_VIEW_HISTORY_RESTORE:
        haUiView = HA_VIEW_HISTORY_DETAIL;
        haUiScroll = 0;
        haUiForce = true;
        break;
    case HA_VIEW_RESET_CONFIRM:
        haUiOpen(haUiResetReturn);
        break;
    default:
        haUiOpen(HA_VIEW_DASH);
        break;
    }
}

// Change the value of the selected settings row in place (the ,/ left-right keys).
// SSID and Event log aren't values -- they open a screen on ENTER, so adjust skips them.
static void haUiSettingAdjust(int dir) {
    switch(haUiCursor) {
    case 1: // Audio off/low/high
        haAudioLevel = (uint8_t)((haAudioLevel + 3 + dir) % 3);
        haCfgSave();
        break;
    case 2: // Language
        haLang = (uint8_t)((haLang + HA_LANG_COUNT + dir) % HA_LANG_COUNT);
        haLangDirty = true;
        haCfgSave();
        break;
    case 3: // Access Point on/off
        haHostTogglePortal();
        break;
    default:
        return;
    }
    haUiForce = true;
}

static void haUiChar(char c) {
    if(haUiView == HA_VIEW_SSID) {
        if(c == '`') { // esc -> back to settings
            haUiBack();
            return;
        }
        size_t n = strlen(haUiEdit);
        if(c >= 0x20 && c < 0x7F && n < sizeof(haUiEdit) - 1) {
            haUiEdit[n] = c;
            haUiEdit[n + 1] = '\0';
            haUiForce = true;
        }
        return;
    }

    if(haUiView == HA_VIEW_HISTORY_RESTORE) {
        if(c == '`') haUiBack();
        else if(c == 'y' || c == 'Y') {
            haUiRestoreFailed = !haUiHistDetailStorage ||
                                !haHistRequestRestore(haUiHistDetail);
            if(!haUiRestoreFailed) haUiOpen(HA_VIEW_BOARD);
            else haUiForce = true;
        }
        return;
    }
    if(haUiView == HA_VIEW_RESET_CONFIRM) {
        if(c == '`') haUiBack();
        else if(c == 'y' || c == 'Y') {
            haHostResetScores();
            haUiOpen(HA_VIEW_BOARD);
        }
        return;
    }

    switch(c) {
    case '`': // esc
        haUiBack();
        return;
    case ';': // up
        if(haUiView == HA_VIEW_GAMES && haUiCursor > 0) haUiCursor--;
        else if(haUiView == HA_VIEW_SETTINGS && haUiCursor > 0) haUiCursor--;
        else if(haUiView == HA_VIEW_BOARD && haUiScroll > 0) haUiScroll--;
        else if(haUiView == HA_VIEW_CONSOLE) haUiScroll++;
        else if(haUiView == HA_VIEW_HISTORY) {
            if(haUiCursor > 0) haUiCursor--;
            else if(haHistStorageReady() && haHist.hasNewer && haHistCatalogNewer())
                haUiCursor = haHist.count - 1;
        } else if(haUiView == HA_VIEW_HISTORY_DETAIL && haUiScroll > 0) haUiScroll--;
        haUiForce = true;
        return;
    case '.': // down
        if(haUiView == HA_VIEW_GAMES && haUiCursor < HA_UI_GAME_COUNT - 1) haUiCursor++;
        else if(haUiView == HA_VIEW_SETTINGS && haUiCursor < HA_SET_COUNT - 1) haUiCursor++;
        else if(haUiView == HA_VIEW_BOARD) haUiScroll++;
        else if(haUiView == HA_VIEW_CONSOLE && haUiScroll > 0) haUiScroll--;
        else if(haUiView == HA_VIEW_HISTORY) {
            if(haHistStorageReady() && haUiCursor + 1 < haHist.count) haUiCursor++;
            else if(haHistStorageReady() && haHist.hasOlder && haHistCatalogOlder())
                haUiCursor = 0;
        } else if(haUiView == HA_VIEW_HISTORY_DETAIL) haUiScroll++;
        haUiForce = true;
        return;
    case ',': // left: page up in the picker, or decrement a setting value
        if(haUiView == HA_VIEW_GAMES) haUiCursor = haUiCursor > 6 ? haUiCursor - 6 : 0;
        else if(haUiView == HA_VIEW_SETTINGS) haUiSettingAdjust(-1);
        else if(haUiView == HA_VIEW_BOARD)
            haUiScroll = haUiScroll > 10 ? haUiScroll - 10 : 0;
        else if(haUiView == HA_VIEW_CONSOLE) haUiScroll += 11;
        else if(haUiView == HA_VIEW_HISTORY && haHistStorageReady() &&
                haHist.hasNewer && haHistCatalogNewer())
            haUiCursor = 0;
        else if(haUiView == HA_VIEW_HISTORY_DETAIL)
            haUiScroll = haUiScroll > 9 ? haUiScroll - 9 : 0;
        haUiForce = true;
        return;
    case '/': // right: page down in the picker, or increment a setting value
        if(haUiView == HA_VIEW_GAMES)
            haUiCursor = haUiCursor + 6 < HA_UI_GAME_COUNT ? haUiCursor + 6 : HA_UI_GAME_COUNT - 1;
        else if(haUiView == HA_VIEW_SETTINGS) haUiSettingAdjust(1);
        else if(haUiView == HA_VIEW_BOARD) haUiScroll += 10;
        else if(haUiView == HA_VIEW_CONSOLE)
            haUiScroll = haUiScroll > 11 ? haUiScroll - 11 : 0;
        else if(haUiView == HA_VIEW_HISTORY && haHistStorageReady() &&
                haHist.hasOlder && haHistCatalogOlder())
            haUiCursor = 0;
        else if(haUiView == HA_VIEW_HISTORY_DETAIL) haUiScroll += 9;
        haUiForce = true;
        return;
    case 'g':
    case 'G':
        haUiOpen(HA_VIEW_GAMES);
        return;
    case 'l':
    case 'L':
        haUiOpen(HA_VIEW_BOARD);
        return;
    case 'h':
    case 'H':
        haUiOpen(HA_VIEW_HISTORY);
        return;
    case 'c':
    case 'C':
        haUiSettingsReturn = haUiView == HA_VIEW_SETTINGS ? HA_VIEW_SETTINGS : HA_VIEW_DASH;
        haUiView = HA_VIEW_CONSOLE;
        haUiScroll = 0;
        haUiForce = true;
        return;
    case 'n':
    case 'N':
        haUiSettingsReturn = haUiView == HA_VIEW_SETTINGS ? HA_VIEW_SETTINGS : HA_VIEW_DASH;
        strlcpy(haUiEdit, haHostSsid(), sizeof(haUiEdit));
        haUiView = HA_VIEW_SSID;
        haUiForce = true;
        return;
    case 'p':
    case 'P':
        haHostTogglePortal();
        haUiForce = true;
        return;
    case 's':
    case 'S':
        if(haUiView == HA_VIEW_GAMES) { // in the picker, S toggles the sort order
            haGameSort ^= 1;
            haUiCursor = 0;
            haUiScroll = 0;
        } else {
            haUiOpen(HA_VIEW_SETTINGS);
        }
        haUiForce = true;
        return;
    case 'r':
    case 'R':
        haUiResetReturn = haUiView == HA_VIEW_BOARD ? HA_VIEW_BOARD : HA_VIEW_DASH;
        haUiView = HA_VIEW_RESET_CONFIRM;
        haUiForce = true;
        return;
    case 'y':
    case 'Y':
        if(haUiView == HA_VIEW_HISTORY_DETAIL && haHistCanRestore()) {
            haUiRestoreFailed = false;
            haUiView = HA_VIEW_HISTORY_RESTORE;
            haUiForce = true;
        }
        return;
    case 'e':
    case 'E':
        haHostRoundEnd();
        haUiForce = true;
        return;
    default:
        return;
    }
}

static void haUiEnter() {
    if(haUiView == HA_VIEW_GAMES) {
        int gameIndex = haGamesOrder[haUiCursor];
        const HaGameItem& it = HA_UI_GAMES[gameIndex];
        haHostSelectGame(it.id);
        haUiOpen(HA_VIEW_DASH);
    } else if(haUiView == HA_VIEW_HISTORY && haHistStorageReady() && haHist.count) {
        if(haUiEnsureHistDetail() && haUiCursor >= 0 && haUiCursor < haHist.count &&
           haHistLoadSession(haHist.s[haUiCursor].num, haUiHistDetail)) {
            haUiView = HA_VIEW_HISTORY_DETAIL;
            haUiScroll = 0;
        }
    } else if(haUiView == HA_VIEW_SSID) {
        if(haUiEdit[0]) haHostApplySsid(haUiEdit);
        haUiOpen(haUiSettingsReturn);
    } else if(haUiView == HA_VIEW_SETTINGS) {
        switch(haUiCursor) {
        case 0: // SSID -> open the editor
            haUiSettingsReturn = HA_VIEW_SETTINGS;
            strlcpy(haUiEdit, haHostSsid(), sizeof(haUiEdit));
            haUiView = HA_VIEW_SSID;
            break;
        case 1: // Audio -> cycle off/low/high
            haAudioLevel = (uint8_t)((haAudioLevel + 1) % 3);
            haCfgSave();
            break;
        case 2: // Language -> cycle, persist, and ask the .ino to re-stream packs
            haLang = (uint8_t)((haLang + 1) % HA_LANG_COUNT);
            haLangDirty = true;
            haCfgSave();
            break;
        case 3: // AP -> toggle
            haHostTogglePortal();
            break;
        case 4: // Event log
            haUiSettingsReturn = HA_VIEW_SETTINGS;
            haUiView = HA_VIEW_CONSOLE;
            haUiScroll = 0;
            break;
        }
    }
    haUiForce = true;
}

static void haUiDel() {
    if(haUiView == HA_VIEW_SSID) {
        size_t n = strlen(haUiEdit);
        if(n) haUiEdit[n - 1] = '\0';
    } else {
        haUiBack();
    }
    haUiForce = true;
}

static void haUiPumpKeys() {
    if(!haUiSnapStorage || !haHostStorage) return;
    if(!M5Cardputer.Keyboard.isChange()) return;
    if(!M5Cardputer.Keyboard.isPressed()) return;
    auto st = M5Cardputer.Keyboard.keysState();
    for(auto c : st.word) haUiChar(c);
    if(st.del) haUiDel();
    if(st.enter) haUiEnter();
}

// Redraw when the mirror moved or a key changed the view, rate-limited so a busy
// game (pong ticks at 30Hz) can't spend all its time pushing pixels. The 1Hz
// floor keeps the battery percentage honest.
static void haUiTick() {
    if(!haUiSnapStorage || !haHostStorage) return;
    uint32_t now = millis();
    if(!haUiForce && now - haUiLastProbe < 50) return;
    haUiLastProbe = now;
    haHostSnapshot(haUiSnap);
    bool changed = haUiForce || haUiSnap.rev != haUiDrawnRev;
    if(changed && now - haUiLastDraw < 100) return;
    if(!changed && now - haUiLastDraw < 1000) return;
    haUiDraw(false);
}
