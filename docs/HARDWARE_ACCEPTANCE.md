# Cardputer and Cardputer-Adv hardware acceptance

This matrix is the physical release gate for the universal Hotspot Arcade image.
Automated CI, successful compilation, and catalog metadata do not substitute for
these checks. A candidate is supported only when the same artifact digest passes on
one original M5Stack Cardputer and one Cardputer-Adv.

M5Burner catalog creation, identity migration, and publication are deliberately out
of scope. Use a CI release candidate, M5Launcher, or direct esptool flashing; do not
record a catalog listing as acceptance evidence.

## Result notation

Record every result as `PASS`, `FAIL`, `BLOCKED`, or `N/A`, plus a link or filename
for evidence. Blank cells mean **not run**, never an implicit pass. Any failure in a
required row blocks release. Keep serial logs from boot through each fault test.

## Candidate and bench record

| Field | Value |
| --- | --- |
| Version / candidate tag | |
| Git commit (40 hex) | |
| CI run URL | |
| `SOURCE_DATE_EPOCH` | |
| App-image SHA-256 | |
| Full-image SHA-256 | |
| Manifest SHA-256 | |
| Original Cardputer serial / revision | |
| Cardputer-Adv serial / revision | |
| microSD make / size / filesystem | |
| iOS model / OS / browser | |
| Android model / OS / browser | |
| Tester / date / location | |

Required bench equipment:

- One original Cardputer and one Cardputer-Adv in known-good physical condition.
- A dedicated, disposable microSD card. Fault tests can corrupt it.
- Eleven simultaneous browser clients. Laptop automation may provide most clients,
  but captive-portal behavior must also be checked on a real iOS phone and Android
  phone.
- A current-limited USB supply, data cable, and a host with the locked esptool.
- A passive test fixture or compatible accessory that pulls Cardputer-Adv G5/G6
  high without emulating the VAMeter I2C addresses. This exercises the upstream
  detection regression fixed by the reviewed M5GFX patch.

## 1. Artifact and provenance gate

Complete these once for the exact files used on both devices.

| Check | Expected | Result | Evidence |
| --- | --- | --- | --- |
| Verify `SHA256SUMS` | Every downloaded artifact matches. | | |
| Inspect `build-manifest.json` | `singleImage` is `true`. | | |
| Inspect compatible devices | Cardputer / `M5Cardputer` / ID 14 and Cardputer-Adv / `M5CardputerADV` / ID 24 appear exactly once. | | |
| Compare device inputs | Both devices use the exact same app-image hash, and both use the exact same full-image hash. | | |
| Validate SPDX SBOM | M5GFX 0.2.26 and the reviewed `5f8a783…` patch are separate packages joined by `PATCH_FOR`. | | |
| Validate ESP metadata | Chip, partition offsets, filenames, trimmed full-image boundary, and declared 8 MiB flash layout pass the locked release validator. | | |
| Confirm clean provenance | Manifest commit matches the reviewed clean source commit and CI attestation. | | |

## 2. Installation and runtime identity

Exercise both layouts on both devices. Preserve the serial log showing the detected
model and board ID.

| Device | Layout | Install path | Expected boot identity | Result | Evidence |
| --- | --- | --- | --- | --- | --- |
| Original Cardputer | App image | M5Launcher / `0x170000` | `Cardputer (M5 board id 14)` | | |
| Original Cardputer | Full image | esptool / `0x0` | `Cardputer (M5 board id 14)` | | |
| Cardputer-Adv | App image | M5Launcher / `0x170000` | `Cardputer Advance (M5 board id 24)` | | |
| Cardputer-Adv | Full image | esptool / `0x0` | `Cardputer Advance (M5 board id 24)` | | |

For manual download mode, first try a normal USB connection. If it does not appear:

- Original Cardputer: hold `G0` while connecting USB-C.
- Cardputer-Adv: switch power off, hold `G0`, switch power on, and release `G0`
  after the USB download port appears.

After each install, confirm the Diagnostics screen shows the same model and numeric
ID as serial. An image reporting any other identity is a failure even if the UI
appears to work.

### Cardputer-Adv detection regression

| Configuration | Expected | Result | Evidence |
| --- | --- | --- | --- |
| Cardputer-Adv with no external fixture | Board ID 24; display, keyboard, audio, and SD initialize. | | |
| Cardputer-Adv with passive G5/G6 pull-ups attached before boot | Still board ID 24; the unit is not misidentified as Cardputer or VAMeter. | | |
| Five cold boots with G5/G6 pull-ups | All five report ID 24 and reach the healthy dashboard. | | |
| Remove fixture and cold boot again | ID 24 and all peripherals remain healthy. | | |

The native classifier test must also demonstrate that IDs other than 14 and 24 are
unsupported. Do not flash unrelated M5 hardware merely to exercise the fatal path.

## 3. Device peripherals and host UI

Run every row independently on each device.

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| Display rotation, color, backlight, and direct-draw fallback are legible | | | |
| Every physical keyboard key registers exactly once | | | |
| `G`, `L`, `H`, `C`, `S`, `D`, `N`, `P`, `E`, `R`, `Esc`, arrows/punctuation, and `Enter` perform the documented action | | | |
| Nickname/SSID entry handles editing, limits, and UTF-8 safely | | | |
| Internal speaker plays AP/join/leave sounds with headphones and external audio accessories removed | | | |
| Audio off/low/high persists across reboot | | | |
| Battery level is plausible and the unit runs from battery | | | |
| Charging and USB-powered operation remain stable | | | |
| Diagnostics identifies model/ID and updates heap, queues, rate limits, loop, lock, SD, and generation counters | | | |

## 4. Network, identity, and game behavior

Run the following on each device. Use the same mix of automated clients and real
phones where practical.

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| Open AP and captive page appear on iOS | | | |
| Open AP and captive page appear on Android | | | |
| Unknown identity requires the displayed six-digit code | | | |
| Known identity resumes without re-entering the code | | | |
| Five wrong attempts/minute trigger per-client throttling | | | |
| Thirty wrong attempts/minute trigger global lockout for 60 seconds | | | |
| Ten authenticated clients remain connected; an eleventh receives `full` | | | |
| Two pending hellos do not evict authenticated clients; excess pending sockets are refused | | | |
| A socket missing the five-second hello deadline is closed | | | |
| Duplicate resume-token connection takes over and closes the older socket | | | |
| Reconnect at 119 seconds retains exact PID/game state | | | |
| Reconnect after 121 seconds gets a fresh PID/game score but keeps cumulative score | | | |
| Offline players cannot be challenged and do not satisfy ordinary quorum | | | |
| Affected 1v1/critical rounds pause during reconnect grace and resolve once | | | |
| Phone score resets per selected game; host score remains cumulative | | | |
| All 15 games open and accept a minimal valid round/action | | | |
| Five concurrent slots work in Pong, Battleship, and Chess | | | |
| Malformed numeric/Draw input is rejected without crash or invalid relay | | | |
| Chat, emoji, Draw, and general-control rate limits increment diagnostics | | | |
| Slow-client pressure coalesces/drops/closes only the affected client, which can resume | | | |

## 5. Planned AP downtime

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| SSID rename announces pause and checkpoints before transport stops | | | |
| Successful rename retains exact engine state, roster, and cumulative scores | | | |
| Failed rename returns to the prior SSID without losing state | | | |
| Manual AP off remains paused indefinitely | | | |
| AP restart gives required identities a ten-minute reconnect window | | | |
| Session resumes automatically when all required identities return | | | |
| Host can resume/end early when required identities do not return | | | |
| Logical game and disconnect clocks do not advance while AP is deliberately off | | | |
| Ten-minute expiry resolves missing participants exactly once | | | |
| Connected and resumed clients receive the active locale after restart | | | |

## 6. microSD, recovery, and history

Use the dedicated test card and retain copies of its contents after each failure.
Perform active-write interruption tests with a controlled power switch; never use a
card containing unrelated data.

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| Blank supported microSD initializes the version-2 directory layout | | | |
| Config and active A/B slots alternate and retain the previous valid generation | | | |
| Reboot restores identities and cumulative standings into a fresh game lobby | | | |
| SD absent boots in degraded NVS mode with no history | | | |
| SD removed after boot degrades safely and increments failure diagnostics | | | |
| Full SD does not erase the active session; discard requires second confirmation | | | |
| Truncated/corrupt newest config slot falls back to the older valid slot | | | |
| Truncated/corrupt newest active slot falls back to the older valid slot or NVS by generation rules | | | |
| Interrupted temporary archive never appears as a valid immutable history record | | | |
| Interrupt at each config/active write, flush, reopen, verify, and promote phase recovers a valid prior or new generation | | | |
| Missing/corrupt `index.bin` rebuilds from immutable history | | | |
| History browses newest-first and shows all standings | | | |
| Restore archives current nonempty play, creates `restored_from`, restores cumulative standings, and opens a fresh lobby | | | |
| Legacy `config.txt`, `current.txt`, and `history.txt` migrate once and preserve `.v1.imported` originals | | | |
| Equal-generation SD/NVS conflict chooses SD; otherwise highest valid generation wins | | | |
| Chat, Draw strokes, raw resume tokens, and transient events are absent from persisted records | | | |

## 7. Locale and repeated-use reliability

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| English, German, and supplied Portuguese-Brazil content load with expected fallback | | | |
| Existing and resumed phones receive every locale change | | | |
| Changing locale ends only the current round and preserves per-game/cumulative score contracts | | | |
| 500 language/game cycles complete | | | |
| Post-warm-up heap degradation after 500 cycles is under 5% | | | |

## 8. OTA health and failure behavior

| Check | Original Cardputer | Cardputer-Adv | Evidence / notes |
| --- | --- | --- | --- |
| Healthy OTA candidate is confirmed only after storage/content/UI/AP and first draw succeed | | | |
| Deliberately failed critical initialization stays rollback-eligible | | | |
| SD mount failure is degraded, not fatal, and a pending healthy image can still confirm | | | |
| Unsupported-board classifier path never reaches OTA confirmation (native/instrumented evidence) | | | |

## 9. Two-hour ten-client soak and budgets

Run mixed Draw and Pong activity with ten authenticated clients for at least two
hours on each device. Sample once per minute and attach the raw log, not only the
summary.

| Metric / gate | Required | Original Cardputer | Cardputer-Adv |
| --- | --- | --- | --- |
| Soak duration | At least 2 hours | | |
| Authenticated clients | 10 continuously, with scripted reconnects | | |
| Eleventh client | Consistently rejected as `full` | | |
| Static DRAM | No more than 90 KiB | | |
| App partition headroom | At least 300 KiB | | |
| Startup free heap | At least 200 KiB | | |
| Minimum free heap during soak | At least 120 KiB | | |
| Minimum largest free block | At least 32 KiB | | |
| Normal engine-lock hold | Under 10 ms | | |
| Loop gap outside forced persistence | Under 50 ms | | |
| Crash, reset, watchdog, corrupt state, or cross-client eviction | None | | |

## Final sign-off

| Gate | Status | Approver / evidence |
| --- | --- | --- |
| Automated CI and reproducibility | | |
| Original Cardputer full matrix | | |
| Cardputer-Adv full matrix | | |
| Cardputer-Adv G5/G6 detection regression | | |
| Same-image digest proof | | |
| Resource budgets on both devices | | |
| iOS and Android captive portals | | |
| Two-hour ten-client soaks | | |
| All blocking failures resolved and rerun | | |

Only after every required row is `PASS` should the candidate be described as working
on both products. M5Burner publication remains a separate later decision.
