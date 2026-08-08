# Third-party notices

Hotspot Arcade Cardputer is MIT-licensed, with attribution retained for Tarik
Caramanico's upstream Hotspot Arcade engine/client/content and genkigenki's
original Cardputer port. The exact upstream revision and file hashes are
recorded in `UPSTREAM.md` and `UPSTREAM.lock.json`.

The vendored networking libraries are separately licensed under LGPL-3.0:

- AsyncTCP — see `vendor/libs/AsyncTCP/LICENSE`.
- ESPAsyncWebServer — see `vendor/libs/ESPAsyncWebServer/LICENSE`.

Those license texts remain in the source and release provenance. Arduino core
and M5 library dependencies are not copied into this repository; their exact
versions, archives, checksums, and dependency relationships are recorded in
`tools/toolchain.lock.json` and represented in the release SPDX SBOM.
