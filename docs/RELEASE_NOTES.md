### Unreleased — Protocol v22 and twenty games

- Firmware protocol v22 adds Secrets, Fill the Blank, Werewolf, Spyfall, and Draw a
  Monster for 20 games total. IDs 1-20 are manifest-authoritative protocol keys and
  are treated as sparse rather than inferred from a numeric range.
- Compile-time guards preserve exactly ten authenticated phone players and five
  concurrent matches in every 1v1 game.
- Only the active game's typed content bank is loaded. Game/locale replacement is
  count-checked and atomic; a missing `de` or `pt-br` game pack set falls back wholly
  to `en` without mixing packs or changing the requested phone UI locale. Spectrum's
  new Wild Card pack remains English-only.
- Content allocation is PSRAM-first with internal-heap fallback and a required 64 KiB
  internal reserve. The Cardputer remains the game-selection authority; phone game
  proposals are denied without mutating the active bank.
- Cumulative Cardputer session standings, per-game phone scores, versioned microSD history, power-loss recovery, and restore into a fresh lobby.
- Planned access-point pauses preserve play state and provide a separate ten-minute
  return window without consuming the ordinary two-minute reconnect grace.
- Protocol v22 replaces generic engine event/result callbacks with typed host events;
  host-directed finished art remains transient, and the localized 24-entry typed log
  is not persisted.
- Generated assets enforce a 72 KiB compressed-web ceiling. The reviewed v22 web
  bundle is 68,137 compressed bytes; the clean release candidate revalidates that
  exact payload before packaging.

### v0.6.0 — Durable ten-player sessions

- One universal firmware image for the original M5Stack Cardputer and
  Cardputer-Adv, with runtime model diagnostics and an unsupported-board stop.
- Ten authenticated phone players with stable browser identities, reconnect and takeover support, and a six-digit host join code.
- Cumulative Cardputer session standings, per-game phone scores, versioned microSD history, power-loss recovery, and restore into a fresh lobby.
- Planned access-point pauses preserve play state and provide a ten-minute return window.
- Deterministic, checksum-locked builds with validated ESP32-S3 images, release manifests, provenance, and deterministic packaging.
- Scrollable host history/events, storage and connection status, diagnostics, generated game/language metadata, and hardened bounded input handling.
- The complete Cardputer host interface follows the German language setting, with
  allocation-free English fallback for other untranslated host locales.

### v0.5.0 — German, top to bottom + settings overhaul

- 🌍 **Play fully in German** — a language switch in Settings (English / Deutsch). The **content**, **phone interface**, and **Cardputer host screen** are German — buttons, prompts, game names, host menus, history, diagnostics, and status text. Pick a language once; the host streams the content, relays the UI language to every phone, and switches its own screen, with English fallback for anything untranslated.
- ♟️ **Chess** — a 15th game (1v1, full FIDE rules with a blitz clock), from upstream.
- 🔤 **UTF-8-safe games** — Word Scramble and Draw handle umlauts and ß correctly.
- 💾 **Settings on the SD card** — SSID, audio and language survive a reboot and even a full-chip reflash.
- 🎛️ **Redesigned settings screen** — option pills, a language switch with `‹ ›` arrows, `,`/`/` to change a value in place.
- 🎨 **New look** — the host screen matches the phone client's black / orange / white palette.
- 🎮 **The default network** carries a game-pad icon so it stands out in the Wi-Fi list.

Fifteen games now.

### Install

Download the reviewed candidate or release artifacts and verify `SHA256SUMS`. The
same `hotspot-arcade-cardputer.ino.bin` and `hotspot-arcade-cardputer.full.bin`
support both Cardputer models; there is no separate Advance build. M5Burner catalog
identity and publication are intentionally deferred during two-device hardware
qualification.

**Or flash by hand** (keeps M5Launcher):
```
esptool --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x170000 hotspot-arcade-cardputer.ino.bin
```

Use the completed [hardware acceptance matrix](HARDWARE_ACCEPTANCE.md) as the release
gate. Full-image installation, model-specific download-mode steps, and recovery are
documented in the README.
