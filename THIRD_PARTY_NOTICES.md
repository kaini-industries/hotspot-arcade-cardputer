# Third-party notices

Hotspot Arcade Cardputer is MIT-licensed, with attribution retained for Tarik
Caramanico's upstream Hotspot Arcade engine/client/content and genkigenki's
original Cardputer port. The exact upstream revision and file hashes are
recorded in `UPSTREAM.md` and `UPSTREAM.lock.json`.

The vendored networking libraries are separately licensed under LGPL-3.0:

- AsyncTCP — see `vendor/libs/AsyncTCP/LICENSE`.
- ESPAsyncWebServer — see `vendor/libs/ESPAsyncWebServer/LICENSE`.

M5GFX is Copyright (c) 2021 M5Stack and is licensed under the MIT License. The
build uses the tagged M5GFX 0.2.26 archive and applies only the Cardputer-Adv
detection fix from upstream M5GFX PR #233, commit
`5f8a783f7dbc07e8ce5c19cf8779829d1eefcde1` (merged as
`701a8b4d23212644ddd65940ccc9f59107248386`). The exact local patch, patch hash,
target preimage hash, and patched-result hash are recorded in
`tools/toolchain.lock.json`; no unreleased M5GFX development snapshot is vendored.

Those license texts remain in the source and release provenance. Except for the
reviewed M5GFX patch above, Arduino core and M5 library dependencies are not copied
into this repository; their exact versions, archives, checksums, and dependency
relationships are recorded in `tools/toolchain.lock.json` and represented in the
release SPDX SBOM. The SBOM records the reviewed M5GFX change as its own
MIT-licensed source package related to the tagged M5GFX package with SPDX
`PATCH_FOR`.
