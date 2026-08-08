# Protocol and state contracts

This downstream build embeds the protocol-v18 engine and browser protocol v2 from
the exact source in `UPSTREAM.lock.json`. The authoritative upstream UART reference
lives in the sibling `hotspot-arcade/docs/PROTOCOL.md`; this document records the
Cardputer-facing public contract and downstream policies.

## Browser handshake

The WebSocket endpoint is `ws://192.168.4.1/ws`. Each message is one JSON object with
a string `t` field.

Client to server:

```json
{"t":"hello","proto":2,"resume":"0123456789abcdef0123456789abcdef","nick":"NOVA","avatar":"🙂","code":"123456"}
```

- `resume` is exactly 32 lowercase hexadecimal characters. The browser creates it
  from 128 cryptographically random bits and retains it locally.
- `code` is required for an unknown identity and optional for a known identity.
- The server derives `SHA-256(resume)[0..15]`. It never logs, echoes, or persists the
  raw resume token.

Successful server response:

```json
{"t":"welcome","proto":2,"session":"<32hex>","pid":3,"nick":"NOVA","avatar":"🙂","lang":"de","resumed":true,"paused":false}
```

Rejection:

```json
{"t":"reject","code":"auth_required|bad_code|throttled|full|bad_protocol","retry_ms":60000}
```

`retry_ms` is present for throttling. Unknown-code attempts are limited to five per
minute per AP client address and thirty globally, followed by a 60-second lockout.
The Cardputer accepts ten authenticated players, two pending hello sockets, and
twelve temporary WebSocket objects total.

## Configuration and planned downtime

Locale changes are pushed to existing and resumed clients:

```json
{"t":"config","lang":"de"}
```

Before planned SSID change or AP-off:

```json
{"t":"server_pause","reason":"ssid_change|ap_off","ssid":"New SSID","reconnect_ms":600000}
```

After transport returns, the server emits `{"t":"server_resume"}` when play
actually resumes. Planned downtime freezes the logical clock, deadline expiry,
challenges, quorum, and exact engine state. The ten-minute value is a host return
window, not the ordinary disconnect grace.

## Presence, seats, and identity

Roster entries include `"online":true|false`. A disconnected identity keeps its PID
and exact game state for 120 seconds. Offline players cannot be challenged and do not
satisfy ordinary party quorum; affected 1v1 matches and role-critical party rounds
pause. Expiry invokes the normal game-specific leave/forfeit once and frees the PID.

The cumulative Cardputer identity remains after PID expiry. A later return gets a new
PID and game score, but retains its cumulative session score. If a second socket uses
the same token while the first is active, the newest socket takes over and the old
socket is closed.

## Challenges and timing

The server assigns every challenge an ID. Lobby challenge objects contain
`{"id":7,"from":2,"to":5}` and acceptance sends `{"t":"accept","id":7}`.
Challenges disappear when either endpoint disconnects, leaves, changes games, or
enters a match. Capacity exhaustion returns:

```json
{"t":"error","code":"match_capacity"}
```

Timed state sends relative `remaining_ms` and `duration_ms`, never raw ESP deadlines
or uptime. Chess additionally uses relative clock fields. All numeric intents are
range-checked; Draw coordinates must be present, finite, and normalized before relay.

## Scores

Browser-visible `score` belongs to the currently selected game. The Cardputer host
ledger accumulates every typed engine score award across games for the in-memory host
session. The next stacked persistence/history change adds the confirmed New Session
operation. Selecting/replaying a game may reset its browser score but does not
subtract from the cumulative ledger. All awards originate from the engine's
centralized score path; host event text is never parsed to infer points.

## Typed host events

Firmware protocol 18 replaces ad hoc result JSON with a bounded typed event frame:

```text
version:u8 kind:u8 game:u8 actor_pid:u8 target_pid:u8 value:i16 text:utf8[0..96]
```

Event version is 1. The seven kinds are `MATCH_STARTED`, `CHAT`, `ROLE`,
`ROUND_WIN`, `ROUND_DRAW`, `ROUND_COMPLETE`, and `GAME_FINAL`. Text truncation stops
at a valid UTF-8 boundary. The Cardputer formats these events into a 24-entry ring;
unknown versions/kinds are ignored rather than interpreted as JSON.

## Persistence boundary in this tranche

Protocol/session state in this branch is in memory, except for the legacy microSD
settings record. Raw resume tokens always remain browser-only. The next stacked
change adds redundant SD/NVS active-state recovery and immutable history; until then,
a reboot starts a new roster and cumulative ledger.

## Security boundary

The six-digit code prevents casual uninvited joins; it does not provide encryption or
proof against a nearby observer. Wi-Fi is open and both HTTP and WebSocket traffic are
plaintext. Never send secrets through the app. WPA2 is a separate future feature.
