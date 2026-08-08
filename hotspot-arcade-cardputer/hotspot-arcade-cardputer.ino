// Hotspot Arcade firmware for the M5Stack Cardputer (ESP32-S3).
//
// Same game engine as esp32/hotspot-arcade-fw, collapsed onto one device: the
// Cardputer runs the open AP + captive portal + WebSocket referee AND is its own
// host, so there is no Flipper, no UART link, and nothing to flash a second board
// with. The web bundle and the content packs are baked into flash by
// tools/gen-cardputer-assets.mjs instead of being streamed in at session start.
//
// The engine (ha_games.h) is untouched. It reports to its host through the same
// six haUart* sinks; here they write into the on-screen mirror (ha_host.h) rather
// than framing UART bytes. docs/PROTOCOL.md still describes the message set --
// this build just delivers it by function call.
//
// For education/fun on your own hardware. It runs an OPEN access point and a
// catch-all captive page; only operate it where that is allowed.

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <M5Cardputer.h>

// Cardputer capacity is bounded by the softAP's ten associated stations. Keep
// the engine's fixed-size state proportional to that hardware limit while still
// allowing every connected player to be placed in a concurrent matchup.
#define HA_MAX_PLAYERS 10
#define DUEL_MAX_MATCHES 5
#define PONG_MAX 5
#define BATTLE_MAX 5
#define CHESS_MAX 5

#include "ha_proto.h"
#include "ha_json.h"
#include "ha_bundle.h"
#include "ha_games.h"
#include "ha_host.h"
#include "ha_ap_reconnect.h"
#include "ha_history.h"
#include "ha_ssid_transaction.h"
#include "ha_content.h"
#include "ha_event_format.h"
#include "ha_network_policy.h"
#include "ha_runtime_types.h"
#include "ha_ui.h"

#define WS_MSG_MAX 512
// 10 is the ESP32-S3 softAP hardware maximum (ESP_WIFI_MAX_CONN_NUM). More phones
// than this cannot associate no matter what -- the chip, not the code, is the cap.
#define AP_MAX_CONN 10

// ---- host speaker: short jingles, respecting the audio level set in the UI ----
// 0 = off, 1 = low, 2 = high. Stored here; the UI settings screen changes it.
uint8_t haAudioLevel = 1;

// Content language (see ha_ui.h): 0 English, 1 Deutsch. This tranche persists
// settings to microSD; the next stacked change adds the bounded NVS fallback.
// Settings changes set haLangDirty so loop() can re-stream the packs.
uint8_t haLang = 0;
bool haLangDirty = false;
static uint8_t haLoadedLang = 0;

static void haBeep(uint16_t freq, uint16_t ms) {
    if(haAudioLevel == 0) return;
    M5Cardputer.Speaker.setVolume(haAudioLevel == 2 ? 200 : 80);
    M5Cardputer.Speaker.tone(freq, ms);
}
static void haJingleUp() { haBeep(1319, 160); }
static void haJingleJoin() { haBeep(1568, 90); }
static void haJingleLeave() { haBeep(523, 130); }

static DNSServer dnsServer;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static IPAddress apIP(192, 168, 4, 1);
static HaSocketTable haSockets = {};

static HaWsFlowState haWsFlow[HA_WS_OBJECT_LIMIT];
// A game-pad emoji in the default SSID makes the network jump out in a phone's Wi-Fi
// list. SSIDs are UTF-8 up to 32 bytes; the emoji is 4, so this fits with room to
// spare. (The host's own screen font has no emoji glyph, so it shows a placeholder
// there -- cosmetic; the phones that matter render it fine.)
static char apName[33] = "\xF0\x9F\x8E\xAE Hotspot Arcade";
static char joinCode[7] = ""; // generated once per boot; never persisted or sent to phones
static bool portalRunning = false;

static HaApState haApState = HaApBooting;
static HaApRequiredRoster haApRequiredRoster = {};
static uint32_t haApReconnectStartedMs = 0;
static bool haApReconnectExpiryApplied = false;
#define HA_AP_RECONNECT_WINDOW_MS 600000UL

static Engine engine;

// Engine state is touched from the loop task (tick, host actions) and from the
// AsyncTCP task (WebSocket events), so it is guarded exactly as in the two-device
// firmware. The host mirror is written only from inside sinks, which are only ever
// reached from an engine call, so this one lock covers both.
static SemaphoreHandle_t engineMutex = nullptr;
#define ENGINE_LOCK() xSemaphoreTakeRecursive(engineMutex, portMAX_DELAY)
#define ENGINE_UNLOCK() xSemaphoreGiveRecursive(engineMutex)

static bool haPersistSsidConfig(const char* ssid);

// ---------------- sinks used by the engine ----------------

static HaWsFlowState* haWsFlowFind(uint32_t wsId) {
    if(!wsId) return nullptr;
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++)
        if(haWsFlow[i].wsId == wsId) return &haWsFlow[i];
    return nullptr;
}

static bool haWsFlowBegin(uint32_t wsId) {
    if(haWsFlowFind(wsId)) return true;
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++) {
        if(haWsFlow[i].wsId) continue;
        haWsFlow[i] = HaWsFlowState{};
        haWsFlow[i].wsId = wsId;
        return true;
    }
    return false;
}

static void haWsFlowEnd(uint32_t wsId) {
    HaWsFlowState* flow = haWsFlowFind(wsId);
    if(flow) *flow = HaWsFlowState{};
}

static void haWsCacheState(
    HaWsFlowState& flow,
    HaWsOutputClass outputClass,
    const String& msg) {
    HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
    if(choice == HaWsDirtyLobby) {
        flow.lobby = msg;
    } else if(choice == HaWsDirtyState) {
        flow.state = msg;
    } else return;
    haWsDirtyMark(flow.dirty, outputClass);
}

static void haWsRetireSupersededCache(
    HaWsFlowState& flow,
    HaWsOutputClass outputClass) {
    HaWsDirtyChoice choice = haWsDirtyChoiceForOutput(outputClass);
    if(!haWsDirtyHas(flow.dirty, choice)) return;
    haWsDirtyRetireSuperseded(flow.dirty, outputClass);
    if(choice == HaWsDirtyLobby) flow.lobby = "";
    else if(choice == HaWsDirtyState) flow.state = "";
}

static void haWsCloseForOutboundFailure(
    AsyncWebSocketClient& client,
    const char* reason) {
    client.close(1013, reason);
}

static HaWsOutputClass haWsOutputClass(const String& msg) {
    char type[16];
    if(!ha_json_str(msg.c_str(), "t", type, sizeof(type)))
        return HaWsOutputControl;
    bool pongHasPhase = false;
    if(strcmp(type, "pong") == 0) {
        char phase[16];
        pongHasPhase = ha_json_str(msg.c_str(), "phase", phase, sizeof(phase));
    }
    return haWsClassifyOutput(type, pongHasPhase);
}

void haWsSendWs(uint32_t wsId, const String& msg) {
    if(!wsId) return;
    AsyncWebSocketClient* client = ws.client(wsId);
    HaWsFlowState* flow = haWsFlowFind(wsId);
    if(!client || !flow) return;
    size_t queueDepth = client->queueLen();
    haWsQueueObserve(flow->queue, queueDepth);
    HaWsOutputClass outputClass = haWsOutputClass(msg);
    HaWsOutputAction action = haWsChooseOutputAction(
        outputClass, queueDepth, flow->queue.controlCount);

    if(action == HaWsOutputDropStream) {
        if(haWsOutputIsReplaceable(outputClass))
            haWsCacheState(*flow, outputClass, msg);
        return;
    }
    if(action == HaWsOutputCoalesce) {
        haWsCacheState(*flow, outputClass, msg);
        return;
    }
    if(action == HaWsOutputCloseControl) {
        // ACK processing runs on AsyncTCP, so refresh once immediately before
        // closing rather than acting on a stale control count.
        queueDepth = client->queueLen();
        haWsQueueObserve(flow->queue, queueDepth);
        action = haWsChooseOutputAction(
            outputClass, queueDepth, flow->queue.controlCount);
    }
    if(action == HaWsOutputCloseControl) {
        haWsCloseForOutboundFailure(*client, "outbound control overload");
        return;
    }

    if(client->text(msg)) {
        // A successful enqueue below the library's fixed queue capacity must
        // always fit our equally sized tracker. Recover by reconnecting if the
        // two ever disagree instead of losing control-count integrity.
        if(!haWsQueueRecord(flow->queue, outputClass))
            haWsCloseForOutboundFailure(*client, "outbound tracking failure");
        else
            haWsRetireSupersededCache(*flow, outputClass);
        return;
    }

    HaWsSendFailureAction failure = haWsChooseSendFailureAction(outputClass);
    if(failure == HaWsFailureCacheSnapshot)
        haWsCacheState(*flow, outputClass, msg);
    else if(failure != HaWsFailureDropMessage)
        // Welcome, config, pause, reject, toast, and heartbeat responses are
        // nonreplaceable. A failed enqueue closes only this client so protocol
        // v2 resume can recover it with a fresh authoritative snapshot.
        haWsCloseForOutboundFailure(*client, "outbound control send failed");
}
void haWsCloseWs(uint32_t wsId) {
    if(wsId) ws.close(wsId, 1008, "identity takeover");
}
void haWsBroadcast(const String& msg) {
    for(AsyncWebSocketClient& client : ws.getClients())
        if(client.status() == WS_CONNECTED) haWsSendWs(client.id(), msg);
}

// Called under ENGINE_LOCK from loop(). A dropped/coalesced authoritative state
// is retried only after that client's AsyncWebSocket queue drains below four.
static void haWsFlushDirty() {
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++) {
        HaWsFlowState& flow = haWsFlow[i];
        if(!flow.wsId || !haWsDirtyAny(flow.dirty)) continue;
        AsyncWebSocketClient* client = ws.client(flow.wsId);
        if(!client) continue;
        for(uint8_t attempt = 0; attempt < 2; attempt++) {
            size_t queueDepth = client->queueLen();
            haWsQueueObserve(flow.queue, queueDepth);
            HaWsDirtyChoice choice = haWsChooseDirtyRetry(flow.dirty, queueDepth);
            if(choice == HaWsDirtyNone) break;
            String& pending = choice == HaWsDirtyLobby ? flow.lobby : flow.state;
            HaWsOutputClass outputClass = haWsDirtyOutputClass(choice);
            if(!client->text(pending)) {
                // A retry is still governed by its message class. Replaceable
                // snapshots remain cached; a future nonreplaceable dirty class
                // would close for resume recovery rather than disappear.
                HaWsSendFailureAction failure =
                    haWsChooseSendFailureAction(outputClass);
                if(failure == HaWsFailureCloseClient)
                    haWsCloseForOutboundFailure(
                        *client, "outbound dirty control send failed");
                else if(failure == HaWsFailureDropMessage) {
                    haWsRetireSupersededCache(flow, outputClass);
                }
                break;
            }
            if(!haWsQueueRecord(flow.queue, outputClass)) {
                haWsCloseForOutboundFailure(*client, "outbound tracking failure");
                break;
            }
            haWsRetireSupersededCache(flow, outputClass);
        }
    }
}

#define HA_AUTH_WINDOW_MS 60000UL
#define HA_AUTH_LOCKOUT_MS 60000UL
#define HA_AUTH_CLIENT_FAILURES 5
#define HA_AUTH_GLOBAL_FAILURES 30
#define HA_AUTH_CLIENT_BUCKETS 16

static HaAuthCounter haAuthGlobal = {};
static HaAuthClientBucket haAuthClients[HA_AUTH_CLIENT_BUCKETS] = {};

// Explicit declarations keep Arduino's sketch preprocessor from synthesizing
// prototypes above these local type definitions.
static void haAuthNormalize(HaAuthCounter& counter, uint32_t now);
static bool haAuthRecordFailure(HaAuthCounter& counter, uint8_t limit, uint32_t now);
static HaAuthClientBucket& haAuthClient(uint32_t wsId, uint32_t now);

static uint32_t haAuthRemaining(uint32_t now, uint32_t deadline) {
    int32_t remaining = (int32_t)(deadline - now);
    return remaining > 0 ? (uint32_t)remaining : 0;
}

static void haAuthNormalize(HaAuthCounter& counter, uint32_t now) {
    if(counter.lockUntilMs) {
        if(haAuthRemaining(now, counter.lockUntilMs)) return;
        counter = HaAuthCounter{};
    }
    if(counter.windowStarted &&
       (uint32_t)(now - counter.windowStartMs) >= HA_AUTH_WINDOW_MS)
        counter = HaAuthCounter{};
}

static bool haAuthRecordFailure(HaAuthCounter& counter, uint8_t limit, uint32_t now) {
    haAuthNormalize(counter, now);
    if(counter.lockUntilMs) return true;
    if(!counter.windowStarted) {
        counter.windowStarted = true;
        counter.windowStartMs = now;
    }
    if(counter.failures < UINT8_MAX) counter.failures++;
    if(counter.failures < limit) return false;
    counter.failures = 0;
    counter.windowStarted = false;
    counter.lockUntilMs = now + HA_AUTH_LOCKOUT_MS;
    return true;
}

static HaAuthClientBucket& haAuthClient(uint32_t clientKey, uint32_t now) {
    HaAuthClientBucket* available = nullptr;
    HaAuthClientBucket* oldest = nullptr;
    uint32_t oldestAge = 0;
    for(uint8_t i = 0; i < HA_AUTH_CLIENT_BUCKETS; i++) {
        HaAuthClientBucket& bucket = haAuthClients[i];
        if(bucket.used && bucket.clientKey == clientKey) {
            bucket.lastSeenMs = now;
            return bucket;
        }
        if(!bucket.used && !available) available = &bucket;
        if(bucket.used) {
            haAuthNormalize(bucket.counter, now);
            uint32_t age = now - bucket.lastSeenMs;
            if(!oldest || age > oldestAge) {
                oldest = &bucket;
                oldestAge = age;
            }
        }
    }
    HaAuthClientBucket* bucket = available ? available : oldest;
    configASSERT(bucket != nullptr);
    *bucket = HaAuthClientBucket{};
    bucket->used = true;
    bucket->clientKey = clientKey;
    bucket->lastSeenMs = now;
    return *bucket;
}

uint8_t haAuthorizeIdentity(
    uint32_t wsId,
    const char* identity,
    const char* code,
    uint32_t* retryMs) {
    if(retryMs) *retryMs = 0;
    // The callback runs inside ENGINE_LOCK. A durable ledger identity bypasses
    // both the party code and brute-force buckets so reboot/AP resume is reliable.
    if(haHostIdentityKnown(identity)) return HA_JOIN_AUTH_KNOWN;

    uint32_t now = millis();
    AsyncWebSocketClient* socket = ws.client(wsId);
    // The AP assigns one IPv4 address per station, so this survives a browser's
    // reconnect/new WebSocket and cannot be bypassed by cycling socket ids.
    uint32_t clientKey = socket ? (uint32_t)socket->remoteIP() : wsId;
    HaAuthClientBucket& client = haAuthClient(clientKey, now);
    haAuthNormalize(client.counter, now);
    haAuthNormalize(haAuthGlobal, now);
    uint32_t clientRetry = haAuthRemaining(now, client.counter.lockUntilMs);
    uint32_t globalRetry = haAuthRemaining(now, haAuthGlobal.lockUntilMs);
    if(clientRetry || globalRetry) {
        if(retryMs) *retryMs = clientRetry > globalRetry ? clientRetry : globalRetry;
        return HA_JOIN_AUTH_THROTTLED;
    }

    if(!code || !code[0]) return HA_JOIN_AUTH_REQUIRED;
    if(strcmp(code, joinCode) == 0) {
        client.counter = HaAuthCounter{};
        if(!haHostCanTrackIdentity(identity))
            return HA_JOIN_AUTH_FULL;
        return HA_JOIN_AUTH_OK;
    }

    bool clientLocked = haAuthRecordFailure(
        client.counter,
        HA_AUTH_CLIENT_FAILURES,
        now);
    bool globalLocked = haAuthRecordFailure(
        haAuthGlobal,
        HA_AUTH_GLOBAL_FAILURES,
        now);
    if(clientLocked || globalLocked) {
        clientRetry = haAuthRemaining(now, client.counter.lockUntilMs);
        globalRetry = haAuthRemaining(now, haAuthGlobal.lockUntilMs);
        if(retryMs) *retryMs = clientRetry > globalRetry ? clientRetry : globalRetry;
        return HA_JOIN_AUTH_THROTTLED;
    }
    return HA_JOIN_AUTH_BAD_CODE;
}

void haUartJoinStable(
    uint8_t pid,
    const char* identity,
    const char* nick,
    const char* avatar) {
    bool joined = haHostJoinStable(pid, identity, nick, avatar);
    if(joined) haJingleJoin(); // jingle on a new join, not a rename
}
void haUartJoin(uint8_t pid, const char* nick) {
    haUartJoinStable(pid, nullptr, nick, nullptr); // protocol-v1 compatibility
}
void haUartLeave(uint8_t pid) {
    haHostLeave(pid);
    haJingleLeave();
}
void haUartScore(uint8_t pid, int delta, const char* reason) {
    (void)reason;
    haHostScore(pid, delta);
}
static const char* haNick(int pid) {
    if(pid >= 1 && pid <= HA_MAX_PLAYERS && haHost.p[pid].used) return haHost.p[pid].nick;
    return "?";
}

static const char* haGameName(uint8_t game) {
    for(size_t i = 0; i < HA_GENERATED_GAME_COUNT; i++)
        if(HA_GENERATED_GAMES[i].id == game) return HA_GENERATED_GAMES[i].label;
    return "Arcade";
}

void haUartHostEvent(
    uint8_t kind,
    uint8_t game,
    uint8_t actor,
    uint8_t target,
    int16_t value,
    const char* text) {
    const char* gameName = haGameName(game);
    const char* actorName = haNick(actor);
    const char* targetName = haNick(target);
    char line[HA_EV_LEN];
    HaHostEventDisposition disposition = haFormatHostEvent(
        kind,
        gameName,
        actorName,
        targetName,
        value,
        text,
        line,
        sizeof(line));
    if(disposition == HaHostEventLog) {
        haHostLog(line);
        return;
    }
    if(disposition == HaHostEventStatus) haHostSetEvent(line);
}

// ---------------- HTTP (captive) ----------------

// Serve the baked web bundle for every host/path so the captive portal always
// resolves. GET "/" (and every OS captive-probe URL) gets the app; other bundled
// paths are served by exact match. Identical policy to the streamed build, just
// reading from flash instead of a heap buffer.
static const HaBakedFile* haFindFile(const char* path) {
    for(size_t i = 0; i < HA_BAKED_FILE_COUNT; i++)
        if(strcmp(HA_BAKED_FILES[i].path, path) == 0) return &HA_BAKED_FILES[i];
    return nullptr;
}

class ArcadeHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* request) const override {
        (void)request;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* request) override {
        const HaBakedFile* a = haFindFile(request->url().c_str());
        if(!a && HA_BAKED_FILE_COUNT) a = &HA_BAKED_FILES[0]; // captive probes -> the app
        if(!a) {
            request->send(200, "text/html", "<h1>Hotspot Arcade</h1><p>No bundle baked in.</p>");
            return;
        }
        AsyncWebServerResponse* res = request->beginResponse(200, a->mime, a->data, a->len);
        if(a->gzip) res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        request->send(res);
    }
};

// ---------------- WebSocket ----------------

static void onWsEvent(
    AsyncWebSocket* srv,
    AsyncWebSocketClient* client,
    AwsEventType type,
    void* arg,
    uint8_t* data,
    size_t len) {
    (void)srv;
    if(type == WS_EVT_CONNECT) {
        HaSocketAdmission admission = HaSocketObjectsFull;
        ENGINE_LOCK();
        admission = haSocketConnect(haSockets, client->id(), millis());
        bool flowReady = admission == HaSocketAccepted && haWsFlowBegin(client->id());
        if(!flowReady && admission == HaSocketAccepted) {
            haSocketDisconnect(haSockets, client->id());
            admission = HaSocketObjectsFull;
        }
        ENGINE_UNLOCK();
        if(admission != HaSocketAccepted) {
            client->close(1013, admission == HaSocketPendingFull ? "pending full" : "socket full");
            return;
        }
        // Firmware applies its own message-class thresholds. A replaceable state
        // queue must never make the library evict an authenticated identity.
        client->setCloseClientOnQueueFull(false);
    } else if(type == WS_EVT_DISCONNECT) {
        uint32_t rawNow = millis();
        ENGINE_LOCK();
        uint8_t detachedPid = engine.pidByWs(client->id());
        engine.onWsDisconnect(client->id(), rawNow);
        if(detachedPid) haHostSetOnline(detachedPid, false);
        haSocketDisconnect(haSockets, client->id());
        haWsFlowEnd(client->id());
        ENGINE_UNLOCK();
    } else if(type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT &&
           len < WS_MSG_MAX) {
            char buf[WS_MSG_MAX];
            memcpy(buf, data, len);
            buf[len] = '\0';
            uint32_t rawNow = millis();
            ENGINE_LOCK();
            // Enforce the planned reconnect deadline on the AsyncTCP task before
            // Engine can accept a late hello and turn a detached seat online. The
            // loop task completes the AP-state transition on its next tick.
            if(haApReconnectExpiresBeforeInput(
                   haApState,
                   rawNow,
                   haApReconnectStartedMs,
                   HA_AP_RECONNECT_WINDOW_MS) &&
               !haApReconnectExpiryApplied) {
                engine.transportResume(rawNow, true);
                haApReconnectExpiryApplied = true;
            }
            HaSocketSlot* slot = haSocketFind(haSockets, client->id());
            char inputType[16];
            bool haveType = ha_json_str(buf, "t", inputType, sizeof(inputType));
            HaInboundClass inputClass = haInboundClass(haveType ? inputType : nullptr);
            bool allowed = slot && haInboundAllow(slot->limiter, inputClass, rawNow);
            if(allowed) {
                engine.onInput(client->id(), buf, rawNow);
                if(engine.pidByWs(client->id())) haSocketAuthenticate(haSockets, client->id());
            }
            ENGINE_UNLOCK();
        } else if(info->final && info->opcode == WS_TEXT) {
            client->close(1009, "message too large");
        }
    }
}

static void haSocketTick(uint32_t now) {
    uint32_t expired[HA_WS_PENDING_LIMIT];
    uint8_t expiredCount = 0;
    ENGINE_LOCK();
    for(uint8_t i = 0; i < HA_WS_OBJECT_LIMIT; i++) {
        const HaSocketSlot& slot = haSockets.slots[i];
        if(haSocketHelloExpired(slot, now) && expiredCount < HA_WS_PENDING_LIMIT)
            expired[expiredCount++] = slot.wsId;
    }
    for(uint8_t i = 0; i < expiredCount; i++) {
        haSocketDisconnect(haSockets, expired[i]);
        haWsFlowEnd(expired[i]);
    }
    haWsFlushDirty();
    ENGINE_UNLOCK();
    for(uint8_t i = 0; i < expiredCount; i++)
        ws.close(expired[i], 1008, "hello timeout");
}

// ---------------- AP lifecycle ----------------

static void haEnsureJoinCode() {
    if(joinCode[0]) return;
    // Rejection sampling avoids modulo bias: [0, limit) contains an exact number
    // of one-million-value ranges. esp_random() is backed by the ESP32 hardware
    // RNG once the Wi-Fi driver is enabled immediately before this call.
    static const uint32_t range = 1000000UL;
    static const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    uint32_t randomValue = 0;
    do randomValue = esp_random(); while(randomValue >= limit);
    snprintf(joinCode, sizeof(joinCode), "%06lu", (unsigned long)(randomValue % range));
}

const char* haHostJoinCode() {
    return joinCode;
}

// Handlers are registered once, not per start: the SSID editor stops and restarts
// the portal, and addHandler() has no matching remove, so re-registering on every
// start would stack a new ArcadeHandler (and leak it) each time the host renames
// the AP. The Flipper build never noticed because it re-flashed state instead.
static bool installHandlers() {
    ArcadeHandler* handler = new(std::nothrow) ArcadeHandler();
    if(!handler) return false;
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.addHandler(handler).setFilter(ON_AP_FILTER);
    return true;
}

static bool startPortalTransport() {
    if(!WiFi.mode(WIFI_AP)) {
        portalRunning = false;
        Serial.println("[ha] Wi-Fi AP mode failed");
        return false;
    }
    haEnsureJoinCode();
    if(!WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0)) ||
       !WiFi.softAP(apName, nullptr, 1, 0, AP_MAX_CONN)) { // open AP
        portalRunning = false;
        Serial.println("[ha] AP start failed");
        return false;
    }
    delay(100);

    if(!dnsServer.start(53, "*", apIP)) {
        WiFi.softAPdisconnect(true);
        portalRunning = false;
        Serial.println("[ha] DNS start failed");
        return false;
    }
    server.begin();
    portalRunning = true;

    ENGINE_LOCK();
    haHost.portalRunning = true;
    haHostLog("AP up");
    ENGINE_UNLOCK();
    haJingleUp();
    Serial.printf("[ha] AP \"%s\" up at %s\n", apName, WiFi.softAPIP().toString().c_str());
    return true;
}

static void stopPortalTransport() {
    if(!portalRunning) return;
    ws.closeAll(1001, "access point pause");
    server.end();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    portalRunning = false;
    Serial.println("[ha] AP transport stopped");
}

static void haPortalResume(bool expireDetachedNow = false) {
    uint32_t now = millis();
    ENGINE_LOCK();
    // At the exact planned-window deadline, detached seats are finalized while
    // game time is still frozen. All-required-ready and explicit host resume use
    // the ordinary path, which starts the normal two-minute grace for anyone the
    // host chose not to wait for.
    engine.transportResume(now, expireDetachedNow);
    haHostLog("session resumed");
    haApState = HaApRunning;
    haApRequiredClear(haApRequiredRoster);
    haApReconnectStartedMs = 0;
    haApReconnectExpiryApplied = false;
    ENGINE_UNLOCK();
}

static void haPortalSetState(HaApState state) {
    ENGINE_LOCK();
    haApState = state;
    ENGINE_UNLOCK();
}

// Freeze engine time and retain the in-memory roster before touching AP/DNS/server
// state. Durable checkpoints are added by the persistence tranche layered on this
// network/session implementation.
static bool haPortalPauseAndStop(const char* reason, const char* reconnectSsid) {
    if(!portalRunning) return true;
    ENGINE_LOCK();
    haApRequiredCapture(haApRequiredRoster, haHost);
    engine.announceServerPause(
        reason,
        reconnectSsid ? reconnectSsid : apName,
        HA_AP_RECONNECT_WINDOW_MS);
    engine.transportPause(millis());
    haHostLog("session paused");
    ENGINE_UNLOCK();

    ENGINE_LOCK();
    haHostSuspendConnections();
    haHost.portalRunning = false;
    haHostLog("AP stopped");
    ENGINE_UNLOCK();
    stopPortalTransport();
    return true;
}

static bool haPortalStartReconnect() {
    if(!startPortalTransport()) return false;
    bool noRequiredPlayers = false;
    ENGINE_LOCK();
    haApState = HaApReconnectWait;
    haApReconnectStartedMs = millis();
    haApReconnectExpiryApplied = false;
    noRequiredPlayers = !haApRequiredRoster.count && !haApRequiredRoster.unidentified;
    ENGINE_UNLOCK();
    if(noRequiredPlayers) haPortalResume();
    return true;
}

static void haPortalTick(uint32_t now) {
    HaApReconnectDecision decision = HaApReconnectDecisionWait;
    ENGINE_LOCK();
    if(haApState == HaApReconnectWait && portalRunning)
        decision = haApReconnectEvaluate(
            haApRequiredRoster,
            haHost,
            now,
            haApReconnectStartedMs,
            HA_AP_RECONNECT_WINDOW_MS);
    ENGINE_UNLOCK();
    if(decision == HaApReconnectDecisionAllRequiredOnline)
        haPortalResume();
    else if(decision == HaApReconnectDecisionWindowExpired)
        haPortalResume(true);
}

// ---------------- host actions (called from the UI, on the loop task) ----------

void haHostSelectGame(uint8_t game) {
    ENGINE_LOCK();
    engine.selectGame(game);
    haHost.activeGame = game;
    if(game != HA_GAME_NONE) haHostGamePlayed(game);
    haHostLog("game changed");
    ENGINE_UNLOCK();
}

bool haHostResetScores(bool discardOnArchiveFailure) {
    (void)discardOnArchiveFailure;
    ENGINE_LOCK();
    engine.resetScores();
    haHostResetSessionScores();
    haHostLog("new session");
    ENGINE_UNLOCK();
    // New Session is a new party-admission boundary as well as a new score
    // ledger. A cryptographically fresh code is displayed immediately.
    joinCode[0] = '\0';
    haEnsureJoinCode();
    return true;
}

void haHostRoundEnd() {
    ENGINE_LOCK();
    engine.roundEnd();
    haHostLog("round ended");
    ENGINE_UNLOCK();
}

static bool haStartSsidTransport(const char*) {
    return haPortalStartReconnect();
}

void haHostApplySsid(const char* ssid) {
    if(!ssid || !ssid[0] || strnlen(ssid, sizeof(apName)) >= sizeof(apName) ||
       strcmp(ssid, apName) == 0)
        return;
    bool renameAllowed = false;
    ENGINE_LOCK();
    renameAllowed = haApSsidRenameAllowed(haApState);
    if(!renameAllowed) haHostSetEvent("resume AP before SSID rename");
    ENGINE_UNLOCK();
    if(!renameAllowed) {
        Serial.println("[ha] refusing SSID rename during reconnect wait");
        return;
    }
    bool wasUp = portalRunning;
    if(wasUp && !haPortalPauseAndStop("ssid_change", ssid)) return;

    HaSsidTransactionResult result = haSsidApplyTransaction(
        apName,
        sizeof(apName),
        ssid,
        wasUp,
        haPersistSsidConfig,
        haStartSsidTransport);
    switch(result) {
    case HaSsidCandidateRejectedPriorRunning:
        Serial.println("[ha] new SSID was not persisted; retaining prior SSID");
        break;
    case HaSsidCandidateRejectedPriorOffline:
        haPortalSetState(HaApManualOff);
        Serial.println("[ha] new SSID rejected and prior SSID restart failed; AP remains paused");
        break;
    case HaSsidFallbackRunning:
        Serial.println("[ha] new SSID failed; prior SSID restored");
        break;
    case HaSsidFallbackOffline:
        haPortalSetState(HaApManualOff);
        Serial.println("[ha] prior SSID fallback also failed; AP remains paused");
        break;
    case HaSsidRollbackRejectedCandidateOffline:
        // The candidate remains both the runtime value and the last successful
        // config generation. Never run the old AP while reboot selects the new.
        haPortalSetState(HaApManualOff);
        Serial.println("[ha] SSID rollback was not persisted; AP remains paused");
        break;
    default:
        break;
    }
}

void haHostTogglePortal() {
    HaApState state = HaApBooting;
    ENGINE_LOCK();
    state = haApState;
    ENGINE_UNLOCK();
    if(state == HaApReconnectWait && portalRunning) {
        haPortalResume(); // host explicitly resumes early with missing players
        return;
    }
    if(portalRunning) {
        if(haPortalPauseAndStop("ap_off", apName)) haPortalSetState(HaApManualOff);
        return;
    }
    if(!haPortalStartReconnect()) {
        haPortalSetState(HaApManualOff);
        Serial.println("[ha] AP start request failed; session remains paused");
    }
}

const char* haHostSsid() {
    return apName;
}

String haHostIp() {
    return portalRunning ? WiFi.softAPIP().toString() : String("--");
}

// Copy the mirror under the lock so the UI can spend milliseconds drawing without
// holding up the WebSocket task.
void haHostSnapshot(HaHost& dst) {
    ENGINE_LOCK();
    memcpy(&dst, &haHost, sizeof(HaHost));
    ENGINE_UNLOCK();
}

// ---------------- Arduino entry ----------------

// Cardputer v1 microSD is on its own SPI bus: SCK=40, MISO=39, MOSI=14, CS=12.
// This is separate from the display bus, so mounting it here doesn't disturb the UI.
static SPIClass haSdSpi(FSPI);
bool haSdOk = false; // non-static: ha_history.h reads it via `extern`
static void haSdBegin() {
    haSdSpi.begin(40, 39, 14, 12);
    haSdOk = SD.begin(12, haSdSpi, 20000000);
    if(haSdOk)
        Serial.printf(
            "[ha] SD ok: %lluMB, type %d\n",
            SD.cardSize() / (1024ULL * 1024ULL),
            (int)SD.cardType());
    else
        Serial.println("[ha] SD: no card or mount failed");
}

// Settings (SSID, audio, language) retain the legacy line-oriented SD format
// until the durable A/B + NVS storage tranche replaces it.
static const char* HA_CFG_PATH = "/hotspot-arcade/config.txt";
static const char* HA_CFG_TMP_PATH = "/hotspot-arcade/config.tmp";
static const char* HA_CFG_BACKUP_PATH = "/hotspot-arcade/config.bak";

static bool haCfgSaveValues(const char* ssid, uint8_t audio, uint8_t lang) {
    // A failed persistence callback must leave the previous durable SSID intact:
    // haSsidApplyTransaction() relies on that before it restarts the prior AP.
    if(!haSdOk || !ssid || !ssid[0]) return false;
    SD.mkdir("/hotspot-arcade");

    char record[96];
    int recordLength = snprintf(
        record,
        sizeof(record),
        "ssid=%s\naudio=%u\nlang=%u\n",
        ssid,
        (unsigned)audio,
        (unsigned)lang);
    if(recordLength <= 0 || (size_t)recordLength >= sizeof(record)) return false;

    SD.remove(HA_CFG_TMP_PATH);
    File f = SD.open(HA_CFG_TMP_PATH, FILE_WRITE);
    if(!f) return false;
    size_t bytesWritten = f.write((const uint8_t*)record, (size_t)recordLength);
    f.flush();
    f.close();
    if(bytesWritten != (size_t)recordLength) {
        SD.remove(HA_CFG_TMP_PATH);
        return false;
    }

    File verify = SD.open(HA_CFG_TMP_PATH, FILE_READ);
    char readback[sizeof(record)];
    size_t bytesRead = verify ? verify.readBytes(readback, (size_t)recordLength) : 0;
    bool verified = verify && verify.size() == (size_t)recordLength &&
                    bytesRead == (size_t)recordLength &&
                    memcmp(readback, record, (size_t)recordLength) == 0;
    if(verify) verify.close();
    if(!verified) {
        SD.remove(HA_CFG_TMP_PATH);
        return false;
    }

    SD.remove(HA_CFG_BACKUP_PATH);
    bool hadCurrent = SD.exists(HA_CFG_PATH);
    if(hadCurrent && !SD.rename(HA_CFG_PATH, HA_CFG_BACKUP_PATH)) {
        SD.remove(HA_CFG_TMP_PATH);
        return false;
    }
    if(!SD.rename(HA_CFG_TMP_PATH, HA_CFG_PATH)) {
        if(hadCurrent) (void)SD.rename(HA_CFG_BACKUP_PATH, HA_CFG_PATH);
        SD.remove(HA_CFG_TMP_PATH);
        return false;
    }
    if(hadCurrent) SD.remove(HA_CFG_BACKUP_PATH);
    return true;
}

bool haCfgSave() {
    return haCfgSaveValues(apName, haAudioLevel, haLang);
}

static bool haPersistSsidConfig(const char* ssid) {
    bool saved = haCfgSaveValues(ssid, haAudioLevel, haLang);
    if(!saved) Serial.println("[ha] legacy config save failed");
    return saved;
}

static void haCfgLoad() {
    if(!haSdOk) return;
    const char* loadPath = SD.exists(HA_CFG_PATH) ? HA_CFG_PATH : HA_CFG_BACKUP_PATH;
    File f = SD.open(loadPath, FILE_READ);
    if(!f) return;
    while(f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if(eq <= 0) continue;
        String key = line.substring(0, eq);
        String value = line.substring(eq + 1);
        if(key == "ssid") {
            if(value.length()) strlcpy(apName, value.c_str(), sizeof(apName));
        } else if(key == "audio") {
            int parsed = value.toInt();
            if(parsed >= 0 && parsed <= 2) haAudioLevel = (uint8_t)parsed;
        } else if(key == "lang") {
            int parsed = value.toInt();
            if(parsed >= 0 && parsed < HA_LANG_COUNT) haLang = (uint8_t)parsed;
        }
    }
    f.close();
}

static void haStartupFatal(const char* reason) {
    Serial.printf("[ha] fatal startup: %s\n", reason ? reason : "unknown");
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5Cardputer.Display.drawString("HOTSPOT ARCADE", 8, 18);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString("Startup failed:", 8, 42);
    M5Cardputer.Display.drawString(reason ? reason : "unknown", 8, 56);
    // Never confirm an OTA image that cannot initialize this tranche's critical
    // runtime. M5Launcher remains able to roll it back on the next reset.
    while(true) {
        M5Cardputer.update();
        delay(1000);
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    engineMutex = xSemaphoreCreateRecursiveMutex();
    if(!engineMutex) haStartupFatal("engine mutex");
    if(!haHostBegin()) haStartupFatal("host memory");
    haUiBegin();

    haSdBegin();
    {
        Preferences prefs;
        if(prefs.begin("ha_cfg", true)) {
            haLang = prefs.getUChar("lang", 0);
            prefs.end();
        }
        if(haLang >= HA_LANG_COUNT) haLang = 0;
    }
    haCfgLoad();

    haHostReset();

    bool contentReady = false;
    ENGINE_LOCK();
    engine.reset(millis());
    contentReady = haContentLoadAll(engine, HA_LANG_CODE[haLang]);
    if(contentReady) {
        // Setup work and first-start storage I/O are not game time. AP startup
        // resumes this clock only after the AP/DNS/HTTP transport is ready.
        engine.transportPause(millis());
        haHostLog("packs loaded");
    }
    ENGINE_UNLOCK();
    if(!contentReady) haStartupFatal("content packs");
    haLoadedLang = haLang;

    if(!installHandlers()) haStartupFatal("HTTP handler memory");
    Serial.printf(
        "[ha] %u web file(s), %u pack(s), free heap %u\n",
        (unsigned)HA_BAKED_FILE_COUNT,
        (unsigned)HA_BAKED_PACK_COUNT,
        (unsigned)ESP.getFreeHeap());

    if(!startPortalTransport()) haStartupFatal("access point");
    haPortalResume();
    haUiDraw();

    // Confirm only after critical allocations, config/session recovery, content,
    // handlers, AP start, and first draw all complete. Plain esptool flashes have
    // no pending image, so this remains a harmless no-op there.
    esp_err_t otaStatus = esp_ota_mark_app_valid_cancel_rollback();
    if(otaStatus != ESP_OK)
        Serial.printf("[ha] OTA healthy-mark warning: %s\n", esp_err_to_name(otaStatus));
}

void loop() {
    uint32_t now = millis();

    M5Cardputer.update();
    haUiPumpKeys();

    if(haLangDirty) { // Settings changed the language -> re-stream that language's packs
        haLangDirty = false;
        uint8_t requestedLang = haLang;
        bool contentReady = false;
        ENGINE_LOCK();
        contentReady = haContentLoadAll(engine, HA_LANG_CODE[requestedLang]);
        if(contentReady) {
            haLoadedLang = requestedLang;
            char event[HA_EV_LEN];
            snprintf(
                event,
                sizeof(event),
                "language: %s",
                HA_LANG_NAME[requestedLang % HA_LANG_COUNT]);
            haHostLog(event);
        } else {
            // The staged loader retains the previous content/language on failure.
            haHostLog("language load failed");
        }
        ENGINE_UNLOCK();
        if(!contentReady) {
            haLang = haLoadedLang;
            haCfgSave(); // keep persisted settings aligned with the content still live
        }
    }

    if(portalRunning) {
        dnsServer.processNextRequest();
        ws.cleanupClients(HA_WS_OBJECT_LIMIT);
        haSocketTick(now);
        ENGINE_LOCK();
        engine.tick(now);
        ENGINE_UNLOCK();
        haPortalTick(now);
    }

    haUiTick();
}
