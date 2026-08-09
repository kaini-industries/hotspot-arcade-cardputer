<p align="center">
  <img src="docs/img/logo.png" alt="Hotspot Arcade — M5Stack Cardputer Edition" width="600">
</p>

# Hotspot Arcade — M5Stack Cardputer + Cardputer-Adv

[![build](https://github.com/kaini-industries/hotspot-arcade-cardputer/actions/workflows/ci.yml/badge.svg)](https://github.com/kaini-industries/hotspot-arcade-cardputer/actions/workflows/ci.yml)
[![latest release](https://img.shields.io/github/v/release/kaini-industries/hotspot-arcade-cardputer?sort=semver)](https://github.com/kaini-industries/hotspot-arcade-cardputer/releases/latest)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Hotspot Arcade turns a Cardputer or Cardputer-Adv into an offline party-game host.
It creates a Wi-Fi access point and captive web app; guests play from phone browsers
while the M5Stack device controls the game, keeps the cumulative standings, and
stores session history. No internet connection or phone app is required.

This Kaini Industries-maintained edition is a Cardputer host port built from the
[Kaini Industries Hotspot Arcade source](https://github.com/kaini-industries/hotspot-arcade),
a maintained fork of [Tarik Caramanico's original Hotspot Arcade](https://github.com/tarikbc/hotspot-arcade).
The original Cardputer port was created by `genkigenki`; both original attributions
are retained. The exact unmodified source commit is pinned in [UPSTREAM.md](UPSTREAM.md).

## v0.6.0 highlights

- Ten authenticated phone players and five concurrent matches in each 1v1 game.
- One firmware image for the original M5Stack Cardputer and Cardputer-Adv, with
  runtime board identification and refusal on unsupported M5 boards.
- Browser protocol v2 with stable resume identities, duplicate-tab takeover, and a
  two-minute transient reconnect grace.
- Open Wi-Fi with a cryptographically generated six-digit party join code.
- Per-game phone scores and a separate 32-person cumulative Cardputer ledger.
- Crash-safe active-session recovery, full microSD history, browsing, and restore.
- Planned AP pause/rename without resetting play, plus a ten-minute return window.
- Bounded socket admission, flow control, rate limits, typed host events, and live
  diagnostics.
- Reproducible, provenance-locked builds with checksums, SPDX SBOM, attestations,
  strict ESP image validation, and deterministic release packaging.

See [the release notes](docs/RELEASE_NOTES.md), [architecture](docs/ARCHITECTURE.md),
and [protocol contract](docs/PROTOCOL.md) for details.

## Hardware and status

The same firmware image targets both the original M5Stack Cardputer and the
Cardputer-Adv (also called Cardputer Advance). At startup it accepts only the
M5Unified runtime identities `M5Cardputer` (board ID 14) and `M5CardputerADV`
(board ID 24), prints and displays the detected model, and stops before OTA health
confirmation on any other board identity.

The locked universal build uses 1,412,448 bytes of the 3,342,336-byte app
partition and 58,248 bytes of static DRAM, leaving 1,929,888 bytes of image
headroom and 269,432 bytes for stack and heap at link time.

The locked M5GFX 0.2.26 source is amended only by the exact reviewed upstream
Cardputer-Adv detection fix described under [Developer setup](#developer-setup).
Automated tests cover the classifier and provenance, but release support is not
accepted until the complete [two-device hardware matrix](docs/HARDWARE_ACCEPTANCE.md)
passes on physical examples of both models. M5Burner catalog identity/publication is
deliberately deferred and is not evidence of hardware compatibility.

## Install

Each release artifact is a single universal image; do not look for a separate
`-adv` build. The artifacts support two installation layouts on either device:

- `hotspot-arcade-cardputer.ino.bin` at `0x170000` keeps M5Launcher.
- `hotspot-arcade-cardputer.full.bin` at `0x0` replaces the whole flash layout.

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write-flash 0x170000 hotspot-arcade-cardputer.ino.bin

# Or replace everything, including M5Launcher:
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write-flash 0x0 hotspot-arcade-cardputer.full.bin
```

The app image can also be copied to microSD and launched from M5Launcher. If a board
does not enter USB download mode automatically:

- On the original Cardputer, hold `G0` while connecting USB-C.
- On Cardputer-Adv, switch power off, hold `G0`, switch power on, then release `G0`
  after the USB download port appears.

Only use artifacts from the Kaini Industries release page or a reviewed CI release
candidate, and verify them against `SHA256SUMS`. See the
[hardware acceptance procedure](docs/HARDWARE_ACCEPTANCE.md) before treating a
candidate as supported.

## Start and join

At boot, the Cardputer displays the open AP name, `http://192.168.4.1`, and a
six-digit join code. A guest joins the AP, opens the captive page, chooses a nickname
and avatar, and enters that code. The browser retains its own random resume token;
the device stores only a one-way-derived 128-bit identity key.

Known identities can reconnect without re-entering the code. A normal disconnect
keeps the exact engine seat for 120 seconds. Returning later receives a fresh PID and
fresh score for the selected game, while the Cardputer's cumulative session score
remains. A newer connection using the same browser identity takes over from the old
socket.

The join code is admission control, not encryption. The AP, HTTP, and WebSocket are
open, so a nearby observer can read traffic or the displayed code. Do not use it for
private data. WPA2 is intentionally deferred.

## Host controls

| Key | Action |
| --- | --- |
| `G` | Choose a game; `;`/`.` move, `Enter` selects, `S` changes sort order. |
| `L` | Browse the cumulative session leaderboard. |
| `H` | Browse immutable history newest-first; inspect or restore a session. |
| `C` | Browse all 24 retained typed host events. |
| `S` | Open settings for SSID, audio, locale, AP, events, and diagnostics. |
| `D` | Open diagnostics directly. |
| `N` | Rename the AP using a planned pause and checkpoint. |
| `P` | Pause/start the AP, or resume early during the reconnect window. |
| `E` | End the current round. |
| `R` | Archive and start a new cumulative session, with confirmation. |
| `Esc` | Return toward the dashboard. |

Changing the SSID or stopping the AP freezes the logical game clock and checkpoints
the session before transport teardown. Manual AP-off remains paused indefinitely.
After restart the host waits up to ten minutes for required players, resumes when
they return, or lets the host resume/end early. If a new SSID fails, the prior SSID
is restored.

## Scores, restart, and history

- Phones show scores for the currently selected game only.
- The Cardputer ledger accumulates awards across games until **New Session**.
- A reboot restores identities, names, avatars, cumulative scores, and the selected
  game, but starts that game in a fresh lobby with phone scores at zero.
- History records never change. Restoring one archives the current nonempty session,
  creates a new active session with `restored_from`, restores cumulative standings,
  and opens the prior selected game in a fresh lobby.
- Chat, Draw strokes, transient events, raw resume tokens, and an in-progress round
  are never persisted.

With microSD, config and active records alternate between CRC-checked A/B slots and
history is retained without automatic pruning:

```text
/hotspot-arcade/config.a
/hotspot-arcade/config.b
/hotspot-arcade/active.a
/hotspot-arcade/active.b
/hotspot-arcade/history/S00000001.ha
/hotspot-arcade/history/index.bin
/hotspot-arcade/migration-v2.done
```

Without microSD, NVS keeps settings and a bounded active-session fallback. Abrupt
power loss can lose up to 30 seconds of recent changes, and history is unavailable.
The device never silently deletes a nonempty active session when archiving fails; a
second explicit discard confirmation is required.

## Games

The fifteen phone-driven games are Trivia, Would You Rather, Word Scramble,
Spectrum, Kiss Marry Kill, Reaction Duel, Connect Four, Tic-Tac-Toe, Dots & Boxes,
Reversi, Draw and Guess, Pong, Guess the Color, Battleship, and Chess. Content and
phone UI are available in English, German, and the currently supplied Portuguese
Brazil packs, with per-game English fallback where a translation is absent.

## Developer setup

The supported host baseline is macOS Apple Silicon or Linux x64. No PlatformIO,
Docker, Playwright, or `jq` is required.

```sh
nvm install 24.19.0
nvm use 24.19.0
brew install arduino-cli esptool emscripten actionlint
gh auth login -h github.com                  # needed only for GitHub release work
git clone https://github.com/kaini-industries/hotspot-arcade.git ../hotspot-arcade

tools/bootstrap.sh                           # project-local Arduino core/libraries
tools/doctor.sh
tools/build.sh
```

`tools/bootstrap-node.sh` is the checksum-verifying alternative to `nvm` and is
what CI uses for the locked Linux x64 Node archive.

`.nvmrc` and `tools/toolchain.lock.json` pin Node 24.19.0, Arduino CLI 1.5.1,
ESP32 core 3.3.11, esptool 5.3.1, Emscripten 6.0.2, actionlint 1.7.12,
Syft 1.50.0, Cosign 3.0.6, the CI GitHub CLI 2.93.0, M5Cardputer 1.1.1,
M5Unified 0.2.19, M5GFX 0.2.26, every resolved transitive Arduino dependency,
and downloaded archive hashes. Arduino data and downloads stay inside
`.cache/arduino`; bootstrapped host tools stay under `.tools`. Syft and Cosign are
optional on developer Macs; CI installs their checksum-pinned Linux binaries with
`tools/bootstrap-ci-tools.sh`.

Cardputer-Adv support keeps the tagged M5GFX 0.2.26 archive and applies only the
exact fix from [M5GFX PR #233](https://github.com/m5stack/M5GFX/pull/233), upstream
commit [`5f8a783`](https://github.com/m5stack/M5GFX/commit/5f8a783f7dbc07e8ce5c19cf8779829d1eefcde1).
The patch file, preimage, patched result, commit, and merge commit are all verified
from `tools/toolchain.lock.json`; the release SBOM records the patch as a separate
source package with a `PATCH_FOR` relationship to M5GFX 0.2.26. The build does not
vendor or track the rest of M5GFX's development branch.

Run the local gates with:

```sh
tools/test-native.sh
tools/test-native.sh --tsan
python3 -m unittest discover -s tests -p 'test_*.py'
node --test tests/*.test.mjs
actionlint
shellcheck tools/*.sh
```

The native suite uses ASan/UBSan by default and TSan for the bounded asynchronous
queue on Linux. `tools/build-release-candidate.sh` performs the locked image,
package, SBOM, manifest, checksum, and provenance gates with
`SOURCE_DATE_EPOCH` set.
It requires an explicit tag, rejects a dirty checkout, marks `-rc.N` builds as
unpublishable candidates, and requires a final tag to resolve to the packaged
commit.

## Architecture

The implementation has five logical modules:

1. **Runtime and transport** — `hotspot-arcade-cardputer.ino`, `ha_device.h`,
   `ha_network_policy.h`, and `ha_runtime_types.h` own board classification, Wi-Fi,
   DNS, WebSockets, admission, AP lifecycle, flow control, and loop scheduling.
2. **Engine and host mirror** — `vendor/engine/`, `ha_host.h`, and
   `ha_event_format.h` own authoritative game state, stable identities, cumulative
   awards, bounded typed events, and UI snapshots.
3. **Content** — `ha_content.h`, generated metadata/assets, and the explicit content
   manifest implement transactional locale/game content replacement.
4. **Recovery** — `ha_config.h`, `ha_active_nvs.h`, and `ha_history.h` implement
   redundant SD/NVS records, migration, immutable archives, and restore.
5. **Host presentation** — `ha_ui.h`, `ha_diagnostics.h`, and `ha_async_queue.h`
   implement the 240×135 UI, 8-bit/direct-draw fallback, diagnostics, and bounded
   loop-task work queues.

Engine/session/mirror state is protected by one recursive mutex; UI and persistence
operate on snapshots, and display, speaker, SD, DNS, and AP I/O stay outside that
critical section. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Upstream and vendoring

Nothing under `vendor/` is edited downstream. Protocol, engine, simulator, and phone
client changes are developed in the sibling Kaini-maintained source checkout first,
then copied from committed Git objects only. That repository remains a fork of Tarik
Caramanico's original project so selected changes can still be proposed upstream:

```sh
node tools/sync-upstream.mjs \
  --repo ../hotspot-arcade \
  --commit aad6e8ffa03a125aa4d6be14030a3f887d5cde05
node tools/gen-assets.mjs
```

The sync requires clean checkouts, the reviewed canonical Kaini source URL, and an
explicit 40-character commit. It replaces all vendor destinations atomically and
writes a deterministic per-file hash inventory in `UPSTREAM.lock.json`. Releases
remain pinned to a reviewed commit in `kaini-industries/hotspot-arcade`, independent
of whether a corresponding contribution has merged into the original project.

## Release process

`VERSION` is the single release version source and is currently `0.6.0`. CI is
read-only. For the current hardware-support phase:

1. Build/test `0.6.0-rc.2` with workflow dispatch and no publication.
2. Verify that the manifest marks one image compatible with board IDs 14 and 24.
3. Complete the full Cardputer and Cardputer-Adv
   [hardware acceptance matrix](docs/HARDWARE_ACCEPTANCE.md) using that exact image.
4. Attach the completed evidence and final measured image/DRAM/heap values to the
   release decision.

M5Burner catalog identity and publication remain a separate, explicitly deferred
cutover. The deterministic M5Burner-format archive may still be built and inspected,
but do not create a final tag or publish the catalog entry as part of this hardware
qualification work.

## License and attribution

The project is MIT-licensed; see [LICENSE](LICENSE). Tarik Caramanico's upstream
engine/client/content and `genkigenki`'s original Cardputer work remain attributed.
Vendored AsyncTCP and ESPAsyncWebServer retain their LGPL-3.0 notices and license
texts; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
