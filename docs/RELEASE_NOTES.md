### v0.6.0 — Durable ten-player sessions

- One universal firmware image for the original M5Stack Cardputer and
  Cardputer-Adv, with runtime model diagnostics and an unsupported-board stop.
- Ten authenticated phone players with stable browser identities, reconnect and takeover support, and a six-digit host join code.
- Cumulative Cardputer session standings, per-game phone scores, versioned microSD history, power-loss recovery, and restore into a fresh lobby.
- Planned access-point pauses preserve play state and provide a ten-minute return window.
- Deterministic, checksum-locked builds with validated ESP32-S3 images, release manifests, provenance, and deterministic packaging.
- Scrollable host history/events, storage and connection status, diagnostics, generated game/language metadata, and hardened bounded input handling.

### v0.5.0 — German, top to bottom + settings overhaul

- 🌍 **Play fully in German** — a language switch in Settings (English / Deutsch). Both the **content** (all six content games, 32 packs) **and the phone interface** are German — buttons, prompts, in-game text, the lot. Pick a language once; the host streams the content and relays the UI language to every phone, English fallback for anything untranslated.
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
