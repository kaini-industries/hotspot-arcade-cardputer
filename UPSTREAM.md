# Upstream

Everything under `vendor/` is copied from one committed Git object in
[tarikbc/hotspot-arcade](https://github.com/tarikbc/hotspot-arcade) (MIT, Tarik Caramanico). Third-party libraries retain their own license files. No file in `vendor/` is edited downstream.

| | |
| --- | --- |
| repository | `https://github.com/tarikbc/hotspot-arcade` |
| commit | `ea2a77cb7b68fd3b981afdd9f91a64180e1e8b17` |
| describe | `ea2a77c` |
| source tree SHA-256 | `4b4ac7283aea860411283b2fcb52e009b79670b35d2f00fe254bee5b33f3126d` |
| web bundle | 1 file(s) |
| content packs | 70 pack(s) |
| full file inventory | `UPSTREAM.lock.json` |

Refresh from a clean upstream and downstream checkout:

```sh
node tools/sync-upstream.mjs --repo ../hotspot-arcade --commit <40-character-sha>
node tools/gen-assets.mjs
```

Review `git diff -- vendor UPSTREAM.md UPSTREAM.lock.json` before committing.
