// Hotspot Arcade firmware for the M5Stack Cardputer and Cardputer Advance
// (ESP32-S3).
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
#include <esp_heap_caps.h>
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
#include "ha_active_nvs.h"
#include "ha_config.h"
#include "ha_ssid_transaction.h"
#include "ha_content.h"
#include "ha_async_queue.h"
#include "ha_device.h"
#include "ha_diagnostics.h"
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

// Generated content/host language index (see ha_ui.h). Persisted in NVS. The
// Settings screen changes it and sets haLangDirty; loop() then re-streams packs and
// the host UI resolves German by language code, with English fallback otherwise.
uint8_t haLang = 0;
alignas(4) std::atomic<uint32_t> haUiLocaleCache{(uint32_t)HaUiEnglish};
bool haLangDirty = false;
static uint8_t haLoadedLang = 0;

enum HaSoundRequest : uint8_t {
    HaSoundApUp = 1,
    HaSoundJoin = 2,
    HaSoundLeave = 3,
};
static HaAsyncQueue<HaSoundRequest, 8> haSoundQueue;

static void haPlaySound(uint16_t freq, uint16_t ms) {
    if(haAudioLevel == 0) return;
    M5Cardputer.Speaker.setVolume(haAudioLevel == 2 ? 200 : 80);
    M5Cardputer.Speaker.tone(freq, ms);
}
// Async engine sinks enqueue sound intents only. The loop task owns all speaker I/O.
static void haJingleUp() { (void)haSoundQueue.push(HaSoundApUp); }
static void haJingleJoin() { (void)haSoundQueue.push(HaSoundJoin); }
static void haJingleLeave() { (void)haSoundQueue.push(HaSoundLeave); }
static void haSoundPump() {
    HaSoundRequest request;
    if(!haSoundQueue.pop(request)) return; // at most one note per loop tick
    if(request == HaSoundApUp) haPlaySound(1319, 160);
    else if(request == HaSoundJoin) haPlaySound(1568, 90);
    else if(request == HaSoundLeave) haPlaySound(523, 130);
}

static DNSServer dnsServer;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static IPAddress apIP(192, 168, 4, 1);
static HaSocketTable haSockets = {};
static HaDiagnostics haDiagnostics = {};

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
static uint16_t haEngineLockDepth = 0;
static uint32_t haEngineLockStartedUs = 0;

// Every critical section is a real C++ lifetime. The paired macros add only the
// lexical scope around an RAII guard, so returns and future exception-enabled test
// builds cannot strand the recursive mutex. Declarations that must outlive the
// section are intentionally made before ENGINE_LOCK().
class HaEngineGuard {
public:
    HaEngineGuard() {
        BaseType_t result = xSemaphoreTakeRecursive(engineMutex, portMAX_DELAY);
        configASSERT(result == pdTRUE);
        locked = result == pdTRUE;
        if(locked && haEngineLockDepth++ == 0) haEngineLockStartedUs = micros();
    }
    ~HaEngineGuard() {
        if(!locked) return;
        configASSERT(haEngineLockDepth > 0);
        if(--haEngineLockDepth == 0) {
            uint32_t heldUs = micros() - haEngineLockStartedUs;
            if(heldUs > haDiagnostics.maxEngineLockUs)
                haDiagnostics.maxEngineLockUs = heldUs;
        }
        BaseType_t result = xSemaphoreGiveRecursive(engineMutex);
        configASSERT(result == pdTRUE);
        (void)result;
    }
    HaEngineGuard(const HaEngineGuard&) = delete;
    HaEngineGuard& operator=(const HaEngineGuard&) = delete;

private:
    bool locked = false;
};

#define ENGINE_LOCK() do { HaEngineGuard haEngineGuard
#define ENGINE_UNLOCK() (void)haEngineGuard; } while(false)

#define HA_PERSIST_PROBE_MS 250UL
#define HA_PERSIST_SD_COALESCE_MS 1000UL
#define HA_PERSIST_RETRY_MS 5000UL
#define HA_CONFIG_REPAIR_MS 5000UL

struct HaPersistenceRuntime {
    bool ready;
    bool usingSd;
    bool dirty;
    bool nvsMirrorPending;
    uint32_t sessionNumber;
    uint32_t generation;
    uint32_t nvsGeneration;
    uint32_t restoredFrom;
    uint32_t lastAttemptMs;
    uint32_t lastNvsAttemptMs;
    uint32_t lastConfigRepairMs;
    uint32_t lastProbeMs;
};

static HaPersistenceRuntime haPersistence = {};
static HaHost* haPersistenceSnapshot = nullptr;
static HaHost* haPersistenceCandidate = nullptr;
static HaActiveNvsRecord* haPersistenceNvsRecord = nullptr;

// Archive/restore transactions release ENGINE_LOCK while writing the SD card. New
// gameplay input is briefly ignored during that window so a point cannot land after
// the archived snapshot and then be erased by the reset. Disconnects are retained
// and replayed once the transaction completes, avoiding ghost players.
static bool haPersistenceTransaction = false;
struct HaPendingDisconnect {
    uint32_t wsId;
    uint32_t rawAt;
};
static HaPendingDisconnect haPersistencePendingDisconnects[HA_MAX_PLAYERS] = {};
static uint8_t haPersistencePendingDisconnectCount = 0;

static void haPersistenceMarkDirty() {
    haPersistence.dirty = true; // callers run under ENGINE_LOCK
}

static void haPersistenceQueueDisconnect(uint32_t wsId, uint32_t rawAt) {
    for(uint8_t i = 0; i < haPersistencePendingDisconnectCount; i++)
        if(haPersistencePendingDisconnects[i].wsId == wsId) return;
    if(haPersistencePendingDisconnectCount < HA_MAX_PLAYERS)
        haPersistencePendingDisconnects[haPersistencePendingDisconnectCount++] =
            HaPendingDisconnect{wsId, rawAt};
}

static bool haPersistenceCheckpoint(bool force = false);
static void haPersistenceBeginTransaction();
static void haPersistenceEndTransaction();
static bool haPersistenceRestoreHistory(const HaHistSession& session);
static bool haPersistenceStartNewSession(bool discardOnArchiveFailure = false);

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
    if(haDiagnostics.outputCoalesced != UINT32_MAX) haDiagnostics.outputCoalesced++;
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
    if(haDiagnostics.overloadCloses != UINT32_MAX) haDiagnostics.overloadCloses++;
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
    if(queueDepth > haDiagnostics.maxSocketQueue)
        haDiagnostics.maxSocketQueue =
            queueDepth > UINT16_MAX ? UINT16_MAX : (uint16_t)queueDepth;
    haWsQueueObserve(flow->queue, queueDepth);
    HaWsOutputClass outputClass = haWsOutputClass(msg);
    HaWsOutputAction action = haWsChooseOutputAction(
        outputClass, queueDepth, flow->queue.controlCount);

    if(action == HaWsOutputDropStream) {
        if(haWsOutputIsReplaceable(outputClass))
            haWsCacheState(*flow, outputClass, msg);
        if(haDiagnostics.streamDropped != UINT32_MAX) haDiagnostics.streamDropped++;
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
    else if(failure == HaWsFailureDropMessage) {
        if(haDiagnostics.streamDropped != UINT32_MAX) haDiagnostics.streamDropped++;
    } else
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
                    if(haDiagnostics.streamDropped != UINT32_MAX)
                        haDiagnostics.streamDropped++;
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
    haPersistenceMarkDirty();
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
    haPersistenceMarkDirty();
}
static const char* haNick(int pid) {
    if(pid >= 1 && pid <= HA_MAX_PLAYERS && haHost.p[pid].used) return haHost.p[pid].nick;
    return "?";
}

static const char* haGameName(uint8_t game) {
    for(size_t i = 0; i < HA_GENERATED_GAME_COUNT; i++) {
        if(HA_GENERATED_GAMES[i].id == game)
            return haUiGameLabelForLocale(
                HA_GENERATED_GAMES[i].key,
                HA_GENERATED_GAMES[i].label,
                haUiActiveLocale());
    }
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
        sizeof(line),
        haUiActiveLocale(),
        game);
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
        if(haPersistenceTransaction) {
            // Unauthenticated sockets can exceed the AP station count. Retain
            // only disconnects that own an Engine seat, bounded by its roster.
            if(engine.pidByWs(client->id()))
                haPersistenceQueueDisconnect(client->id(), rawNow);
        }
        else
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
            if(!allowed && inputClass < HaInboundClassCount &&
               haDiagnostics.rateRejected[inputClass] != UINT32_MAX)
                haDiagnostics.rateRejected[inputClass]++;
            if(allowed && !haPersistenceTransaction) {
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
    haDiagnostics.wsAuthenticated = haSocketCount(haSockets, true);
    haDiagnostics.wsPending = haSocketCount(haSockets, false);
    haDiagnostics.wsObjects =
        (uint8_t)(haDiagnostics.wsAuthenticated + haDiagnostics.wsPending);
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
    haHostLog(haUiT(HaUiTextEventApUp));
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
    haHostLog(haUiT(HaUiTextEventSessionResumed));
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

// Freeze engine time and durably checkpoint before touching AP/DNS/server state.
// If the forced checkpoint fails, the still-live transport is resumed unchanged.
static bool haPortalPauseAndStop(const char* reason, const char* reconnectSsid) {
    if(!portalRunning) return true;
    ENGINE_LOCK();
    haApRequiredCapture(haApRequiredRoster, haHost);
    engine.announceServerPause(
        reason,
        reconnectSsid ? reconnectSsid : apName,
        HA_AP_RECONNECT_WINDOW_MS);
    engine.transportPause(millis());
    haHostLog(haUiT(HaUiTextEventSessionPaused));
    haPersistenceMarkDirty();
    ENGINE_UNLOCK();

    if(!haPersistenceCheckpoint(true)) {
        Serial.println("[ha] refusing AP change: active checkpoint failed");
        haPortalResume();
        return false;
    }

    ENGINE_LOCK();
    haHostSuspendConnections();
    haHost.portalRunning = false;
    haHostLog(haUiT(HaUiTextEventApStopped));
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
    haHostLog(haUiT(HaUiTextEventGameChanged));
    haPersistenceMarkDirty();
    ENGINE_UNLOCK();
}

bool haHostResetScores(bool discardOnArchiveFailure) {
    bool started = haPersistenceStartNewSession(discardOnArchiveFailure);
    if(started) {
        // New Session is a new party-admission boundary as well as a new score
        // ledger. A cryptographically fresh code is displayed immediately.
        joinCode[0] = '\0';
        haEnsureJoinCode();
    }
    return started;
}

void haHostRoundEnd() {
    ENGINE_LOCK();
    engine.roundEnd();
    haHostLog(haUiT(HaUiTextEventRoundEnded));
    ENGINE_UNLOCK();
}

void haHostCheckpoint() {
    if(!haPersistence.ready) return;
    bool dirty = false;
    ENGINE_LOCK();
    dirty = haPersistence.dirty;
    ENGINE_UNLOCK();
    if(dirty && !haPersistenceCheckpoint(true))
        Serial.println("[ha] requested active checkpoint failed");
}

static bool haPersistSsidConfig(const char* ssid) {
    bool saved = haConfigSave(ssid, haAudioLevel, haLang);
    if(!saved) {
        if(haDiagnostics.sdFailures != UINT32_MAX) haDiagnostics.sdFailures++;
        Serial.println("[ha] config checkpoint failed");
    }
    return saved;
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
    if(!renameAllowed) haHostSetEvent(haUiT(HaUiTextEventResumeApBeforeRename));
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

void haHostDiagnosticsSnapshot(HaDiagnostics& dst) {
    ENGINE_LOCK();
    dst = haDiagnostics;
    ENGINE_UNLOCK();
}

// ---------------- Arduino entry ----------------

// Cardputer and Cardputer Advance microSD use the same dedicated SPI pins:
// SCK=40, MISO=39, MOSI=14, CS=12. This is separate from the display bus, so
// mounting it here doesn't disturb the UI on either supported device.
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
    {
        if(haDiagnostics.sdFailures != UINT32_MAX) haDiagnostics.sdFailures++;
        Serial.println("[ha] SD: no card or mount failed");
    }
}

bool haCfgSave() { // non-static: the UI calls this after any settings mutation
    return haPersistSsidConfig(apName);
}

static bool haPersistenceStorageBegin() {
    if(haPersistenceSnapshot && haPersistenceCandidate && haPersistenceNvsRecord)
        return haActiveNvsStorageBegin();
    HaHost* snapshot = new(std::nothrow) HaHost{};
    HaHost* candidate = new(std::nothrow) HaHost{};
    HaActiveNvsRecord* nvs = new(std::nothrow) HaActiveNvsRecord{};
    if(!snapshot || !candidate || !nvs) {
        delete snapshot;
        delete candidate;
        delete nvs;
        return false;
    }
    haPersistenceSnapshot = snapshot;
    haPersistenceCandidate = candidate;
    haPersistenceNvsRecord = nvs;
    return haActiveNvsStorageBegin();
}

// Shares the history namespace/counter so no-SD sessions remain monotonic when a
// card is later inserted. `floor` protects records created by older firmware that
// did not update the counter beside their active NVS slot.
static uint32_t haPersistenceReserveSessionNumber(uint32_t floor = 0) {
    Preferences preferences;
    if(!preferences.begin("ha_hist", false)) return 0;
    uint32_t high = preferences.getUInt("n", 0);
    if(floor > high) high = floor;
    if(high == UINT32_MAX) {
        preferences.end();
        return 0;
    }
    uint32_t next = high + 1;
    size_t written = preferences.putUInt("n", next);
    preferences.end();
    return written == sizeof(next) ? next : 0;
}

static void haPersistenceInitEmptyHost(HaHost& host) {
    host = HaHost{};
    for(uint8_t pid = 0; pid <= HA_MAX_PLAYERS; pid++)
        host.p[pid].sessionIndex = HA_SESSION_INDEX_NONE;
    host.activeGame = HA_GAME_NONE;
    host.portalRunning = portalRunning;
}

static bool haPersistenceHostFromHistory(const HaHistSession& source, HaHost& host) {
    if(source.count > HA_SESSION_MAX_PLAYERS ||
       source.gameCount > HA_SESSION_GAME_STATS_MAX)
        return false;
    haPersistenceInitEmptyHost(host);
    host.activeGame = source.game;
    for(uint8_t i = 0; i < source.count; i++) {
        HaHostSessionPlayer& destination = host.session[i];
        destination.used = true;
        strlcpy(destination.clientId, source.p[i].clientId, sizeof(destination.clientId));
        strlcpy(destination.avatar, source.p[i].avatar, sizeof(destination.avatar));
        strlcpy(destination.nick, source.p[i].nick, sizeof(destination.nick));
        destination.score = source.p[i].score;
        host.sessionCount++;
    }
    for(uint8_t i = 0; i < source.gameCount; i++) {
        if(source.games[i].game == HA_GAME_NONE || !source.games[i].count) return false;
        host.games[host.gameCount++] = HaHostGamePlay{
            source.games[i].game,
            source.games[i].count
        };
    }
    return true;
}

static bool haPersistenceHostFromNvs(const HaActiveNvsRecord& source, HaHost& host) {
    if(!haActiveNvsRecordValid(source) ||
       source.participantCount > HA_SESSION_MAX_PLAYERS ||
       source.gameCount > HA_SESSION_GAME_STATS_MAX)
        return false;
    haPersistenceInitEmptyHost(host);
    host.activeGame = source.activeGame;
    for(uint8_t i = 0; i < source.participantCount; i++) {
        HaHostSessionPlayer& destination = host.session[i];
        destination.used = true;
        strlcpy(destination.clientId, source.participants[i].identity, sizeof(destination.clientId));
        strlcpy(destination.avatar, source.participants[i].avatar, sizeof(destination.avatar));
        strlcpy(destination.nick, source.participants[i].name, sizeof(destination.nick));
        destination.score = source.participants[i].cumulativeScore;
        host.sessionCount++;
    }
    for(uint8_t i = 0; i < source.gameCount; i++)
        host.games[host.gameCount++] = HaHostGamePlay{
            source.games[i].game,
            source.games[i].count
        };
    return true;
}

static void haPersistenceUseHistoryMetadata() {
    bool wasUsingSd = haPersistence.usingSd;
    uint32_t previousGeneration = haPersistence.generation;
    haPersistence.usingSd = haSdOk && haHistStorageReady();
    if(!haPersistence.usingSd) return;
    haPersistence.sessionNumber = haHistActive.num;
    haPersistence.generation = haHistActive.seq;
    haPersistence.restoredFrom = haHistActive.restoredFrom;
    if(!wasUsingSd || previousGeneration != haPersistence.generation)
        haPersistence.nvsMirrorPending = true;
}

static bool haPersistenceMirrorActiveNvs(bool force) {
    if(!haPersistence.usingSd || !haPersistence.nvsMirrorPending) return true;
    uint32_t now = millis();
    if(!force && !haActiveNvsCheckpointDue(now, false)) return true;
    if(!force && haPersistence.lastNvsAttemptMs &&
       (uint32_t)(now - haPersistence.lastNvsAttemptMs) < HA_PERSIST_RETRY_MS)
        return true;
    haPersistence.lastNvsAttemptMs = now;

    // Reconstruct from the verified SD record, not from newer live RAM, so both
    // media carry exactly one logical generation and SD wins their intentional tie.
    if(!haPersistenceHostFromHistory(haHistActive, *haPersistenceCandidate) ||
       !haActiveNvsCaptureHost(
           *haPersistenceCandidate,
           haHistActive.num,
           haHistActive.restoredFrom,
           *haPersistenceNvsRecord,
           haHistActive.seq)) {
        Serial.println("[ha] active NVS mirror capture rejected");
        return false;
    }
    HaActiveNvsCheckpointResult result = haActiveNvsCheckpointSdMirror(
        *haPersistenceNvsRecord,
        now,
        force);
    if(result == HaActiveNvsCheckpointDeferred) return true;
    if(result != HaActiveNvsCheckpointWritten) {
        Serial.println("[ha] active NVS mirror failed");
        return false;
    }
    haPersistence.nvsGeneration = haHistActive.seq;
    haPersistence.nvsMirrorPending = false;
    return true;
}

static bool haPersistenceAdoptNvsToSd(
    const HaActiveNvsRecord& source,
    const HaHost& host) {
    if(!haSdOk || !haHistStorageReady() || !source.generation) return false;
    haHistFromHost(host, *haHistScratch);
    uint32_t sessionNumber = source.sessionNumber;
    char archivePath[64];
    haHistArchivePath(sessionNumber, archivePath, sizeof(archivePath));
    if(SD.exists(archivePath)) sessionNumber = haHistReserveNum();
    if(!sessionNumber) return false;
    haHistScratch->num = sessionNumber;
    haHistScratch->seq = source.generation;
    haHistScratch->restoredFrom = source.restoredFrom;
    haHistScratch->archived = false;
    return haHistWriteActive(*haHistScratch);
}

static bool haPersistenceInitializeActive() {
    if(!haPersistenceStorageBegin()) return false;
    haActiveNvsResetCheckpointRateLimit();
    bool sdValid = haSdOk && haHistStorageReady() &&
                   haHistActive.num != 0 && haHistActive.seq != 0;
    bool nvsValid = haActiveNvsRead(*haPersistenceNvsRecord);
    haPersistence.nvsGeneration =
        nvsValid ? haPersistenceNvsRecord->generation : 0;
    HaActiveNvsSource source = haActiveNvsChooseSource(
        sdValid,
        sdValid ? haHistActive.seq : 0,
        nvsValid,
        nvsValid ? haPersistenceNvsRecord->generation : 0);

    bool imported = false;
    if(source == HaActiveNvsSourceSd) {
        imported = haPersistenceHostFromHistory(haHistActive, *haPersistenceCandidate);
        haPersistenceUseHistoryMetadata();
    } else if(source == HaActiveNvsSourceNvs) {
        imported = haPersistenceHostFromNvs(*haPersistenceNvsRecord, *haPersistenceCandidate);
        haPersistence.usingSd = false;
        haPersistence.sessionNumber = haPersistenceNvsRecord->sessionNumber;
        haPersistence.generation = haPersistenceNvsRecord->generation;
        haPersistence.restoredFrom = haPersistenceNvsRecord->restoredFrom;
        if(imported && sdValid) {
            if(haPersistenceAdoptNvsToSd(*haPersistenceNvsRecord, *haPersistenceCandidate)) {
                haPersistenceUseHistoryMetadata();
                // The adopted SD record and its source NVS record already share
                // one verified generation and payload unless an immutable archive
                // collision required assigning the active record a new session id.
                haPersistence.nvsMirrorPending =
                    haHistActive.num != haPersistenceNvsRecord->sessionNumber;
            } else {
                // Keep the winning NVS record intact and use it as the active store.
                haSdOk = false;
                Serial.println("[ha] SD active adoption failed; using NVS fallback");
            }
        }
    } else {
        haPersistenceInitEmptyHost(*haPersistenceCandidate);
        imported = true;
        haPersistence.usingSd = false;
        haPersistence.sessionNumber = haPersistenceReserveSessionNumber();
        // A newly formatted SD card establishes its first empty active slot at
        // sequence 1. Reserve that value for the SD baseline so the first real
        // no-card checkpoint starts at generation 2; otherwise inserting a fresh
        // card later would create an equal-generation tie, which SD intentionally
        // wins, and discard the live NVS session before it can be adopted.
        haPersistence.generation = 2;
        haPersistence.restoredFrom = 0;
        if(!haPersistence.sessionNumber) return false;
    }
    if(!imported) return false;

    if(haPersistence.usingSd) {
        // Repair missing, stale, or equal-generation-conflicting NVS from the SD
        // winner. Failure degrades to SD-only operation and is retried from loop().
        if(!haPersistence.nvsMirrorPending &&
           haPersistence.nvsGeneration == haPersistence.generation) {
            haActiveNvsSetCheckpointBaseline(millis());
        } else if(!haPersistenceMirrorActiveNvs(true)) {
            Serial.println("[ha] initial active NVS mirror deferred after failure");
        }
    } else if(source == HaActiveNvsSourceNvs) {
        // Avoid an immediate wear write merely because runtime bookkeeping reset.
        haActiveNvsSetCheckpointBaseline(millis());
    }

    ENGINE_LOCK();
    haHost = *haPersistenceCandidate;
    haHostTouch();
    haPersistence.dirty = source == HaActiveNvsSourceNone;
    ENGINE_UNLOCK();
    haPersistence.ready = true;
    return true;
}

static void haPersistenceRecordSuccess(const HaHost& snapshot) {
    ENGINE_LOCK();
    haPersistence.dirty = haHost.rev != snapshot.rev;
    ENGINE_UNLOCK();
}

static void haPersistenceRecordFailure() {
    ENGINE_LOCK();
    haPersistence.dirty = true;
    if(haDiagnostics.sdFailures != UINT32_MAX) haDiagnostics.sdFailures++;
    ENGINE_UNLOCK();
}

static bool haPersistenceActivateNvsFallback(uint8_t generationAdvance = 1) {
    if(!haPersistence.usingSd) return true;
    if((uint32_t)generationAdvance > UINT32_MAX - haPersistence.generation) {
        Serial.println("[ha] active generation exhausted; NVS cannot outrank SD");
        return false;
    }
    haPersistence.usingSd = false;
    haSdOk = false;
    haPersistence.nvsMirrorPending = false;
    haActiveNvsResetCheckpointRateLimit();
    // Boot intentionally gives SD an equal-generation tie. The first fallback
    // record must therefore outrank the last verified SD slot, or reinserting
    // the failed card could resurrect the older state.
    haPersistence.generation += generationAdvance;
    return true;
}

static bool haPersistenceCheckpointSnapshot(const HaHost& snapshot, bool force) {
    haPersistence.lastAttemptMs = millis();
    if(haPersistence.usingSd && haSdOk) {
        if(haHistCheckpoint(snapshot, force)) {
            haPersistenceUseHistoryMetadata();
            if(!haPersistenceMirrorActiveNvs(force))
                Serial.println("[ha] active session is safe on SD; NVS mirror pending");
            haPersistenceRecordSuccess(snapshot);
            return true;
        }
        // A mounted card can still be removed or fail during a write. Continue from
        // the last known session metadata in the bounded NVS fallback.
        Serial.println("[ha] SD checkpoint failed; switching active session to NVS");
        if(!haPersistenceActivateNvsFallback()) {
            haPersistenceRecordFailure();
            return false;
        }
        force = true;
    }

    if(!haActiveNvsCaptureHost(
           snapshot,
           haPersistence.sessionNumber,
           haPersistence.restoredFrom,
           *haPersistenceNvsRecord,
           haPersistence.generation)) {
        Serial.println("[ha] active NVS capture rejected (stable client ids unavailable?)");
        haPersistenceRecordFailure();
        return false;
    }
    HaActiveNvsCheckpointResult result = haActiveNvsCheckpointNoSd(
        *haPersistenceNvsRecord,
        millis(),
        force);
    if(result == HaActiveNvsCheckpointDeferred) {
        haPersistenceRecordFailure();
        return false;
    }
    if(result != HaActiveNvsCheckpointWritten) {
        Serial.println("[ha] active NVS checkpoint failed");
        haPersistenceRecordFailure();
        return false;
    }
    uint32_t generation = 0;
    if(haActiveNvsLatestGeneration(generation)) {
        haPersistence.generation = generation;
        haPersistence.nvsGeneration = generation;
    }
    haPersistence.nvsMirrorPending = false;
    haPersistenceRecordSuccess(snapshot);
    return true;
}

static bool haPersistenceCheckpoint(bool force) {
    if(!haPersistence.ready || !haPersistenceSnapshot) return false;
    haHostSnapshot(*haPersistenceSnapshot);
    return haPersistenceCheckpointSnapshot(*haPersistenceSnapshot, force);
}

static void haPersistenceBeginTransaction() {
    ENGINE_LOCK();
    haPersistenceTransaction = true;
    haPersistencePendingDisconnectCount = 0;
    ENGINE_UNLOCK();
}

static void haPersistenceEndTransaction() {
    ENGINE_LOCK();
    haPersistenceTransaction = false;
    for(uint8_t i = 0; i < haPersistencePendingDisconnectCount; i++) {
        const HaPendingDisconnect& pending = haPersistencePendingDisconnects[i];
        engine.onWsDisconnect(pending.wsId, pending.rawAt);
    }
    haPersistencePendingDisconnectCount = 0;
    ENGINE_UNLOCK();
}

static void haPersistenceZeroSessionScores(HaHost& host) {
    for(uint8_t i = 0; i < HA_SESSION_MAX_PLAYERS; i++)
        if(host.session[i].used) host.session[i].score = 0;
    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++)
        if(host.p[pid].used) host.p[pid].score = 0;
    for(uint8_t i = 0; i < HA_SESSION_GAME_STATS_MAX; i++)
        host.games[i] = HaHostGamePlay{};
    host.gameCount = 0;
}

static bool haPersistenceStartNewSession(bool discardOnArchiveFailure) {
    if(!haPersistence.ready) return false;
    haPersistenceBeginTransaction();
    haHostSnapshot(*haPersistenceSnapshot);
    bool havePlayers = haPersistenceSnapshot->sessionCount > 0;
    bool startedOnSd = haPersistence.usingSd;
    uint32_t previousSessionNumber = haPersistence.sessionNumber;
    uint32_t previousRestoredFrom = haPersistence.restoredFrom;
    bool startWithoutArchive = false;

    if(!haPersistence.usingSd && havePlayers && !discardOnArchiveFailure) {
        ENGINE_LOCK();
        haHostLog(haUiT(HaUiTextEventArchiveUnavailable));
        ENGINE_UNLOCK();
        haPersistenceEndTransaction();
        return false;
    }

    if(haPersistence.usingSd && havePlayers) {
        if(!haHistArchive(*haPersistenceSnapshot)) {
            if(discardOnArchiveFailure) {
                startWithoutArchive = true;
            } else {
                ENGINE_LOCK();
                haHostLog(haUiT(HaUiTextEventArchiveFailed));
                haPersistenceMarkDirty();
                ENGINE_UNLOCK();
                haPersistenceEndTransaction();
                return false;
            }
        } else {
            haPersistenceUseHistoryMetadata();
        }
        if(startWithoutArchive) {
            ENGINE_LOCK();
            haHostLog(haUiT(HaUiTextEventArchiveSkipped));
            ENGINE_UNLOCK();
        }
    } else if(haPersistence.usingSd && !havePlayers) {
        // There is nothing immutable to archive, but still clear play counts and
        // any transient engine score state in the existing empty active session.
    } else {
        uint32_t currentGeneration = 0;
        if(haActiveNvsLatestGeneration(currentGeneration) &&
           currentGeneration == UINT32_MAX) {
            ENGINE_LOCK();
            haHostLog(haUiT(HaUiTextEventGenerationExhausted));
            ENGINE_UNLOCK();
            haPersistenceEndTransaction();
            return false;
        }
        uint32_t next = haPersistenceReserveSessionNumber(haPersistence.sessionNumber);
        if(!next) {
            ENGINE_LOCK();
            haHostLog(haUiT(HaUiTextEventSessionIdFailed));
            ENGINE_UNLOCK();
            haPersistenceEndTransaction();
            return false;
        }
        haPersistence.sessionNumber = next;
        haPersistence.restoredFrom = 0;
    }

    // Commit the replacement active record before changing the engine or host
    // mirror. The A/B media scheme leaves the prior boot state valid on a torn
    // write; after an ambiguous media failure, immediately supersede the candidate
    // with the unchanged snapshot before reporting that the transaction failed.
    *haPersistenceCandidate = *haPersistenceSnapshot;
    haPersistenceZeroSessionScores(*haPersistenceCandidate);
    bool replacementProtected = false;
    if(startWithoutArchive) {
        replacementProtected = haHistStartNewActive(*haPersistenceCandidate);
        if(replacementProtected) {
            haPersistenceUseHistoryMetadata();
            haPersistence.lastAttemptMs = millis();
            if(!haPersistenceMirrorActiveNvs(true))
                Serial.println("[ha] discarded-session NVS mirror remains pending");
            haPersistenceRecordSuccess(*haPersistenceCandidate);
        }
    } else {
        replacementProtected = haPersistenceCheckpointSnapshot(
            *haPersistenceCandidate,
            true);
    }
    if(!replacementProtected) {
        bool protectedCurrent = false;
        if(startedOnSd) {
            protectedCurrent = haHistCheckpointPrepared(
                *haPersistenceSnapshot,
                true,
                previousRestoredFrom);
            if(protectedCurrent) {
                // The candidate write may have marked SD unavailable before its
                // NVS fallback also failed. A fully verified compensating write
                // proves the mounted card is usable again, so restore SD authority
                // instead of retrying the unchanged session into a bad fallback.
                haSdOk = true;
                haPersistenceUseHistoryMetadata();
                if(!haPersistenceMirrorActiveNvs(true))
                    Serial.println("[ha] rollback NVS mirror remains pending");
                haPersistenceRecordSuccess(*haPersistenceSnapshot);
            } else {
                // The failed candidate and failed rollback may each have left a
                // readable seq+1 SD slot. Protect the unchanged RAM state in NVS
                // at seq+2 so it wins over every ambiguous on-card outcome.
                haPersistence.restoredFrom = previousRestoredFrom;
                bool advanced = false;
                if(haPersistence.usingSd) {
                    advanced = haPersistenceActivateNvsFallback(2);
                } else if(haPersistence.generation < UINT32_MAX) {
                    haPersistence.generation++;
                    advanced = true;
                }
                protectedCurrent = advanced &&
                                   haPersistenceCheckpointSnapshot(
                                       *haPersistenceSnapshot,
                                       true);
            }
        } else {
            haPersistence.sessionNumber = previousSessionNumber;
            haPersistence.restoredFrom = previousRestoredFrom;
            // A verified-read failure can still follow a physically committed
            // NVS candidate. Immediately write the unchanged snapshot at the next
            // generation rather than relying on a later scheduler tick.
            protectedCurrent = haPersistenceCheckpointSnapshot(
                *haPersistenceSnapshot,
                true);
        }
        if(!protectedCurrent)
            Serial.println("[ha] new-session rollback could not protect active state");
        ENGINE_LOCK();
        haHostLog(haUiT(HaUiTextEventCheckpointFailed));
        haPersistence.dirty = !protectedCurrent;
        ENGINE_UNLOCK();
        haPersistenceEndTransaction();
        return false;
    }

    ENGINE_LOCK();
    engine.resetScores();
    haHostResetSessionScores();
    haHostLog(haUiT(HaUiTextEventNewSession));
    haPersistence.dirty = false;
    ENGINE_UNLOCK();
    haPersistenceEndTransaction();
    return true;
}

static bool haPersistenceRestoreHistory(const HaHistSession& session) {
    if(!haPersistence.ready || !haPersistence.usingSd || !session.archived ||
       !haPersistenceHostFromHistory(session, *haPersistenceCandidate))
        return false;

    haPersistenceBeginTransaction();
    haHostSnapshot(*haPersistenceSnapshot);
    uint32_t previousRestoredFrom = haPersistence.restoredFrom;
    if(haPersistenceSnapshot->sessionCount > 0) {
        if(!haHistArchive(*haPersistenceSnapshot)) {
            haPersistenceEndTransaction();
            return false;
        }
        haPersistenceUseHistoryMetadata();
    }

    if(!haHistStartRestoredActive(*haPersistenceCandidate, session.num)) {
        // Archive may already have advanced the active slot. Put the unchanged RAM
        // session back into that slot so a failed restore never changes boot state.
        bool rolledBack = haHistCheckpointPrepared(
            *haPersistenceSnapshot,
            true,
            previousRestoredFrom);
        if(rolledBack) {
            haPersistenceUseHistoryMetadata();
            haPersistence.lastAttemptMs = millis();
            if(!haPersistenceMirrorActiveNvs(true))
                Serial.println("[ha] restore-rollback NVS mirror remains pending");
            haPersistenceRecordSuccess(*haPersistenceSnapshot);
        } else {
            // A failed rollback means the card cannot currently protect the live
            // RAM session. Move that unchanged snapshot to a strictly newer NVS
            // generation before releasing the transaction gate.
            Serial.println("[ha] restore rollback failed; protecting active state in NVS");
            // Both failed SD writes may have placed a valid seq+1 record even
            // though verification could not read it back. Advance twice so the
            // unchanged NVS snapshot wins over that ambiguous commit as well.
            bool protectedInNvs = haPersistenceActivateNvsFallback(2) &&
                                  haPersistenceCheckpointSnapshot(
                                      *haPersistenceSnapshot,
                                      true);
            if(!protectedInNvs) {
                ENGINE_LOCK();
                haHostLog(haUiT(HaUiTextEventRestoreStorageFailed));
                haPersistenceMarkDirty();
                ENGINE_UNLOCK();
            }
        }
        haPersistenceEndTransaction();
        return false;
    }
    haPersistenceUseHistoryMetadata();
    haPersistence.lastAttemptMs = millis();
    if(!haPersistenceMirrorActiveNvs(true))
        Serial.println("[ha] restored session NVS mirror remains pending");

    // Existing phones must hello again because engine pids are intentionally not
    // persisted. closeAll() invokes disconnect callbacks, which the gate queues.
    ws.closeAll();
    ENGINE_LOCK();
    // Disconnects captured before this reset refer only to the discarded Engine
    // roster. Do not replay their earlier raw timestamps into the fresh clock.
    haPersistencePendingDisconnectCount = 0;
    engine.reset(millis());
    engine.setLang(HA_LANG_CODE[haLang]);
    haHost = *haPersistenceCandidate;
    haHost.portalRunning = portalRunning;
    if(haHost.activeGame != HA_GAME_NONE) engine.selectGame(haHost.activeGame);
    if(!portalRunning) engine.transportPause(millis());
    // The restored game is a fresh lobby with no suspended engine seats. Any
    // barrier captured for the discarded session is now obsolete.
    haApRequiredClear(haApRequiredRoster);
    haApReconnectStartedMs = 0;
    haApReconnectExpiryApplied = false;
    haApState = haApStateAfterHistoryRestore(portalRunning);
    haHostLog(haUiT(HaUiTextEventHistoryRestored));
    haPersistence.dirty = false;
    ENGINE_UNLOCK();
    haPersistenceEndTransaction();
    return true;
}

static void haPersistenceTick() {
    if(!haPersistence.ready) return;
    uint32_t now = millis();
    if((uint32_t)(now - haPersistence.lastProbeMs) < HA_PERSIST_PROBE_MS) return;
    haPersistence.lastProbeMs = now;
    if((uint32_t)(now - haPersistence.lastConfigRepairMs) >= HA_CONFIG_REPAIR_MS) {
        haPersistence.lastConfigRepairMs = now;
        if(!haConfigRepairMirrors())
            Serial.println("[ha] config mirror repair remains pending");
    }
    // A forced UI checkpoint can advance the history sequence independently of
    // this timer. Keep the cross-media generation floor synchronized regardless.
    if(haPersistence.usingSd) haPersistenceUseHistoryMetadata();

    bool dirty = false;
    ENGINE_LOCK();
    dirty = haPersistence.dirty;
    ENGINE_UNLOCK();
    if(haPersistence.usingSd) {
        if(dirty &&
           (uint32_t)(now - haPersistence.lastAttemptMs) >=
               HA_PERSIST_SD_COALESCE_MS) {
            (void)haPersistenceCheckpoint(false);
        } else if(haPersistence.nvsMirrorPending) {
            (void)haPersistenceMirrorActiveNvs(false);
        }
        return;
    }
    if(!dirty ||
       (uint32_t)(now - haPersistence.lastAttemptMs) <
           HA_ACTIVE_NVS_MIN_CHECKPOINT_MS)
        return;
    (void)haPersistenceCheckpoint(false);
}

static void haStartupFatal(const char* reason) {
    Serial.printf("[ha] fatal startup: %s\n", reason ? reason : "unknown");
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5Cardputer.Display.drawString("HOTSPOT ARCADE", 8, 18);
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.drawString("Startup failed:", 8, 42);
    M5Cardputer.Display.drawString(reason ? reason : "unknown", 8, 56);
    // Deliberately do not confirm an OTA image that cannot initialize its critical
    // mutex/heap state. M5Launcher can roll it back on the next reset.
    while(true) {
        M5Cardputer.update();
        delay(1000);
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    static_assert(
        (uint16_t)m5::board_t::board_M5Cardputer == HA_BOARD_ID_CARDPUTER,
        "M5Unified Cardputer board id changed");
    static_assert(
        (uint16_t)m5::board_t::board_M5CardputerADV ==
            HA_BOARD_ID_CARDPUTER_ADVANCE,
        "M5Unified Cardputer Advance board id changed");
    const uint16_t boardId = (uint16_t)M5.getBoard();
    const HaDeviceKind deviceKind = haDeviceClassify(boardId);
    haDiagnostics.boardId = boardId;
    haDiagnostics.deviceKind = deviceKind;
    Serial.printf(
        "[ha] device: %s (M5 board id %u)\n",
        haDeviceName(deviceKind),
        (unsigned)boardId);
    if(!haDeviceSupported(deviceKind)) haStartupFatal("unsupported M5 board");

    engineMutex = xSemaphoreCreateRecursiveMutex();
    if(!engineMutex) haStartupFatal("engine mutex");
    if(!haHostBegin()) haStartupFatal("host memory");
    if(!haPersistenceStorageBegin()) haStartupFatal("persistence memory");
    haUiBegin();
    if(!haUiSnapStorage) haStartupFatal("UI snapshot memory");

    haSdBegin();
    if(!haConfigBegin(apName, haAudioLevel, haLang)) haStartupFatal("configuration");
    const HaConfigRecord& savedConfig = haConfigGet();
    strlcpy(apName, savedConfig.ssid, sizeof(apName));
    haAudioLevel = savedConfig.audio;
    haLang = savedConfig.language;
    haUiSetLocaleFromLanguage(haLang);

    bool historyReady = false;
    if(haSdOk) {
        if(!haHistStorageBegin()) haStartupFatal("history memory");
        historyReady = haHistBegin();
        if(!historyReady) {
            haSdOk = false;
            Serial.println("[ha] history unavailable; using active NVS fallback");
        }
    }
    if(historyReady) {
        haHistSetRestoreHandler(haPersistenceRestoreHistory);
        haConfigMigrationDone();
    }

    haHostReset();
    if(!haPersistenceInitializeActive()) haStartupFatal("active session recovery");

    bool contentReady = false;
    ENGINE_LOCK();
    engine.reset(millis());
    contentReady = haContentLoadAll(engine, HA_LANG_CODE[haLang]);
    if(contentReady) {
        if(haHost.activeGame != HA_GAME_NONE) engine.selectGame(haHost.activeGame);
        // Setup work and first-start storage I/O are not game time. AP startup
        // resumes this clock only after the AP/DNS/HTTP transport is ready.
        engine.transportPause(millis());
        haHostLog(haUiT(HaUiTextEventPacksLoaded));
    }
    ENGINE_UNLOCK();
    if(!contentReady) haStartupFatal("content packs");
    haLoadedLang = haLang;

    if(!installHandlers()) haStartupFatal("HTTP handler memory");
    if(haPersistence.dirty && !haPersistenceCheckpoint(true))
        haStartupFatal("initial active checkpoint");
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
    static uint32_t previousLoopMs = millis();
    uint32_t now = millis();
    uint32_t loopGap = now - previousLoopMs;
    previousLoopMs = now;
    if(loopGap > haDiagnostics.maxLoopGapMs) haDiagnostics.maxLoopGapMs = loopGap;

    M5Cardputer.update();
    haUiPumpKeys();
    haSoundPump();

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
                haUiT(HaUiTextEventLanguageFormat),
                HA_LANG_NAME[requestedLang % HA_LANG_COUNT]);
            haHostLog(event);
        } else {
            // The staged loader retains the previous content/language on failure.
            haHostLog(haUiT(HaUiTextEventLanguageLoadFailed));
        }
        ENGINE_UNLOCK();
        if(!contentReady) {
            haLang = haLoadedLang;
            haUiSetLocaleFromLanguage(haLang);
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

    haPersistenceTick();
    haDiagnostics.freeHeap = ESP.getFreeHeap();
    haDiagnostics.minFreeHeap = ESP.getMinFreeHeap();
    haDiagnostics.largestFreeBlock =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    haDiagnostics.soundDropped = haSoundQueue.dropped();
    haDiagnostics.checkpointGeneration = haPersistence.generation;
    haUiTick();
    delay(1); // let Wi-Fi/AsyncTCP/idle tasks run between host frames
}
