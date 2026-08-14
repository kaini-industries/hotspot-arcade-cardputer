# Protocol and state contracts

This downstream build embeds the firmware protocol-v22 engine and browser protocol v2 from
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

## Capacity and game registry

The downstream build contract is exactly ten authenticated phone players and five
simultaneous matches in every 1v1 implementation: Connect Four, Tic-Tac-Toe, Dots &
Boxes, Reversi, Pong, Battleship, and Chess. Capacity exhaustion returns
`{"t":"error","code":"match_capacity"}`; limits are compile-time asserted rather
than inherited from the larger upstream defaults.

The current manifest maps the 20 supported games as follows:

| ID | Game | ID | Game |
| ---: | --- | ---: | --- |
| 1 | Trivia | 11 | Guess the Color |
| 2 | Connect Four | 12 | Battleship |
| 3 | Tic-Tac-Toe | 13 | Spectrum |
| 4 | Dots & Boxes | 14 | Kiss Marry Kill |
| 5 | Drawing | 15 | Chess |
| 6 | Pong | 16 | Secrets |
| 7 | Reaction Duel | 17 | Fill the Blank |
| 8 | Would You Rather | 18 | Werewolf |
| 9 | Word Scramble | 19 | Spyfall |
| 10 | Reversi | 20 | Draw a Monster |

IDs are protocol keys, not array positions. Even though the current assignments span
1 through 20, clients and downstream code must treat them as sparse and use
`tools/content-manifest.json` for membership, labels, pack metadata, and display
order.

## Game selection and content transactions

The Cardputer is the only game-selection authority. Browser clients retain the
`{"t":"proposeGame","game":"spyfall"}` verb for compatibility, but this adapter
does not enqueue flash I/O from a WebSocket callback. It replies without changing
state:

```json
{"t":"result","event":"game_change","status":"policy_denied","game":"spyfall","id":19}
```

Protocol v22 replaces all-games content replacement with an active-game transaction.
`CONTENT_BEGIN` names the target game and requested locale; `CONTENT_PACK` and
`CONTENT_ITEM` populate one typed staging bank; `CONTENT_COMMIT` supplies exact
little-endian 16-bit pack/item counts; and `CONTENT_ABORT` discards staging. The old
`SELECT_GAME` message is deprecated for content-bearing games. The Cardputer calls
the equivalent engine methods directly on its loop task.

At most one `ContentBank` is live and one is staged. A successful commit atomically
publishes the staged bank, resets the target game to its lobby, then sends exactly one
locale configuration and one authoritative state. Re-selecting the same game for a
locale change preserves its phone scores; selecting a different game resets them.
Any allocation, parse, count, or validation failure aborts staging and leaves the
previous game, bank, locale, round, and scores unchanged.

For content-bearing games, requested `de` and `pt-br` locales follow their explicit
manifest fallback to `en` when no translated pack set exists for that game. The
choice applies to the complete game bank: translated and English packs are never
mixed, and availability may differ by locale. The requested locale remains active
for phone UI configuration even when English pack bytes are used. Spectrum's v22
Wild Card pack is English-only and is not inserted into either translated Spectrum
bank. Packless games commit an empty typed bank. Bank allocation prefers PSRAM. An
internal fallback must leave 64 KiB free after the actual allocation; content-string
mutation and commit require 96 KiB beforehand, including 32 KiB of headroom for
Frankendraw's bounded fallback store. The content manifest declares every pack key's
exact engine-buffer byte ceiling; generation rejects oversized UTF-8 values, edge
whitespace, and ASCII control bytes before firmware compilation.

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
ledger accumulates every typed engine score award across games until New Session.
Selecting/replaying a game may reset its browser score but does not subtract from the
cumulative ledger. All awards originate from the engine's centralized score path;
host event text is never parsed to infer points.

## Host event surfaces

The Cardputer's localized recent-event surface accepts a bounded typed host-event
frame:

```text
version:u8 kind:u8 game:u8 actor_pid:u8 target_pid:u8 value:i16 text:utf8[0..96]
```

Event version is 1. The seven kinds are `MATCH_STARTED`, `CHAT`, `ROLE`,
`ROUND_WIN`, `ROUND_DRAW`, `ROUND_COMPLETE`, and `GAME_FINAL`. Text truncation stops
at a valid UTF-8 boundary. The Cardputer formats these events into a 24-entry ring;
unknown versions/kinds are ignored rather than interpreted as JSON.

Protocol v22 removes the legacy generic event and round-result callbacks in favor of
the semantic host-event sink. Streamed finished art remains a separate host output;
the Cardputer intentionally discards that host stream instead of persisting it, while
phones receive their bounded `fdart` WebSocket picture. Only typed host events drive
the localized bounded ring, and that ring itself is not persisted.

## Persistence contract

The following are durable: session number/provenance, stable identity digests,
nicknames, avatars, cumulative scores, selected game, sparse game-play counts, and
settings. The following are transient: raw tokens, sockets/PIDs across reboot, phone
game scores, current round state, chat, Draw strokes, host-directed finished art,
and the typed host-event log.

Reboot restores the durable roster and cumulative standings, then starts the selected
game in a fresh lobby. Restoring immutable history creates a new active session with
`restored_from`; it never reactivates or modifies the archive.

## Security boundary

The six-digit code prevents casual uninvited joins; it does not provide encryption or
proof against a nearby observer. Wi-Fi is open and both HTTP and WebSocket traffic are
plaintext. Never send secrets through the app. WPA2 is a separate future feature.
